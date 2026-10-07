// ======================================================================
// \title  SequenceModel.cpp
// \brief  What a sequence file calls for, and whether two sequencers' runs
//         of it agree
// ======================================================================

#include "Svc/WasmSequencer/test/e2e/SequenceModel.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace Svc {
namespace WasmSeqE2e {

namespace {

bool readFile(const std::string& path, std::vector<U8>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

//! The index of the first event at which `a` and `b` differ
size_t firstDifference(const Trace& a, const Trace& b) {
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return common;
}

std::string details(const Trace& cmdTrace, const Trace& wasmTrace) {
    return "\n  CmdSequencer:\n" + describe(cmdTrace) + "  WasmSequencer:\n" + describe(wasmTrace);
}

//! Compare the events of the two traces before `untilUs` (all of them when
//! `untilUs` is the maximum)
std::string compare(const Trace& cmdTrace, const Trace& wasmTrace, U64 untilUs) {
    Trace cmd;
    Trace wasm;
    for (const Event& event : cmdTrace) {
        if (event.timeUs < untilUs) {
            cmd.push_back(event);
        }
    }
    for (const Event& event : wasmTrace) {
        if (event.timeUs < untilUs) {
            wasm.push_back(event);
        }
    }
    const size_t at = firstDifference(cmd, wasm);
    if (at == cmd.size() && at == wasm.size()) {
        return "";
    }
    std::string message = "first difference at event " + std::to_string(at) + ":";
    message += "\n    CmdSequencer:  " + (at < cmd.size() ? describe(cmd[at]) : std::string("(nothing more)"));
    message += "\n    WasmSequencer: " + (at < wasm.size() ? describe(wasm[at]) : std::string("(nothing more)"));
    return message + details(cmdTrace, wasmTrace);
}

//! When CmdSequencer first dispatched a command for an absolute time that had
//! already passed when the record was reached (the maximum if it never did).
//! Commands go out in record order, from the first record again at each
//! start; a record is reached when the run starts or the command before it
//! completes.
U64 firstPastAbsolute(const std::vector<Record>& records, const Scenario& scenario, const Trace& cmdTrace) {
    const U64 start = microseconds(scenario.start);
    size_t index = 0;
    U64 reached = 0;
    for (const Event& event : cmdTrace) {
        if (event.kind == Event::SEQ_START) {
            index = 0;
            reached = event.timeUs;
        } else if (event.kind == Event::COMMAND) {
            if (index < records.size() && records[index].descriptor == Record::ABSOLUTE) {
                const U64 tag = static_cast<U64>(records[index].seconds) * US + records[index].useconds;
                if (tag <= start + reached) {
                    return event.timeUs;
                }
            }
            index++;
            reached = tickAtOrAfter(event.timeUs + scenario.latencyUs, scenario.tickUs);
        }
    }
    return ~static_cast<U64>(0);
}

}  // namespace

bool parseBin(const std::string& path, std::vector<Record>& records, std::string& error) {
    std::vector<U8> bytes;
    if (!readFile(path, bytes)) {
        error = "cannot read " + path;
        return false;
    }
    Fw::ExternalSerializeBuffer buffer(bytes.data(), bytes.size());
    if (buffer.setBuffLen(bytes.size()) != Fw::FW_SERIALIZE_OK) {
        error = "cannot buffer " + path;
        return false;
    }
    U32 size = 0;
    U32 count = 0;
    FwTimeBaseStoreType timeBase = 0;
    FwTimeContextStoreType timeContext = 0;
    bool ok = buffer.deserializeTo(size) == Fw::FW_SERIALIZE_OK && buffer.deserializeTo(count) == Fw::FW_SERIALIZE_OK &&
              buffer.deserializeTo(timeBase) == Fw::FW_SERIALIZE_OK &&
              buffer.deserializeTo(timeContext) == Fw::FW_SERIALIZE_OK;
    for (U32 i = 0; ok && i < count; i++) {
        U8 descriptor = 0;
        Record record{};
        ok = buffer.deserializeTo(descriptor) == Fw::FW_SERIALIZE_OK && descriptor <= Record::END_OF_SEQUENCE;
        record.descriptor = static_cast<Record::Descriptor>(descriptor);
        if (ok && record.descriptor != Record::END_OF_SEQUENCE) {
            U32 length = 0;
            ok = buffer.deserializeTo(record.seconds) == Fw::FW_SERIALIZE_OK &&
                 buffer.deserializeTo(record.useconds) == Fw::FW_SERIALIZE_OK &&
                 buffer.deserializeTo(length) == Fw::FW_SERIALIZE_OK && length <= buffer.getDeserializeSizeLeft();
            if (ok) {
                record.packet.assign(buffer.getBuffAddrLeft(), buffer.getBuffAddrLeft() + length);
                ok = buffer.deserializeSkip(length) == Fw::FW_SERIALIZE_OK;
            }
        }
        records.push_back(record);
    }
    if (!ok) {
        error = "malformed sequence file " + path;
    }
    return ok;
}

U64 microseconds(const Fw::Time& time) {
    return static_cast<U64>(time.getSeconds()) * US + time.getUSeconds();
}

U64 tickAtOrAfter(U64 us, U32 tickUs) {
    return ((us + tickUs - 1) / tickUs) * tickUs;
}

bool hasReference(const Scenario& scenario) {
    return scenario.cancelAtUs < 0;
}

Trace reference(const std::vector<Record>& records, const Scenario& scenario, Sequencer sequencer) {
    Trace trace;
    const U64 start = microseconds(scenario.start);
    U64 now = 0;
    I32 index = 0;
    for (U32 run = 0; run < scenario.runs; run++) {
        trace.push_back(Event{Event::SEQ_START, now, {}, Fw::CmdResponse::OK});
        Fw::CmdResponse::T outcome = Fw::CmdResponse::OK;
        for (const Record& record : records) {
            if (record.descriptor == Record::END_OF_SEQUENCE) {
                break;
            }
            const U64 tag = static_cast<U64>(record.seconds) * US + record.useconds;
            U64 due = 0;
            if (record.descriptor == Record::RELATIVE) {
                due = now + tag;
            } else {
                due = (tag > start) ? tag - start : 0;
            }
            if (due > now) {
                now = tickAtOrAfter(due, scenario.tickUs);
            } else if (record.descriptor == Record::ABSOLUTE && sequencer == Sequencer::WASM) {
                now = tickAtOrAfter(now + 1, scenario.tickUs);
            }
            trace.push_back(Event{Event::COMMAND, now, record.packet, Fw::CmdResponse::OK});
            const I32 current = index++;
            if (current == scenario.silentAt) {
                now = tickAtOrAfter(now + Bench::COMMAND_TIMEOUT_S * US, scenario.tickUs);
                outcome = Fw::CmdResponse::EXECUTION_ERROR;
                break;
            }
            // The dispatcher's answer is delivered on the bench's step
            now = tickAtOrAfter(now + scenario.latencyUs, scenario.tickUs);
            if (current == scenario.failAt) {
                outcome = Fw::CmdResponse::EXECUTION_ERROR;
                break;
            }
        }
        trace.push_back(Event{Event::SEQ_DONE, now, {}, outcome});
        now = tickAtOrAfter(now + scenario.settleUs, scenario.tickUs);
    }
    return trace;
}

std::string judge(const std::vector<Record>& records,
                  const Scenario& scenario,
                  const Trace& cmdTrace,
                  bool cmdFinished,
                  const Trace& wasmTrace,
                  bool wasmFinished) {
    if (!cmdFinished) {
        return "CmdSequencer did not finish" + details(cmdTrace, wasmTrace);
    }
    if (!wasmFinished) {
        return "WasmSequencer did not finish" + details(cmdTrace, wasmTrace);
    }

    const Trace cmdExpected = reference(records, scenario, Sequencer::CMD);
    const Trace wasmExpected = reference(records, scenario, Sequencer::WASM);
    const U64 everything = ~static_cast<U64>(0);

    if (!hasReference(scenario)) {
        // Up to the first command CmdSequencer dispatched for an absolute time
        // already past, where the expected delay starts and the cancel may
        // then find the two in different states
        return compare(cmdTrace, wasmTrace, firstPastAbsolute(records, scenario, cmdTrace));
    }

    if (cmdExpected == wasmExpected) {
        const std::string difference = compare(cmdTrace, wasmTrace, everything);
        if (!difference.empty()) {
            return difference;
        }
    }
    if (cmdTrace != cmdExpected) {
        return "CmdSequencer departs from the reference\n  Reference:\n" + describe(cmdExpected) +
               details(cmdTrace, wasmTrace);
    }
    if (cmdExpected == wasmExpected) {
        return "";
    }
    // An absolute time already past: the expected tick of difference
    if (wasmTrace != wasmExpected) {
        return "WasmSequencer departs from its reference (CmdSequencer's, with each past absolute time a tick "
               "later)\n  Reference:\n" +
               describe(wasmExpected) + details(cmdTrace, wasmTrace);
    }
    return "";
}

}  // namespace WasmSeqE2e
}  // namespace Svc
