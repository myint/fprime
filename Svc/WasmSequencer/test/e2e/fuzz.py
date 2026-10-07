#!/usr/bin/env python3
"""Differential fuzzing of WasmSequencer, with CmdSequencer as the oracle.

Each case is a random `.seq` sequence over the Ref deployment's commands and a
random scenario to run it in: the rate group, the clock's start, how long the
dispatcher takes to answer and which command it fails or never answers,
cancels and reruns. The sequence is compiled twice, from the legacy `.seq`
syntax both tools read:

* by `fprime-seqgen` into a `.bin`, which Svc::CmdSequencer runs, and
* by `fprime-wasm seq` (https://github.com/myint/fprime-wasm, branch
  `myint-seq`) into a `.wasm`, which Svc::WasmSequencer runs.

The Svc_WasmSequencer_fuzz executable runs both on identical simulated
benches. WasmSequencer must do exactly what CmdSequencer does: the same
commands, byte for byte, at the same times, with the same outcome. The one
expected difference is a tick of delay on an absolute time already past.

A sequence one compiler accepts and the other rejects is a finding too. Some
cases are generated invalid on purpose (an unknown command, a value out of
range, a string too long, a malformed time), which both must reject.

Findings are minimized and written to the work directory's `findings/`, each
with the command to replay it.

Usage (from an F Prime virtual environment, with Rust installed):

    Svc/WasmSequencer/test/e2e/fuzz.py --cases 1000 --seed 1
    Svc/WasmSequencer/test/e2e/fuzz.py --replay build-fprime-wasm-seq-e2e/fuzz/findings/<finding>
"""

import argparse
import contextlib
import datetime
import io
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_e2e as e2e  # noqa: E402

FUZZ_TARGET = "Svc_WasmSequencer_fuzz"

US = 1_000_000

#: Fw::CmdResponse constants other than OK
FAILURES = {1: "INVALID_OPCODE", 2: "VALIDATION_ERROR", 3: "FORMAT_ERROR", 4: "EXECUTION_ERROR", 5: "BUSY"}

#: Rate group periods, microseconds
TICKS = [1000, 10_000, 100_000, 1_000_000]

#: Clock ticks a run may take, at most; keeps each case fast
MAX_TICKS = 200_000

#: First second of 2026 and of 2027, UTC
YEAR_START = 1_767_225_600
YEAR_END = 1_798_761_600

#: CmdSequencer and WasmSequencer both give up on a command after this long
COMMAND_TIMEOUT_US = 60 * US

#: Characters a generated string is made of
PRINTABLE = [chr(c) for c in range(0x20, 0x7F)]


# ----------------------------------------------------------------------
# The dictionary
# ----------------------------------------------------------------------


class Dictionary:
    """The commands and types of a deployment's JSON dictionary."""

    def __init__(self, path):
        with open(path) as file:
            data = json.load(file)
        self.commands = data["commands"]
        self.types = {definition["qualifiedName"]: definition for definition in data["typeDefinitions"]}
        names = [command["name"] for command in self.commands]
        self.names = names
        # Every trailing part of a command's name that names it alone
        self.suffixes = {}
        for name in names:
            parts = name.split(".")
            self.suffixes[name] = [
                ".".join(parts[i:])
                for i in range(1, len(parts))
                if sum(1 for other in names if other == ".".join(parts[i:]) or other.endswith("." + ".".join(parts[i:])))
                == 1
            ]

    def resolve(self, reference):
        """The definition `reference` names, through any aliases."""
        while reference.get("kind") == "qualifiedIdentifier":
            reference = self.types[reference["name"]]
            if reference["kind"] == "alias":
                reference = reference["underlyingType"]
        return reference


# ----------------------------------------------------------------------
# Generating sequences
# ----------------------------------------------------------------------


def fraction(micros, rng):
    """The digits after the point for `micros` microseconds: six, or fewer when the rest are zeros."""
    digits = f"{micros:06d}"
    return digits.rstrip("0") if rng.random() < 0.5 else digits


def seqgen_reads(tag):
    """The (descriptor, seconds, useconds) fprime-seqgen encodes for time tag `tag`.

    fprime-seqgen goes through a float of seconds, truncating, so a time such
    as R00:00:28.495765 comes out a microsecond short. This repeats its
    arithmetic, so the generator only writes times it encodes faithfully.
    """
    text = tag[1:]
    if tag[0] == "R":
        when = datetime.datetime.strptime(text, "%H:%M:%S.%f" if "." in text else "%H:%M:%S")
        delta = datetime.timedelta(
            hours=when.hour, minutes=when.minute, seconds=when.second, microseconds=when.microsecond
        ).total_seconds()
        descriptor = 1
    else:
        when = datetime.datetime.strptime(text, "%Y-%jT%H:%M:%S.%f" if "." in text else "%Y-%jT%H:%M:%S")
        delta = (when - datetime.datetime(1970, 1, 1)).total_seconds()
        descriptor = 0
    seconds = int(delta)
    return descriptor, seconds, int((delta - seconds) * 1000000)


class Generator:
    """Random `.seq` text in the syntax both fprime-seqgen and fprime-wasm read."""

    def __init__(self, dictionary, rng):
        self.dictionary = dictionary
        self.rng = rng

    # Values ------------------------------------------------------------

    def integer(self, size, signed, invalid=False):
        low, high = (-(1 << (size - 1)), (1 << (size - 1)) - 1) if signed else (0, (1 << size) - 1)
        rng = self.rng
        if invalid:
            value = rng.choice([high + 1, high + rng.randint(2, 1000)] + ([low - 1] if signed or rng.random() < 0.5 else [-1]))
        else:
            value = rng.choice(
                [
                    low,
                    high,
                    0,
                    1,
                    -1 if signed else 2,
                    rng.randint(low, high),
                    rng.randint(max(low, -100), min(high, 100)),
                ]
            )
        form = rng.random()
        if value >= 0 and form < 0.15:
            return hex(value) if rng.random() < 0.5 else hex(value).upper().replace("0X", "0x")
        if value >= 0 and form < 0.2:
            return "+" + str(value)
        if form < 0.25 and abs(value) >= 1000:
            return f"{value:_}"
        return str(value)

    def floating(self, size):
        rng = self.rng
        largest = 3.4028234663852886e38 if size == 32 else 1.7976931348623157e308
        smallest = 1.1754943508222875e-38 if size == 32 else 2.2250738585072014e-308
        kind = rng.random()
        if kind < 0.3:
            # A short decimal
            value = round(rng.uniform(-1000, 1000), rng.randint(0, 4))
            text = repr(value)
        elif kind < 0.5:
            exponent = rng.randint(-37, 37) if size == 32 else rng.randint(-300, 300)
            text = f"{rng.uniform(1, 9.999):.{rng.randint(0, 8)}f}e{exponent}"
            if rng.random() < 0.5:
                text = "-" + text
        elif kind < 0.7:
            text = repr(rng.choice([0.0, -0.0, 1.0, -1.0, 0.1, 0.5, largest, -largest, smallest, -smallest]))
        elif kind < 0.8:
            text = str(rng.randint(-100000, 100000))
        elif kind < 0.9:
            text = "." + str(rng.randint(0, 99999))
        else:
            text = repr(rng.uniform(-1e6, 1e6))
        return text

    def string(self, size, invalid=False):
        rng = self.rng
        quote = '"' if rng.random() < 0.8 else "'"
        alphabet = [c for c in PRINTABLE if c != quote]
        if invalid:
            length = size + rng.randint(1, 5)
        else:
            length = rng.choice([0, 1, size, rng.randint(0, size), rng.randint(0, min(size, 12))])
        text = "".join(rng.choice(alphabet) for _ in range(length))
        # A backslash before the closing quote reads as an escape to one tool and not the other
        while text.endswith("\\"):
            text = text[:-1] + "x"
        return quote + text + quote

    def value(self, reference, invalid=False):
        """A literal for `reference`; `invalid` makes it one both compilers must reject."""
        rng = self.rng
        definition = self.dictionary.resolve(reference)
        kind = definition["kind"]
        if kind == "integer":
            return self.integer(definition["size"], definition["signed"], invalid)
        if kind == "float":
            if invalid:
                # fprime-seqgen takes 1e309 as infinity, and 1.5x, 1e and nan as numbers
                return rng.choice((["3.5e38", "-1e39"] if definition["size"] == 32 else []) + ["1..5", "--1.0"])
            return self.floating(definition["size"])
        if kind == "bool":
            if invalid:
                return rng.choice(["2", "truth", "-1"])
            return rng.choice(["true", "false", "true", "false", "True", "FALSE"])
        if kind == "string":
            return self.string(definition["size"], invalid)
        if kind == "enum":
            if invalid:
                return "NOT_A_CONSTANT"
            return rng.choice(definition["enumeratedConstants"])["name"]
        if kind == "array":
            items = [self.value(definition["elementType"]) for _ in range(definition["size"])]
            if invalid:
                items[rng.randrange(len(items))] = self.value(definition["elementType"], invalid=True)
            return "[" + ", ".join(items) + ("," if rng.random() < 0.1 else "") + "]"
        if kind == "struct":
            members = sorted(definition["members"].items(), key=lambda item: item[1]["index"])
            if rng.random() < 0.2:
                rng.shuffle(members)
            spoiled = rng.randrange(len(members)) if invalid else -1
            texts = []
            for position, (name, member) in enumerate(members):
                if "size" in member:
                    # A member array: spoil one of its elements
                    bad = rng.randrange(member["size"]) if position == spoiled else -1
                    text = "[" + ", ".join(self.value(member["type"], invalid=(i == bad)) for i in range(member["size"])) + "]"
                else:
                    text = self.value(member["type"], invalid=(position == spoiled))
                texts.append(f"{name}: {text}")
            return "{" + ", ".join(texts) + "}"
        raise ValueError(f"cannot generate a {kind}")

    # Lines -------------------------------------------------------------

    def relative(self):
        """A relative wait, microseconds, and its time tag."""
        rng = self.rng
        kind = rng.random()
        if kind < 0.45:
            whole = 0
        elif kind < 0.75:
            whole = 0
        elif kind < 0.95:
            whole = rng.randint(1, 30)
        else:
            whole = rng.randint(30, 600)
        hours, rest = divmod(whole, 3600)
        tag = f"R{hours:02d}:{rest // 60:02d}:{rest % 60:02d}"
        if kind < 0.45:
            return 0, tag + (rng.choice([".0", ".000000"]) if rng.random() < 0.1 else "")
        for _ in range(50):
            micros = rng.choice([rng.randrange(1, US), rng.randrange(1, 1000) * 1000, rng.randrange(1, 10) * 100_000])
            if kind < 0.75 or rng.random() < 0.7:
                candidate = tag + "." + fraction(micros, rng)
                if seqgen_reads(candidate) == (1, whole, micros):
                    return whole * US + micros, candidate
        return whole * US, tag

    def absolute(self, start):
        """An absolute time, microseconds since 1970, and its time tag: within a few minutes of `start`."""
        rng = self.rng
        for _ in range(50):
            moment = start + rng.randint(-300 * US, 600 * US)
            if rng.random() < 0.4:
                moment -= moment % US
            seconds, micros = divmod(moment, US)
            when = datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc)
            tag = "A" + when.strftime("%Y-%jT%H:%M:%S")
            if micros:
                tag += "." + fraction(micros, rng)
            if seqgen_reads(tag) == (0, seconds, micros):
                return moment, tag
        moment = start - start % US
        when = datetime.datetime.fromtimestamp(moment // US, datetime.timezone.utc)
        return moment, "A" + when.strftime("%Y-%jT%H:%M:%S")

    def command_line(self, start, invalid=None):
        """One command line; returns (text, intent) where intent is (descriptor, seconds, useconds)."""
        rng = self.rng
        commands = self.dictionary.commands
        if invalid == "argument":
            commands = [command for command in commands if command["formalParams"]]
        command = rng.choice(commands)
        name = command["name"]
        if self.dictionary.suffixes[name] and rng.random() < 0.3:
            mnemonic = rng.choice(self.dictionary.suffixes[name])
        else:
            mnemonic = name
        if invalid == "command":
            mnemonic = name + "_NOT_A_COMMAND"

        if rng.random() < 0.75:
            wait, tag = self.relative()
            intent = (1, wait // US, wait % US)
        else:
            moment, tag = self.absolute(start)
            intent = (0, moment // US, moment % US)
        if invalid == "time":
            tag = rng.choice(["R00:00:60", "R24:00:00", "R0:0:0", "R00:60:00", "A2026-400T00:00:00", "A2026-001T25:00:00"])

        params = command["formalParams"]
        spoiled = rng.randrange(len(params)) if invalid == "argument" else -1
        arguments = [self.value(param["type"], invalid=(index == spoiled)) for index, param in enumerate(params)]

        separator = rng.choice([" ", " ", ", ", ","])
        text = tag + " " + mnemonic
        if arguments:
            text += (" " if rng.random() < 0.8 else ", ") + separator.join(arguments)
        if rng.random() < 0.1:
            text = rng.choice(["  ", "\t"]) + text
        if rng.random() < 0.1:
            text += rng.choice([" ; ", ";", "  ; "]) + self.comment()
        return text, intent

    def comment(self):
        return "".join(self.rng.choice([c for c in PRINTABLE if c not in "\"'"]) for _ in range(self.rng.randint(0, 20)))

    def sequence(self, start, invalid_rate):
        """A random sequence: (text, intents, valid)."""
        rng = self.rng
        count = rng.choice([1, 2, 3, rng.randint(1, 8), rng.randint(1, 30)])
        spoiled = rng.randrange(count) if rng.random() < invalid_rate else -1
        how = rng.choice(["command", "argument", "argument", "time"])
        lines = []
        intents = []
        for index in range(count):
            if rng.random() < 0.1:
                lines.append("; " + self.comment())
            if rng.random() < 0.05:
                lines.append("")
            text, intent = self.command_line(start, how if index == spoiled else None)
            lines.append(text)
            intents.append(intent)
        return "\n".join(lines) + ("\n" if rng.random() < 0.9 else ""), intents, spoiled < 0


# ----------------------------------------------------------------------
# Scenarios
# ----------------------------------------------------------------------


def duration_of(intents, start_us):
    """How long a run could take, microseconds, at most."""
    total = 0
    for descriptor, seconds, useconds in intents:
        tag = seconds * US + useconds
        if descriptor == 1:
            total += tag
        else:
            total = max(total, tag - start_us)
    return total


def scenario_for(rng, intents, start_us):
    commands = len(intents)
    runs = 1 if rng.random() < 0.8 else 2
    latency = rng.choice([0, 0, 0, 1000, 10_000, 250_000, rng.randint(0, 5 * US)])
    silent = rng.randrange(commands * runs) if rng.random() < 0.15 else -1
    longest = (duration_of(intents, start_us) + commands * (latency + US) + (COMMAND_TIMEOUT_US if silent >= 0 else 0)
               + 2 * US) * runs + 10 * US
    ticks = [tick for tick in TICKS if longest // tick <= MAX_TICKS]
    tick = rng.choice(ticks) if ticks else TICKS[-1]
    return {
        "start_seconds": start_us // US,
        "start_useconds": start_us % US,
        "tick_us": tick,
        "latency_us": latency,
        "fail_at": rng.randrange(commands * runs) if rng.random() < 0.4 else -1,
        "fail_with": rng.choice(list(FAILURES)),
        "silent_at": silent,
        "cancel_at_us": rng.randint(0, longest) if rng.random() < 0.2 else -1,
        "runs": runs,
        "limit_us": longest + 3600 * US,
    }


def case_line(name, scenario):
    fields = [
        name,
        f"{name}.bin",
        f"{name}.wasm",
        scenario["start_seconds"],
        scenario["start_useconds"],
        scenario["tick_us"],
        scenario["latency_us"],
        scenario["fail_at"],
        scenario["fail_with"],
        scenario["silent_at"],
        scenario["cancel_at_us"],
        scenario["runs"],
        scenario["limit_us"],
    ]
    return "\t".join(str(field) for field in fields)


# ----------------------------------------------------------------------
# Compiling and running
# ----------------------------------------------------------------------


class Compilers:
    def __init__(self, dictionary_path, dictionary, fprime_wasm):
        self.dictionary_path = dictionary_path
        self.dictionary = dictionary
        self.fprime_wasm = fprime_wasm
        from fprime_gds.common.models.dictionaries import Dictionaries
        from fprime_gds.common.tools import seqgen

        # What the fprime-seqgen command does before compiling: take the type
        # definitions (FwSizeStoreType, FwOpcodeType, ...) from the dictionary
        Dictionaries.load_dictionaries_into_config(str(dictionary_path))
        self.seqgen = seqgen

    def seqgen_compile(self, source, output):
        """fprime-seqgen, in process: (ok, messages)."""
        if output.exists():
            output.unlink()
        named, _ = e2e.full_names(source.read_text(), self.dictionary.names)
        copy = source.with_suffix(".seqgen.seq")
        copy.write_text(named)
        captured = io.StringIO()
        try:
            with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
                self.seqgen.generateSequence(str(copy), str(output), str(self.dictionary_path), 0xFFFF)
        except self.seqgen.SeqGenException as error:
            return False, error.getMsg()
        except Exception as error:  # fprime-seqgen crashing is a rejection too, but say so
            if output.exists():
                output.unlink()
            return False, f"fprime-seqgen raised {type(error).__name__}: {error}"
        return output.exists(), captured.getvalue()

    def wasm_compile(self, sources):
        """`fprime-wasm seq` on several sources at once; {source: (ok, messages)}."""
        for source in sources:
            if source.with_suffix(".wasm").exists():
                source.with_suffix(".wasm").unlink()
        result = subprocess.run(
            [str(self.fprime_wasm), "seq", "--dictionary", str(self.dictionary_path)] + [str(s) for s in sources],
            capture_output=True,
            text=True,
            cwd=sources[0].parent if sources else None,
        )
        output = (result.stdout + result.stderr).splitlines()
        outcomes = {}
        for source in sources:
            messages = "\n".join(line for line in output if source.name in line)
            outcomes[source] = (source.with_suffix(".wasm").exists(), messages)
        return outcomes


def read_bin_times(path):
    """The (descriptor, seconds, useconds) of each command record in a CmdSequencer file."""
    data = path.read_bytes()
    _size, count = struct.unpack(">II", data[:8])
    offset = 11
    times = []
    for _ in range(count):
        descriptor = data[offset]
        if descriptor == 2:
            break
        seconds, useconds, length = struct.unpack(">III", data[offset + 1 : offset + 13])
        times.append((descriptor, seconds, useconds))
        offset += 13 + length
    return times


def run_cases(executable, directory, lines, gtest_filter=None):
    """Run the cases in `lines` (written to `directory`); {name: failure message or None}, and whether it crashed."""
    cases_file = directory / "cases.tsv"
    cases_file.write_text("\n".join(lines) + "\n")
    report = directory / "results.json"
    if report.exists():
        report.unlink()
    command = [str(executable), f"--gtest_output=json:{report}"]
    if gtest_filter:
        command.append(f"--gtest_filter={gtest_filter}")
    result = subprocess.run(
        command, env=dict(os.environ, WASM_SEQ_FUZZ_CASES=str(cases_file)), capture_output=True, text=True
    )
    if not report.exists():
        return None, result.stdout + result.stderr
    outcomes = {}
    for suite in json.loads(report.read_text()).get("testsuites", []):
        for test in suite.get("testsuite", []):
            failures = test.get("failures", [])
            outcomes[test["name"]] = "\n".join(strip_assertion(f["failure"]) for f in failures) if failures else None
    return outcomes, result.stdout + result.stderr


def strip_assertion(failure):
    """The judge's message, without Google Test's `file:line` and `Expected: true` preamble."""
    marker = "Expected: true\n"
    return failure.split(marker, 1)[1] if marker in failure else failure


def category_of(message):
    if message.startswith("CmdSequencer departs") or message.startswith("CmdSequencer did not finish"):
        return "model"
    return "behavior"


# ----------------------------------------------------------------------
# One case, end to end, for minimizing and replaying
# ----------------------------------------------------------------------


class Runner:
    def __init__(self, compilers, executable):
        self.compilers = compilers
        self.executable = executable

    def check(self, directory, text, scenario, intents=None):
        """Compile and run one case in `directory`.

        Returns (kind, message): kind is "pass", "compile" (the compilers
        disagree), "rejected" (both reject), "seqgen-time" (fprime-seqgen
        encoded a time other than the one written), "crash", "model" or
        "behavior".
        """
        if directory.exists():
            shutil.rmtree(directory)
        directory.mkdir(parents=True)
        source = directory / "case.seq"
        source.write_text(text)
        bin_ok, bin_messages = self.compilers.seqgen_compile(source, directory / "case.bin")
        wasm_ok, wasm_messages = self.compilers.wasm_compile([source])[source]
        if bin_ok != wasm_ok:
            return "compile", compile_message(bin_ok, bin_messages, wasm_ok, wasm_messages)
        if not bin_ok:
            return "rejected", ""
        if intents is not None and read_bin_times(directory / "case.bin") != intents:
            return "seqgen-time", "fprime-seqgen encoded different times"
        outcomes, output = run_cases(self.executable, directory, [case_line("case", scenario)])
        if outcomes is None:
            return "crash", output
        message = outcomes.get("case")
        if message is None:
            return "pass", ""
        return category_of(message), message


def compile_message(bin_ok, bin_messages, wasm_ok, wasm_messages):
    return (
        f"fprime-seqgen {'accepts' if bin_ok else 'rejects'}, fprime-wasm {'accepts' if wasm_ok else 'rejects'}\n"
        f"  fprime-seqgen:\n{e2e.indent(bin_messages)}\n  fprime-wasm:\n{e2e.indent(wasm_messages)}"
    )


def split_lines(text):
    return text.splitlines()


def is_command(line):
    return e2e.COMMAND_LINE.match(line) is not None


def minimize(runner, directory, text, scenario, kind, budget=200):
    """Shrink a failing case while it still fails the same way."""
    attempts = 0

    def fails(candidate_text, candidate_scenario):
        nonlocal attempts
        attempts += 1
        got, _ = runner.check(directory / "attempt", candidate_text, candidate_scenario)
        return got == kind

    # Simpler scenario first
    for key, simple in [("runs", 1), ("cancel_at_us", -1), ("silent_at", -1), ("fail_at", -1), ("latency_us", 0)]:
        if kind == "compile" or scenario[key] == simple or attempts >= budget:
            continue
        candidate = dict(scenario, **{key: simple})
        if fails(text, candidate):
            scenario = candidate

    # Drop comments and blank lines, then commands, largest chunks first
    lines = split_lines(text)
    stripped = [line for line in lines if is_command(line)]
    if stripped != lines and fails("\n".join(stripped) + "\n", scenario):
        lines = stripped
    chunk = max(1, len(lines) // 2)
    while chunk >= 1 and attempts < budget:
        index = 0
        while index < len(lines) and attempts < budget:
            removed = lines[index : index + chunk]
            remaining = lines[:index] + lines[index + chunk :]
            if not remaining:
                index += chunk
                continue
            commands_before = sum(1 for line in lines[:index] if is_command(line))
            removed_commands = sum(1 for line in removed if is_command(line))
            candidate = dict(scenario)
            for key in ("fail_at", "silent_at"):
                if candidate[key] >= commands_before + removed_commands:
                    candidate[key] -= removed_commands
                elif candidate[key] >= commands_before:
                    candidate[key] = -1
            if fails("\n".join(remaining) + "\n", candidate):
                lines = remaining
                scenario = candidate
            else:
                index += chunk
        chunk //= 2
    final = "\n".join(lines) + "\n"
    shutil.rmtree(directory / "attempt", ignore_errors=True)
    return final, scenario


def save_finding(findings, name, kind, text, scenario, message, runner, args, minimized):
    directory = findings / f"{kind}-{name}"
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    (directory / "original.seq").write_text(text)
    final_text, final_scenario = text, scenario
    if minimized:
        final_text, final_scenario = minimize(runner, directory, text, scenario, kind)
    # Leave the compiled files and the run of the (minimized) case in the finding
    final_kind, final_message = runner.check(directory / "case", final_text, final_scenario)
    (directory / "case.seq").write_text(final_text)
    (directory / "scenario.json").write_text(json.dumps(final_scenario, indent=2) + "\n")
    (directory / "failure.txt").write_text((final_message or message) + "\n")
    (directory / "README.md").write_text(
        f"# Fuzz finding: {kind}\n\n"
        f"Found with seed {args.seed}, case `{name}`. `original.seq` is the generated sequence;\n"
        f"`case.seq` and `scenario.json` the {'minimized ' if minimized else ''}case, which still fails\n"
        f"({final_kind}). `failure.txt` is what the test reported. Replay it with\n\n"
        f"```shell\nSvc/WasmSequencer/test/e2e/fuzz.py --replay {directory}\n```\n"
    )
    return directory


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cases", type=int, default=500, help="cases to generate (default: 500)")
    parser.add_argument("--seconds", type=float, help="stop after this long instead, if sooner")
    parser.add_argument("--seed", type=int, help="random seed (default: chosen and printed)")
    parser.add_argument("--batch", type=int, default=100, help="cases per run of the executable")
    parser.add_argument("--invalid-rate", type=float, default=0.1, help="share of sequences made invalid on purpose")
    parser.add_argument("--no-minimize", action="store_true", help="save findings as generated")
    parser.add_argument("--stop-on-finding", action="store_true", help="stop at the first finding")
    parser.add_argument("--replay", help="replay a saved finding's directory")
    parser.add_argument("--dictionary", help="deployment dictionary (default: the Ref deployment's, built if needed)")
    parser.add_argument("--fprime-wasm", help="fprime-wasm executable (default: cargo install from git)")
    parser.add_argument("--fprime-wasm-git", default=e2e.FPRIME_WASM_GIT, help="repository to install fprime-wasm from")
    parser.add_argument(
        "--fprime-wasm-branch", default=e2e.FPRIME_WASM_BRANCH, help="branch to install fprime-wasm from"
    )
    parser.add_argument("--reinstall", action="store_true", help="reinstall fprime-wasm even if already installed")
    parser.add_argument("--ut-exe", help=f"{FUZZ_TARGET} executable (default: build it)")
    parser.add_argument(
        "--work-dir", default=str(e2e.REPO / "build-fprime-wasm-seq-e2e"), help="where cases and findings are written"
    )
    return parser.parse_args()


def replay(args, runner):
    directory = Path(args.replay).resolve()
    text = (directory / "case.seq").read_text()
    scenario = json.loads((directory / "scenario.json").read_text())
    kind, message = runner.check(directory / "replay", text, scenario)
    print(text)
    print(json.dumps(scenario, indent=2))
    print(f"result: {kind}")
    if message:
        print(message)
    return 0 if kind in ("pass", "rejected") else 1


def main():
    args = parse_args()
    work = Path(args.work_dir).resolve()
    work.mkdir(parents=True, exist_ok=True)

    dictionary_path = e2e.find_dictionary(args)
    fprime_wasm = e2e.find_fprime_wasm(args, work)
    executable = e2e.build_executable(FUZZ_TARGET, args.ut_exe)
    dictionary = Dictionary(dictionary_path)
    compilers = Compilers(dictionary_path, dictionary, fprime_wasm)
    runner = Runner(compilers, executable)

    if args.replay:
        return replay(args, runner)

    if args.seed is None:
        args.seed = random.SystemRandom().randrange(1 << 32)
    e2e.log(f"seed {args.seed}; replay this run with --seed {args.seed} --cases {args.cases} --batch {args.batch}")
    rng = random.Random(args.seed)
    generator = Generator(dictionary, rng)

    fuzz = work / "fuzz"
    batches = fuzz / "batches"
    findings = fuzz / "findings"
    shutil.rmtree(batches, ignore_errors=True)
    findings.mkdir(parents=True, exist_ok=True)

    counts = {"pass": 0, "rejected": 0, "seqgen-time": 0}
    saved = []
    began = time.monotonic()
    generated = 0
    batch_number = 0
    while generated < args.cases:
        if args.seconds is not None and time.monotonic() - began > args.seconds:
            break
        batch_dir = batches / f"b{batch_number:04d}"
        batch_dir.mkdir(parents=True)
        batch_number += 1
        cases = []
        for _ in range(min(args.batch, args.cases - generated)):
            name = f"c{generated:06d}"
            generated += 1
            start_us = rng.randint(YEAR_START, YEAR_END) * US + (rng.randrange(US) if rng.random() < 0.5 else 0)
            text, intents, valid = generator.sequence(start_us, args.invalid_rate)
            source = batch_dir / f"{name}.seq"
            source.write_text(text)
            cases.append(
                {"name": name, "text": text, "intents": intents, "valid": valid, "source": source,
                 "scenario": scenario_for(rng, intents, start_us)}
            )

        wasm_outcomes = compilers.wasm_compile([case["source"] for case in cases])
        runnable = []
        for case in cases:
            bin_ok, bin_messages = compilers.seqgen_compile(case["source"], case["source"].with_suffix(".bin"))
            wasm_ok, wasm_messages = wasm_outcomes[case["source"]]
            if bin_ok != wasm_ok:
                case["kind"] = "compile"
                case["message"] = compile_message(bin_ok, bin_messages, wasm_ok, wasm_messages)
            elif not bin_ok:
                counts["rejected"] += 1
                if case["valid"]:
                    # Both rejecting a sequence meant to be valid points at the generator
                    case["kind"] = "generator"
                    case["message"] = compile_message(bin_ok, bin_messages, wasm_ok, wasm_messages)
            elif not case["valid"]:
                case["kind"] = "compile"
                case["message"] = "both compilers accept a sequence made invalid on purpose\n" + case["text"]
                # A replay cannot tell which line was meant to be invalid
                case["keep"] = True
            elif read_bin_times(case["source"].with_suffix(".bin")) != case["intents"]:
                counts["seqgen-time"] += 1
            else:
                runnable.append(case)

        if runnable:
            outcomes, output = run_cases(executable, batch_dir, [case_line(c["name"], c["scenario"]) for c in runnable])
            if outcomes is None:
                # The executable died: find which case did it, one at a time
                outcomes = {}
                for case in runnable:
                    single, single_output = run_cases(
                        executable, batch_dir, [case_line(c["name"], c["scenario"]) for c in runnable],
                        gtest_filter=f"fuzz.{case['name']}",
                    )
                    if single is None:
                        case["kind"] = "crash"
                        case["message"] = single_output[-4000:]
                    else:
                        outcomes.update(single)
            for case in runnable:
                if "kind" in case:
                    continue
                message = outcomes.get(case["name"])
                if message is None:
                    counts["pass"] += 1
                else:
                    case["kind"] = category_of(message)
                    case["message"] = message

        for case in cases:
            if "kind" not in case:
                continue
            counts[case["kind"]] = counts.get(case["kind"], 0) + 1
            e2e.log(f"{case['name']}: {case['kind']}: {case['message'].splitlines()[0] if case['message'] else ''}")
            directory = save_finding(
                findings, case["name"], case["kind"], case["text"], case["scenario"], case["message"], runner, args,
                minimized=not args.no_minimize and case["kind"] != "generator" and not case.get("keep"),
            )
            saved.append(directory)
            e2e.log(f"    saved to {directory}")
            if args.stop_on_finding:
                break
        if saved and args.stop_on_finding:
            break
        e2e.log(f"{generated} cases: " + ", ".join(f"{count} {kind}" for kind, count in sorted(counts.items())))

    e2e.log(f"done in {time.monotonic() - began:.0f} s, seed {args.seed}")
    e2e.log(", ".join(f"{count} {kind}" for kind, count in sorted(counts.items())))
    for directory in saved:
        e2e.log(f"finding: {directory}")
    return 1 if saved else 0


if __name__ == "__main__":
    sys.exit(main())
