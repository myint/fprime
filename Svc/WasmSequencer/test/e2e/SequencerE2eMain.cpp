// ======================================================================
// \title  SequencerE2eMain.cpp
// \brief  End-to-end equivalence of Svc::WasmSequencer and Svc::CmdSequencer
//
// Each sequence in the manifest was compiled twice from the same `.seq`
// source: by `fprime-seqgen` into a `.bin` for CmdSequencer, and by
// `fprime-wasm seq` into a `.wasm` for WasmSequencer (see run_e2e.py). Every
// scenario below runs both on identical benches and requires the two traces --
// each command's bytes and dispatch time, the start and completion reports
// and the completion status -- to be identical. Where the scenario's outcome
// follows from the `.bin` alone, the traces must also match that reference,
// so the two cannot agree by both doing nothing.
// ======================================================================

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Svc/WasmSequencer/test/e2e/SequencerBench.hpp"
#include "gtest/gtest.h"

namespace Svc {
namespace WasmSeqE2e {

namespace {

constexpr U64 US = 1000000;

//! One record of a CmdSequencer `.bin`
struct Record {
    enum Descriptor : U8 { ABSOLUTE = 0, RELATIVE = 1, END_OF_SEQUENCE = 2 };
    Descriptor descriptor;
    U32 seconds;
    U32 useconds;
    std::vector<U8> packet;
};

//! A sequence compiled both ways
struct Sequence {
    std::string name;    //!< Short name, also the test name
    std::string source;  //!< The `.seq` it was compiled from, relative to the repository
    std::string bin;     //!< CmdSequencer file, relative to the manifest
    std::string wasm;    //!< WasmSequencer module, relative to the manifest
    std::vector<Record> records;
};

bool readFile(const std::string& path, std::vector<U8>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

//! Read the command records out of a CmdSequencer F Prime sequence file
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

std::vector<Sequence>& sequences() {
    static std::vector<Sequence> all;
    return all;
}

//! Load the manifest run_e2e.py writes: one tab-separated `name source bin wasm` per line
bool loadManifest(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read manifest " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream fields(line);
        Sequence sequence;
        if (!std::getline(fields, sequence.name, '\t') || !std::getline(fields, sequence.source, '\t') ||
            !std::getline(fields, sequence.bin, '\t') || !std::getline(fields, sequence.wasm, '\t')) {
            error = "malformed manifest line: " + line;
            return false;
        }
        if (!parseBin(sequence.bin, sequence.records, error)) {
            return false;
        }
        sequences().push_back(sequence);
    }
    return true;
}

// ----------------------------------------------------------------------
// The reference: what CmdSequencer's documented semantics say a run does
// ----------------------------------------------------------------------

U64 microseconds(const Fw::Time& time) {
    return static_cast<U64>(time.getSeconds()) * US + time.getUSeconds();
}

//! First rate-group tick at or after `us`
U64 tickAtOrAfter(U64 us, U32 tickUs) {
    return ((us + tickUs - 1) / tickUs) * tickUs;
}

//! Whether `scenario` is one the reference models: no cancel of a running sequence
bool hasReference(const Scenario& scenario) {
    return scenario.cancelAtUs < 0;
}

//! The trace `records` must produce under `scenario`. A relative record is due
//! that long after the previous command completed (or the run started); an
//! absolute record at its time. A command due now is dispatched at once; one
//! due later, on the first tick at or after it is due. A command answered
//! other than OK ends the run, as does one unanswered for COMMAND_TIMEOUT_S.
Trace reference(const std::vector<Record>& records, const Scenario& scenario) {
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

// ----------------------------------------------------------------------
// Scenarios
// ----------------------------------------------------------------------

std::vector<Scenario> scenariosFor(const Sequence& sequence) {
    std::vector<Scenario> all;
    I32 commands = 0;
    U64 longestWait = 0;
    U64 longestWaitStart = 0;
    U64 elapsed = 0;
    bool absolute = false;
    U64 firstAbsolute = 0;
    U64 lastAbsolute = 0;
    for (const Record& record : sequence.records) {
        if (record.descriptor == Record::END_OF_SEQUENCE) {
            break;
        }
        commands++;
        const U64 tag = static_cast<U64>(record.seconds) * US + record.useconds;
        if (record.descriptor == Record::RELATIVE) {
            if (tag > longestWait) {
                longestWait = tag;
                longestWaitStart = elapsed;
            }
            elapsed += tag;
        } else {
            firstAbsolute = absolute ? std::min(firstAbsolute, tag) : tag;
            lastAbsolute = absolute ? std::max(lastAbsolute, tag) : tag;
            absolute = true;
        }
    }

    // How long a run takes, near enough to pick a rate the clock can be stepped at
    const U64 span = absolute ? lastAbsolute - firstAbsolute : 0;
    const U64 duration = elapsed + span + 60 * US;

    Scenario nominal;
    nominal.name = "Nominal";
    // A 100 Hz rate group; 1 Hz for a sequence that runs for hours
    nominal.tickUs = (duration <= 3600 * US) ? 10000 : 1000000;
    nominal.limitUs = duration + 3600 * US;
    if (absolute) {
        // Start a minute before the first absolute time, so it is waited for
        const U64 start = firstAbsolute - 60 * US;
        nominal.start.set(TimeBase::TB_WORKSTATION_TIME, 0, static_cast<U32>(start / US), static_cast<U32>(start % US));
    }
    all.push_back(nominal);

    Scenario latency = nominal;
    latency.name = "ResponseLatency";
    latency.latencyUs = 250000;
    all.push_back(latency);

    if (duration <= 300 * US) {
        Scenario fastRate = nominal;
        fastRate.name = "Rate1kHz";
        fastRate.tickUs = 1000;
        all.push_back(fastRate);
    }

    if (duration <= 3600 * US) {
        Scenario slowRate = nominal;
        slowRate.name = "Rate10Hz";
        slowRate.tickUs = 100000;
        all.push_back(slowRate);
    }

    Scenario oddStart = nominal;
    oddStart.name = "StartJustBeforeSecond";
    oddStart.start.add(0, 999999);
    all.push_back(oddStart);

    Scenario rerun = latency;
    rerun.name = "RunTwice";
    rerun.runs = 2;
    all.push_back(rerun);

    if (absolute) {
        Scenario late = nominal;
        late.name = "StartAfterAbsoluteTimes";
        const U64 start = lastAbsolute + 60 * US;
        late.start.set(TimeBase::TB_WORKSTATION_TIME, 0, static_cast<U32>(start / US), static_cast<U32>(start % US));
        all.push_back(late);
    }

    for (I32 k = 0; k < commands; k++) {
        Scenario fail = latency;
        fail.name = "FailCommand" + std::to_string(k);
        fail.failAt = k;
        all.push_back(fail);
    }

    if (commands > 0) {
        const Fw::CmdResponse::T others[] = {Fw::CmdResponse::INVALID_OPCODE, Fw::CmdResponse::VALIDATION_ERROR,
                                             Fw::CmdResponse::FORMAT_ERROR, Fw::CmdResponse::BUSY};
        for (const Fw::CmdResponse::T response : others) {
            Scenario fail = nominal;
            fail.name = std::string("LastCommand") + responseName(response);
            fail.failAt = commands - 1;
            fail.failWith = response;
            all.push_back(fail);
        }

        Scenario silentFirst = nominal;
        silentFirst.name = "FirstCommandTimesOut";
        silentFirst.silentAt = 0;
        all.push_back(silentFirst);

        Scenario silentLast = nominal;
        silentLast.name = "LastCommandTimesOut";
        silentLast.silentAt = commands - 1;
        all.push_back(silentLast);

        Scenario cancelAwaiting = nominal;
        cancelAwaiting.name = "CancelAwaitingResponse";
        cancelAwaiting.latencyUs = 2 * US;
        cancelAwaiting.cancelAtUs = absolute ? 61 * US : 1 * US;
        all.push_back(cancelAwaiting);
    }

    if (longestWait > 0) {
        Scenario cancelWaiting = nominal;
        cancelWaiting.name = "CancelDuringWait";
        cancelWaiting.cancelAtUs = static_cast<I64>(longestWaitStart + longestWait / 2);
        all.push_back(cancelWaiting);
    }

    // A cancel with nothing running ends nothing and reports nothing
    Scenario cancelAfter = nominal;
    cancelAfter.name = "CancelAfterCompletion";
    cancelAfter.cancelAfterDone = true;
    all.push_back(cancelAfter);

    return all;
}

// ----------------------------------------------------------------------
// The test
// ----------------------------------------------------------------------

class Equivalence : public ::testing::Test {
  public:
    Equivalence(const Sequence& sequence, const Scenario& scenario) : m_sequence(sequence), m_scenario(scenario) {}

    void TestBody() override {
        SCOPED_TRACE(this->m_sequence.source);

        std::unique_ptr<CmdSequencerBench> cmdBench(new CmdSequencerBench());
        const Trace cmdTrace = cmdBench->run(this->m_sequence.bin.c_str(), this->m_scenario);

        std::unique_ptr<WasmSequencerBench> wasmBench(new WasmSequencerBench());
        const Trace wasmTrace = wasmBench->run(this->m_sequence.wasm.c_str(), this->m_scenario);

        const std::string detail = "\n  CmdSequencer:\n" + describe(cmdTrace) + "  CmdSequencer events:\n" +
                                   cmdBench->log() + "\n  WasmSequencer:\n" + describe(wasmTrace) +
                                   "  WasmSequencer events:\n" + wasmBench->log();

        EXPECT_TRUE(cmdBench->finished()) << "CmdSequencer did not finish" << detail;
        EXPECT_TRUE(wasmBench->finished()) << "WasmSequencer did not finish" << detail;

        // Same behavior, event by event
        const size_t common = std::min(cmdTrace.size(), wasmTrace.size());
        for (size_t i = 0; i < common; i++) {
            if (cmdTrace[i] != wasmTrace[i]) {
                ADD_FAILURE() << "first difference at event " << i << ":\n    CmdSequencer:  " << describe(cmdTrace[i])
                              << "\n    WasmSequencer: " << describe(wasmTrace[i]) << detail;
                return;
            }
        }
        EXPECT_EQ(cmdTrace.size(), wasmTrace.size()) << "one sequencer did more than the other" << detail;

        // And the behavior the sequence file calls for
        if (hasReference(this->m_scenario)) {
            const Trace expected = reference(this->m_sequence.records, this->m_scenario);
            EXPECT_TRUE(cmdTrace == expected) << "CmdSequencer departs from the reference\n  Reference:\n"
                                              << describe(expected) << detail;
        }
    }

  private:
    const Sequence m_sequence;
    const Scenario m_scenario;
};

void registerTests() {
    for (const Sequence& sequence : sequences()) {
        for (const Scenario& scenario : scenariosFor(sequence)) {
            ::testing::RegisterTest(sequence.name.c_str(), scenario.name.c_str(), nullptr, nullptr, __FILE__, __LINE__,
                                    [=]() -> ::testing::Test* { return new Equivalence(sequence, scenario); });
        }
    }
}

}  // namespace
}  // namespace WasmSeqE2e
}  // namespace Svc

TEST(WasmSequencerE2e, Manifest) {
    const char* manifest = std::getenv("WASM_SEQ_E2E_MANIFEST");
    if (manifest == nullptr) {
        GTEST_SKIP() << "WASM_SEQ_E2E_MANIFEST is not set; run Svc/WasmSequencer/test/e2e/run_e2e.py";
    }
    ASSERT_FALSE(Svc::WasmSeqE2e::sequences().empty()) << "the manifest lists no sequences";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // The manifest names its files relative to its own directory, and both
    // sequencers are given those short names: CmdSequencer's port takes a
    // command string, which is too short for most absolute paths
    const char* manifest = std::getenv("WASM_SEQ_E2E_MANIFEST");
    if (manifest != nullptr) {
        std::string path(manifest);
        const size_t slash = path.rfind('/');
        if (slash != std::string::npos) {
            if (chdir(path.substr(0, slash).c_str()) != 0) {
                std::fprintf(stderr, "cannot enter the directory of %s\n", manifest);
                return 1;
            }
            path = path.substr(slash + 1);
        }
        std::string error;
        if (!Svc::WasmSeqE2e::loadManifest(path, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        Svc::WasmSeqE2e::registerTests();
    }
    return RUN_ALL_TESTS();
}
