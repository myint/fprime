// ======================================================================
// \title  SequenceModel.hpp
// \brief  What a sequence file calls for, and whether two sequencers' runs
//         of it agree
// ======================================================================

#ifndef Svc_WasmSequencer_E2e_SequenceModel_HPP
#define Svc_WasmSequencer_E2e_SequenceModel_HPP

#include <string>
#include <vector>

#include "Svc/WasmSequencer/test/e2e/SequencerBench.hpp"

namespace Svc {
namespace WasmSeqE2e {

constexpr U64 US = 1000000;

//! One record of a CmdSequencer `.bin`
struct Record {
    enum Descriptor : U8 { ABSOLUTE = 0, RELATIVE = 1, END_OF_SEQUENCE = 2 };
    Descriptor descriptor;
    U32 seconds;
    U32 useconds;
    std::vector<U8> packet;
};

//! Read the command records out of a CmdSequencer F Prime sequence file
bool parseBin(const std::string& path, std::vector<Record>& records, std::string& error);

U64 microseconds(const Fw::Time& time);

//! First rate-group tick at or after `us`
U64 tickAtOrAfter(U64 us, U32 tickUs);

//! Whether `scenario` is one the reference models: no cancel of a running sequence
bool hasReference(const Scenario& scenario);

//! The sequencer a reference is for
enum class Sequencer { CMD, WASM };

//! The trace `records` must produce under `scenario`. A relative record is due
//! that long after the previous command completed (or the run started); an
//! absolute record at its time. A command due now is dispatched at once; one
//! due later, on the first tick at or after it is due. The exception is
//! WasmSequencer's absolute record that is already due: `asleep` still waits,
//! so the command is dispatched on the next tick. A command answered other
//! than OK ends the run, as does one unanswered for COMMAND_TIMEOUT_S.
//!
//! A cancel is not modeled. Neither is a response slower than the timeout.
Trace reference(const std::vector<Record>& records, const Scenario& scenario, Sequencer sequencer);

//! Judge WasmSequencer's run against CmdSequencer's, the oracle. Returns an
//! empty string when they agree, else what went wrong.
//!
//! The traces must be identical, except for the one expected difference: a
//! command whose absolute time has already passed is dispatched a tick later
//! by WasmSequencer. Where the scenario's references show that difference,
//! each sequencer is held to its own reference instead; under a cancel,
//! which the references do not model, the traces must agree up to the first
//! point the references differ.
//!
//! Where a reference exists, CmdSequencer must also match it, so the two
//! cannot agree by both doing nothing.
std::string judge(const std::vector<Record>& records,
                  const Scenario& scenario,
                  const Trace& cmdTrace,
                  bool cmdFinished,
                  const Trace& wasmTrace,
                  bool wasmFinished);

}  // namespace WasmSeqE2e
}  // namespace Svc

#endif
