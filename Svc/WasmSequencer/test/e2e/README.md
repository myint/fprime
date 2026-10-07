# WasmSequencer / CmdSequencer end-to-end equivalence

These tests check that a `.seq` command sequence behaves the same on
`Svc::WasmSequencer` as on `Svc::CmdSequencer`. The end-to-end test runs the
repository's example sequences. The [differential fuzzer](#differential-fuzzing)
runs random sequences and scenarios, with `CmdSequencer` as the oracle.

Each sequence is compiled twice against the `Ref` deployment's dictionary:

* by `fprime-seqgen` into the `.bin` file `CmdSequencer` runs, and
* by `fprime-wasm seq`, from the
  [`myint-seq` branch of fprime-wasm](https://github.com/myint/fprime-wasm/tree/myint-seq),
  into the `.wasm` module `WasmSequencer` runs.

The test then runs both files through both sequencers and compares what each one
does.

## Running

From an F Prime virtual environment, with Rust (`cargo`) installed:

```shell
Svc/WasmSequencer/test/e2e/run_e2e.py
```

The script does the following:

1. Finds the `Ref` dictionary, building `TestDeploymentsProject/Ref` if needed.
   `--dictionary` names a different one.
2. Installs `fprime-wasm` from the `myint-seq` branch with `cargo install` into
   `build-fprime-wasm-seq-e2e/`. `--fprime-wasm` uses an existing binary instead.
3. Compiles every `.seq` file tracked in the repository, plus the sequences in
   [`seq/`](seq), with both compilers. `fprime-wasm seq` accepts a unique
   trailing part of a command name, such as `cmdDisp.CMD_NO_OP`. `fprime-seqgen`
   only accepts the full name, so the copy given to `fprime-seqgen` names each
   command in full. The two compilers must agree on which sequences are valid.
   A sequence that only one of them accepts fails the run. A sequence that both
   reject, such as `Svc/DpCatalog/test/ut/seq/send_dps.seq`, whose command names
   predate the current topology, is reported and skipped.
4. Builds the `Svc_WasmSequencer_e2e` unit test executable and runs it on the
   compiled pairs. Arguments the script does not recognize, such as
   `--gtest_filter=e2e_waits.*`, are passed to the executable.

The executable reads the pairs from the manifest named by
`WASM_SEQ_E2E_MANIFEST`. Without that variable, it skips.

## What is compared

[`SequencerBench`](SequencerBench.hpp) wires one sequencer to a simulated
command dispatcher, clock and rate group. It runs the sequencer without its
thread and dispatches the sequencer's queue after every stimulus, so a run is
deterministic. Each scenario runs once on a `CmdSequencer` bench and once on a
`WasmSequencer` bench. The two traces must be identical. A trace records the
following:

* each command dispatched, byte for byte, and the time it was dispatched
* the `seqStartOut` and `seqDone` reports, their times, and the completion
  status

Where the outcome follows from the `.bin` file alone (every scenario except the
two cancels), each trace must also match a reference built from the file's
records. Without that check, the two sequencers could agree by both doing
nothing.

One difference between the two sequencers is expected; see
[Expected difference](#expected-difference).

Each sequence runs in the following scenarios:

| Scenario | Dispatcher and clock |
|---|---|
| `Nominal` | Every command is answered `OK` at once. The rate group runs at 100 Hz, or at 1 Hz for a sequence that runs for hours. |
| `ResponseLatency` | Each command is answered 250 ms after it is sent. Relative waits count from completion. |
| `Rate1kHz`, `Rate10Hz` | Other rate group periods, which change when a timer is seen to expire. |
| `StartJustBeforeSecond` | The clock starts at `x.999999` s, which exercises the microsecond carry. |
| `RunTwice` | The same instance runs the sequence twice in a row. |
| `FailCommand<k>` | Command *k* is answered `EXECUTION_ERROR`. One scenario runs for each command. |
| `LastCommand<response>` | The last command fails with each of the other non-`OK` responses. |
| `FirstCommandTimesOut`, `LastCommandTimesOut` | A command is never answered. Both sequencers time out after 60 s. |
| `CancelAwaitingResponse` | `seqCancelIn` is invoked while a command awaits its response. |
| `CancelDuringWait` | `seqCancelIn` is invoked halfway through the longest relative wait. |
| `CancelAfterCompletion` | `seqCancelIn` is invoked with nothing running. |
| `StartAfterAbsoluteTimes` | For a sequence with absolute times, the clock starts after every one of them. |

## Expected difference

A command whose absolute time has already passed is dispatched at different
times:

* CmdSequencer dispatches it at once (`performCmd_Step_ABSOLUTE`).
* WasmSequencer's `asleep` wakes only on a `checkTimers` tick, so it dispatches
  the command on the next tick: one rate group period later, which is one
  second at 1 Hz. Every later command is delayed with it.

This is expected. When a scenario reaches such a command, the two traces are
not compared with each other. Instead, each one is checked against its own
reference, and WasmSequencer's reference includes the one-tick delay. A
scenario with a cancel has no reference, so its traces are compared up to the
first such command. After that, the delay can legitimately leave the cancel
finding the two sequencers in different states. The rules are in `judge()` in
[`SequenceModel.hpp`](SequenceModel.hpp), shared by both tests. In
[`absolute_times.seq`](seq/absolute_times.seq), this applies to the commands
tagged with a time that has already passed, and to every scenario that starts
after all of the absolute times.

`fprime-seqgen` converts a time tag to floating-point seconds and truncates it,
which can lose a microsecond. For example, `A2026-001T12:01:00.000001` becomes
`12:01:00`, and `R00:00:28.495765` becomes 28.495764 s. `fprime-wasm seq` keeps
the exact value. The sub-second absolute time in `absolute_times.seq` is
therefore `.5`, which both tools represent exactly. The fuzzer only writes
times that `fprime-seqgen` encodes exactly.

## Sequences

Every `.seq` file in the repository is included. The sequences in [`seq/`](seq)
cover what the repository's examples do not:

* [`absolute_times.seq`](seq/absolute_times.seq): absolute time tags, mixed with
  relative ones, including one that is already in the past.
* [`arguments.seq`](seq/arguments.seq): every scalar type at its limits,
  strings, booleans, enums, arrays, nested structs and aliased types.
* [`waits.seq`](seq/waits.seq): relative waits from 1 µs to almost a day.
* [`send_dps.seq`](seq/send_dps.seq): `Svc/DpCatalog/test/ut/seq/send_dps.seq`,
  updated to the current `Ref` command names.

## Differential fuzzing

[`fuzz.py`](fuzz.py) generates random sequences in the legacy `.seq` syntax and
random scenarios to run them in. It compiles each sequence with `fprime-seqgen`
and with `fprime-wasm seq`, then runs both through the `Svc_WasmSequencer_fuzz`
executable on the same benches as the end-to-end test. `CmdSequencer` is the
oracle: `WasmSequencer` must do exactly what it does, apart from the
[expected difference](#expected-difference).

```shell
Svc/WasmSequencer/test/e2e/fuzz.py --cases 1000 --seed 1
Svc/WasmSequencer/test/e2e/fuzz.py --seconds 3600       # for an hour, with a random seed
```

The run is reproducible from the seed it prints. Tool options (`--dictionary`,
`--fprime-wasm`, `--ut-exe`, `--work-dir`) are the same as for `run_e2e.py`.

### What it generates

* **Sequences** of 1 to 30 commands, drawn from every command in the
  dictionary. Arguments of every type are generated from the dictionary:
  * integers at and inside their limits, in decimal, hex, `+`-signed and
    `1_000` forms
  * floats in several notations, including signed zero, extremes and
    subnormals
  * strings from empty to full length, in either quote
  * booleans
  * enum constants
  * arrays, with or without a trailing comma
  * structs, with members in or out of order

  A command is named in full or by a unique suffix. Lines get relative waits
  from zero to ten minutes, and absolute times from five minutes before the
  start to ten minutes after. Commas, indentation, comments and blank lines are
  sprinkled in.
* **Scenarios**:
  * the start time anywhere in 2026, to the microsecond
  * a rate group of 1 kHz, 100 Hz, 10 Hz or 1 Hz
  * response latency from zero to five seconds
  * a command answered with a failure, or never answered
  * a cancel at a random time
  * a second run on the same instance
* **Invalid sequences** (`--invalid-rate`, 10% by default): an unknown command,
  an argument out of range or of the wrong kind, a string too long, or a
  malformed time tag. Both compilers must reject these.

The generator stays within the syntax both compilers accept. `fprime-seqgen` is
more lenient than `fprime-wasm seq` in ways that are not worth reporting: it
accepts:
* too few or too many arguments
* unquoted strings
* `yes` as a boolean
* day 366 of a non-leap year
* float literals such as `inf`, `nan`, `1e309` (infinity), `1.5x`, `1e`, and
  `"1.5"`

On the other hand, it only accepts full command names. The copy given to `fprime-seqgen`
names each command in full.

### Findings

Each case ends in one of these outcomes:

| Outcome | Meaning |
|---|---|
| `pass` | `WasmSequencer` did what `CmdSequencer` did. |
| `rejected` | Both compilers rejected a sequence made invalid on purpose. |
| `behavior` | **`WasmSequencer` departs from `CmdSequencer`**: a command, time, start or completion report differs. |
| `compile` | One compiler accepts a sequence and the other rejects it, or both accept one made invalid. |
| `crash` | The test executable died, for example on an assertion or a sanitizer error. |
| `model` | `CmdSequencer` departs from the reference model. This points at the harness, not at `WasmSequencer`. |
| `generator` | Both compilers rejected a sequence meant to be valid. This points at the generator. |
| `seqgen-time` | `fprime-seqgen` encoded a different time than the one written. The case is skipped. |

The fuzzer saves each finding under `<work-dir>/fuzz/findings/<outcome>-<case>/`.
It first minimizes the finding: it simplifies the scenario, then drops lines
while the case still fails the same way. `--no-minimize` turns this off. Each
finding directory holds the following:
* `original.seq`, the sequence as generated
* `case.seq` and `scenario.json`, the minimized case
* the compiled `.bin` and `.wasm`
* `failure.txt`, what the test reported

Replay a finding with:

```shell
Svc/WasmSequencer/test/e2e/fuzz.py --replay <finding directory>
```

The script exits nonzero if any finding was saved.
