// ======================================================================
// \title  SequencerFuzzMain.cpp
// \brief  Differential fuzzing of Svc::WasmSequencer against
//         Svc::CmdSequencer
//
// fuzz.py generates random `.seq` sequences and random scenarios: when each
// command is answered and how, the rate group, the start time, cancels and
// reruns. It compiles each sequence with `fprime-seqgen` for CmdSequencer and
// with `fprime-wasm seq` for WasmSequencer, and lists the pairs that both
// accept, with their scenarios, in the file WASM_SEQ_FUZZ_CASES names. This
// executable runs every case through both sequencers on identical benches.
// CmdSequencer is the oracle: WasmSequencer must do exactly what it does,
// apart from the expected tick of delay on an absolute time already past
// (see judge() in SequenceModel.hpp).
// ======================================================================

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Svc/WasmSequencer/test/e2e/SequenceModel.hpp"
#include "Svc/WasmSequencer/test/e2e/SequencerBench.hpp"
#include "gtest/gtest.h"

namespace Svc {
namespace WasmSeqE2e {

namespace {

//! One generated sequence, compiled both ways, and the scenario to run it in
struct Case {
    std::string bin;
    std::string wasm;
    Scenario scenario;
    std::vector<Record> records;
};

std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

//! Parse a whole decimal integer, signed or not
template <typename T>
bool parseNumber(const std::string& text, T& value) {
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (text.empty() || end == nullptr || *end != '\0') {
        return false;
    }
    value = static_cast<T>(parsed);
    return true;
}

//! Load the cases fuzz.py writes, one per line, tab separated:
//!
//!     name bin wasm startSeconds startUseconds tickUs latencyUs failAt failWith silentAt cancelAtUs runs limitUs
bool loadCases(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read cases " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::vector<std::string> fields;
        std::istringstream stream(line);
        std::string field;
        while (std::getline(stream, field, '\t')) {
            fields.push_back(field);
        }
        Case item;
        U32 startSeconds = 0;
        U32 startUseconds = 0;
        I32 failWith = 0;
        bool ok = fields.size() == 13;
        if (ok) {
            item.scenario.name = fields[0];
            item.bin = fields[1];
            item.wasm = fields[2];
            ok = parseNumber(fields[3], startSeconds) && parseNumber(fields[4], startUseconds) &&
                 parseNumber(fields[5], item.scenario.tickUs) && parseNumber(fields[6], item.scenario.latencyUs) &&
                 parseNumber(fields[7], item.scenario.failAt) && parseNumber(fields[8], failWith) &&
                 parseNumber(fields[9], item.scenario.silentAt) && parseNumber(fields[10], item.scenario.cancelAtUs) &&
                 parseNumber(fields[11], item.scenario.runs) && parseNumber(fields[12], item.scenario.limitUs) &&
                 item.scenario.tickUs > 0 && startUseconds < US;
        }
        if (!ok) {
            error = "malformed case: " + line;
            return false;
        }
        item.scenario.start.set(TimeBase::TB_WORKSTATION_TIME, 0, startSeconds, startUseconds);
        item.scenario.failWith = static_cast<Fw::CmdResponse::T>(failWith);
        if (!parseBin(item.bin, item.records, error)) {
            return false;
        }
        cases().push_back(item);
    }
    return true;
}

class Differential : public ::testing::Test {
  public:
    explicit Differential(const Case& item) : m_case(item) {}

    void TestBody() override {
        std::unique_ptr<CmdSequencerBench> cmdBench(new CmdSequencerBench());
        const Trace cmdTrace = cmdBench->run(this->m_case.bin.c_str(), this->m_case.scenario);

        std::unique_ptr<WasmSequencerBench> wasmBench(new WasmSequencerBench());
        const Trace wasmTrace = wasmBench->run(this->m_case.wasm.c_str(), this->m_case.scenario);

        const std::string failure = judge(this->m_case.records, this->m_case.scenario, cmdTrace, cmdBench->finished(),
                                          wasmTrace, wasmBench->finished());
        EXPECT_TRUE(failure.empty()) << failure << "\n  CmdSequencer events:\n"
                                     << cmdBench->log() << "  WasmSequencer events:\n"
                                     << wasmBench->log();
    }

  private:
    const Case m_case;
};

}  // namespace
}  // namespace WasmSeqE2e
}  // namespace Svc

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    // Cases name their files relative to the cases file, and both sequencers
    // are given those short names: CmdSequencer's port takes a command string
    const char* file = std::getenv("WASM_SEQ_FUZZ_CASES");
    if (file == nullptr) {
        std::fprintf(stderr, "WASM_SEQ_FUZZ_CASES is not set; run Svc/WasmSequencer/test/e2e/fuzz.py\n");
        return 0;
    }
    std::string path(file);
    const size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        if (chdir(path.substr(0, slash).c_str()) != 0) {
            std::fprintf(stderr, "cannot enter the directory of %s\n", file);
            return 1;
        }
        path = path.substr(slash + 1);
    }
    std::string error;
    if (!Svc::WasmSeqE2e::loadCases(path, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    for (const Svc::WasmSeqE2e::Case& item : Svc::WasmSeqE2e::cases()) {
        ::testing::RegisterTest("fuzz", item.scenario.name.c_str(), nullptr, nullptr, __FILE__, __LINE__,
                                [=]() -> ::testing::Test* { return new Svc::WasmSeqE2e::Differential(item); });
    }
    return RUN_ALL_TESTS();
}
