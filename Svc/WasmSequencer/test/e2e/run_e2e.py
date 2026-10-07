#!/usr/bin/env python3
"""End-to-end test: WasmSequencer runs `.seq` sequences exactly as CmdSequencer does.

Every `.seq` file tracked in the repository, plus the sequences in
Svc/WasmSequencer/test/e2e/seq, is compiled twice against the Ref deployment's
dictionary:

* by `fprime-seqgen` into a `.bin`, the file Svc::CmdSequencer runs, and
* by `fprime-wasm seq` (https://github.com/myint/fprime-wasm, branch
  `myint-seq`) into a `.wasm`, the module Svc::WasmSequencer runs.

The two compilers must agree on which sequences are valid. Each valid one is
then run through both sequencers by the Svc_WasmSequencer_e2e unit test
executable, which checks the two behave identically: the same commands, byte
for byte, at the same times, with the same outcome, across nominal runs,
command failures, timeouts and cancels.

Usage (from an F Prime virtual environment, with Rust installed):

    Svc/WasmSequencer/test/e2e/run_e2e.py [--gtest_filter=...]

Anything not recognized is passed on to the test executable.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent

FPRIME_WASM_GIT = "https://github.com/myint/fprime-wasm"
FPRIME_WASM_BRANCH = "myint-seq"

E2E_TARGET = "Svc_WasmSequencer_e2e"

# A command line: time tag, then mnemonic (fprime-seqgen's grammar)
COMMAND_LINE = re.compile(r"^(\s*[RA][0-9][^\s,]*[\s,]+)([A-Za-z_][A-Za-z0-9_.]*)(.*)$", re.DOTALL)


def log(message):
    print(f"[e2e] {message}", flush=True)


def run(command, **kwargs):
    log("$ " + " ".join(str(part) for part in command))
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


# ----------------------------------------------------------------------
# Tools and inputs
# ----------------------------------------------------------------------


def find_dictionary(args):
    """The Ref deployment's dictionary, building Ref if it has not been built."""
    if args.dictionary:
        return Path(args.dictionary).resolve()
    pattern = "TestDeploymentsProject/build-artifacts/*/Ref/dict/RefTopologyDictionary.json"
    found = sorted(REPO.glob(pattern))
    if not found:
        log("no Ref dictionary found; building TestDeploymentsProject/Ref")
        ref = REPO / "TestDeploymentsProject" / "Ref"
        run(["fprime-util", "generate"], cwd=ref)
        run(["fprime-util", "build", "-j", str(os.cpu_count() or 1)], cwd=ref)
        found = sorted(REPO.glob(pattern))
    if not found:
        sys.exit(f"no dictionary matches {pattern}")
    return found[0]


def find_fprime_wasm(args, work):
    """The `fprime-wasm` command: given, or installed from the myint-seq branch."""
    if args.fprime_wasm:
        return Path(args.fprime_wasm).resolve()
    root = work / "fprime-wasm"
    binary = root / "bin" / "fprime-wasm"
    if not binary.exists() or args.reinstall:
        run(
            [
                "cargo",
                "install",
                "--locked",
                "--force",
                "--git",
                args.fprime_wasm_git,
                "--branch",
                args.fprime_wasm_branch,
                "--root",
                root,
                "fprime-wasm",
            ]
        )
    return binary


def find_e2e_executable(args):
    """The Svc_WasmSequencer_e2e executable, built in the repository's unit test build."""
    if args.ut_exe:
        return Path(args.ut_exe).resolve()
    build = REPO / "build-fprime-automatic-native-ut"
    if not (build / "CMakeCache.txt").exists():
        run(["fprime-util", "generate", "--ut"], cwd=REPO)
    run(["cmake", "--build", build, "--target", E2E_TARGET, "-j", str(os.cpu_count() or 1)])
    found = sorted(build.glob(f"bin/*/{E2E_TARGET}"))
    if not found:
        sys.exit(f"{E2E_TARGET} was not built under {build}")
    return found[0]


def find_sequences():
    """Every `.seq` file in the repository, by path relative to it."""
    tracked = subprocess.run(
        ["git", "ls-files", "--", "*.seq"], cwd=REPO, check=True, capture_output=True, text=True
    ).stdout.split()
    extra = [str(path.relative_to(REPO)) for path in (HERE / "seq").glob("*.seq")]
    return sorted(set(tracked) | set(extra))


# ----------------------------------------------------------------------
# Compiling
# ----------------------------------------------------------------------


def command_names(dictionary):
    with open(dictionary) as file:
        return [command["name"] for command in json.load(file)["commands"]]


def full_names(source, names):
    """`source` with each command named in full.

    `fprime-wasm seq` accepts any trailing part of a command's name that is
    unique (`cmdDisp.CMD_NO_OP`); `fprime-seqgen` only the full name
    (`CdhCore.cmdDisp.CMD_NO_OP`). Naming each command in full for seqgen keeps
    the two compiling the same command. A name that is not a unique suffix is
    left alone, for seqgen to reject. Returns the text and what was renamed.
    """
    exact = set(names)
    renamed = []
    lines = []
    for number, line in enumerate(source.splitlines(keepends=True), start=1):
        match = COMMAND_LINE.match(line)
        if match and match.group(2) not in exact:
            mnemonic = match.group(2)
            candidates = [name for name in names if name.endswith("." + mnemonic)]
            if len(candidates) == 1:
                line = match.group(1) + candidates[0] + match.group(3)
                renamed.append(f"line {number}: {mnemonic} -> {candidates[0]}")
        lines.append(line)
    return "".join(lines), renamed


def compile_seqgen(source, output, dictionary):
    """Compile with fprime-seqgen; returns (ok, messages)."""
    if output.exists():
        output.unlink()
    result = subprocess.run(
        ["fprime-seqgen", "--dictionary", str(dictionary), str(source), str(output)],
        capture_output=True,
        text=True,
    )
    ok = result.returncode == 0 and output.exists()
    return ok, (result.stdout + result.stderr).strip()


def compile_wasm(fprime_wasm, source, output, dictionary):
    """Compile with `fprime-wasm seq`; returns (ok, messages)."""
    if output.exists():
        output.unlink()
    result = subprocess.run(
        [str(fprime_wasm), "seq", str(source), "--dictionary", str(dictionary), "--output", str(output)],
        capture_output=True,
        text=True,
    )
    ok = result.returncode == 0 and output.exists()
    return ok, (result.stdout + result.stderr).strip()


def test_name(path):
    """A Google Test suite name for the sequence at `path`."""
    relative = Path(path)
    if relative.parent == (HERE / "seq").relative_to(REPO):
        stem = "e2e_" + relative.stem
    else:
        stem = str(relative.with_suffix(""))
    return re.sub(r"[^A-Za-z0-9]", "_", stem)


def indent(text):
    return "\n".join("        " + line for line in text.splitlines()) or "        (no output)"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dictionary", help="deployment dictionary (default: the Ref deployment's, built if needed)")
    parser.add_argument("--fprime-wasm", help="fprime-wasm executable (default: cargo install from git)")
    parser.add_argument("--fprime-wasm-git", default=FPRIME_WASM_GIT, help="repository to install fprime-wasm from")
    parser.add_argument("--fprime-wasm-branch", default=FPRIME_WASM_BRANCH, help="branch to install fprime-wasm from")
    parser.add_argument("--reinstall", action="store_true", help="reinstall fprime-wasm even if already installed")
    parser.add_argument("--ut-exe", help=f"{E2E_TARGET} executable (default: build it)")
    parser.add_argument(
        "--work-dir", default=str(REPO / "build-fprime-wasm-seq-e2e"), help="where compiled sequences are written"
    )
    args, gtest_args = parser.parse_known_args()

    if shutil.which("fprime-seqgen") is None:
        sys.exit("fprime-seqgen is not on PATH; activate an F Prime virtual environment")

    work = Path(args.work_dir).resolve()
    sequences_dir = work / "sequences"
    if sequences_dir.exists():
        shutil.rmtree(sequences_dir)
    sequences_dir.mkdir(parents=True)

    dictionary = find_dictionary(args)
    fprime_wasm = find_fprime_wasm(args, work)
    executable = find_e2e_executable(args)
    names = command_names(dictionary)
    log(f"dictionary: {dictionary}")
    log(f"fprime-wasm: {fprime_wasm}")

    manifest = ["# name\tsource\tCmdSequencer file\tWasmSequencer module"]
    disagreements = []
    rejected = []
    for index, path in enumerate(find_sequences()):
        source = REPO / path
        stem = f"s{index:03d}"
        named, renamed = full_names(source.read_text(), names)
        seqgen_source = sequences_dir / f"{stem}.seq"
        seqgen_source.write_text(named)

        bin_ok, bin_messages = compile_seqgen(seqgen_source, sequences_dir / f"{stem}.bin", dictionary)
        wasm_ok, wasm_messages = compile_wasm(fprime_wasm, source, sequences_dir / f"{stem}.wasm", dictionary)

        log(f"{path}: fprime-seqgen {'ok' if bin_ok else 'rejected'}, fprime-wasm {'ok' if wasm_ok else 'rejected'}")
        for rename in renamed:
            log(f"    for fprime-seqgen, {rename}")
        if bin_ok and wasm_ok:
            manifest.append(f"{test_name(path)}\t{path}\t{stem}.bin\t{stem}.wasm")
        elif not bin_ok and not wasm_ok:
            rejected.append(path)
            log("    fprime-seqgen:\n" + indent(bin_messages))
            log("    fprime-wasm:\n" + indent(wasm_messages))
        else:
            disagreements.append(path)
            log("    fprime-seqgen:\n" + indent(bin_messages))
            log("    fprime-wasm:\n" + indent(wasm_messages))

    manifest_path = sequences_dir / "manifest.tsv"
    manifest_path.write_text("\n".join(manifest) + "\n")
    log(f"{len(manifest) - 1} sequences compiled by both, {len(rejected)} rejected by both")

    environment = dict(os.environ, WASM_SEQ_E2E_MANIFEST=str(manifest_path))
    status = subprocess.run([str(executable)] + gtest_args, env=environment).returncode

    if disagreements:
        log("the compilers disagree on whether these sequences are valid:")
        for path in disagreements:
            log(f"    {path}")
        return 1
    return status


if __name__ == "__main__":
    sys.exit(main())
