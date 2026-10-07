# WasmSequencer / CmdSequencer end-to-end equivalence

This test checks that a `.seq` command sequence behaves the same on
`Svc::WasmSequencer` as on `Svc::CmdSequencer`.

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
two cancels), the trace must also match a reference built from the file's
records. Without that check, the two sequencers could agree by both doing
nothing.

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

## Known difference

`absolute_times` fails. In CmdSequencer, a command whose absolute time has
already passed is dispatched at once (`performCmd_Step_ABSOLUTE`). In
WasmSequencer, `asleep` always waits for the next `checkTimers` tick, even when
its deadline has already passed. Each such command is therefore dispatched one
rate group period late, and every later command is delayed with it.

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
