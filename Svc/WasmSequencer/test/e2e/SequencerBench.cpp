// ======================================================================
// \title  SequencerBench.cpp
// \brief  Drives Svc::CmdSequencer and Svc::WasmSequencer through the same
//         scenario on a simulated clock and records what each does
// ======================================================================

#include "Svc/WasmSequencer/test/e2e/SequencerBench.hpp"

#include <cstdio>
#include <sstream>

#include "Fw/Com/ComPacket.hpp"
#include "gtest/gtest.h"

namespace Svc {

// ----------------------------------------------------------------------
// Queue access
//
// Both sequencers are active components; the bench runs them without their
// threads, dispatching each queued message itself. Dispatching is protected,
// so it goes through the class each component already befriends for its unit
// tests. This executable links neither component's own unit tests, so the
// names are free here.
// ----------------------------------------------------------------------

class CmdSequencerTester {
  public:
    static void drain(CmdSequencerComponentImpl& component) {
        U32 dispatched = 0;
        while (component.m_queue.getMessagesAvailable() > 0) {
            ASSERT_LT(dispatched++, 100000u) << "CmdSequencer queue did not drain";
            (void)component.doDispatch();
        }
    }
};

class WasmSequencerTester {
  public:
    static void drain(WasmSequencer& component) {
        U32 dispatched = 0;
        while (component.m_queue.getMessagesAvailable() > 0) {
            ASSERT_LT(dispatched++, 100000u) << "WasmSequencer queue did not drain";
            (void)component.doDispatch();
        }
    }
};

namespace WasmSeqE2e {

namespace {

constexpr FwSizeType QUEUE_DEPTH = 32;

//! CmdSequencer's sequence buffer; the largest file it can load
constexpr FwSizeType CMD_SEQ_BUFFER_SIZE = 64 * 1024;

std::string hex(const std::vector<U8>& bytes) {
    std::string text;
    char digits[4];
    for (const U8 byte : bytes) {
        (void)std::snprintf(digits, sizeof digits, "%02X", byte);
        text += digits;
    }
    return text;
}

}  // namespace

// ----------------------------------------------------------------------
// Event
// ----------------------------------------------------------------------

const char* responseName(Fw::CmdResponse::T response) {
    switch (response) {
        case Fw::CmdResponse::OK:
            return "OK";
        case Fw::CmdResponse::INVALID_OPCODE:
            return "INVALID_OPCODE";
        case Fw::CmdResponse::VALIDATION_ERROR:
            return "VALIDATION_ERROR";
        case Fw::CmdResponse::FORMAT_ERROR:
            return "FORMAT_ERROR";
        case Fw::CmdResponse::EXECUTION_ERROR:
            return "EXECUTION_ERROR";
        case Fw::CmdResponse::BUSY:
            return "BUSY";
        default:
            return "UNKNOWN";
    }
}

bool Event::operator==(const Event& other) const {
    if (this->kind != other.kind || this->timeUs != other.timeUs) {
        return false;
    }
    switch (this->kind) {
        case COMMAND:
            return this->packet == other.packet;
        case SEQ_DONE:
            return this->response == other.response;
        default:
            return true;
    }
}

std::string describe(const Event& event) {
    std::ostringstream text;
    text << "t=" << (event.timeUs / 1000000) << "." << std::to_string(1000000 + event.timeUs % 1000000).substr(1)
         << "s ";
    switch (event.kind) {
        case Event::SEQ_START:
            text << "SEQ_START";
            break;
        case Event::COMMAND:
            text << "COMMAND " << hex(event.packet);
            break;
        case Event::SEQ_DONE:
            text << "SEQ_DONE " << responseName(event.response);
            break;
    }
    return text.str();
}

std::string describe(const Trace& trace) {
    std::string text;
    for (const Event& event : trace) {
        text += "    " + describe(event) + "\n";
    }
    return text.empty() ? "    (nothing)\n" : text;
}

// ----------------------------------------------------------------------
// Bench
// ----------------------------------------------------------------------

Bench::Bench(const char* name) : Fw::PassiveComponentBase(name) {
    this->init(0);
}

Bench::~Bench() {}

void Bench::initPorts() {
    this->m_commandIn.init();
    this->m_commandIn.addCallComp(this, commandIn);
    this->m_seqDoneIn.init();
    this->m_seqDoneIn.addCallComp(this, seqDoneIn);
    this->m_seqStartIn.init();
    this->m_seqStartIn.addCallComp(this, seqStartIn);
    this->m_timeIn.init();
    this->m_timeIn.addCallComp(this, timeIn);
#if FW_ENABLE_TEXT_LOGGING
    this->m_textIn.init();
    this->m_textIn.addCallComp(this, textIn);
#endif
}

Trace Bench::run(const char* file, const Scenario& scenario) {
    this->m_scenario = scenario;
    this->m_trace.clear();
    this->m_log.clear();
    this->m_elapsedUs = 0;
    this->m_now = scenario.start;
    this->m_pending = Pending();
    this->m_commandIndex = 0;
    this->m_finishedRuns = 0;
    bool cancelled = false;

    for (U32 run = 0; run < scenario.runs; run++) {
        this->m_done = false;
        this->invokeRun(file);
        this->drain();

        const U64 limit = this->m_elapsedUs + scenario.limitUs;
        while (true) {
            if (this->deliverDue()) {
                continue;
            }
            if (this->m_done) {
                this->m_finishedRuns++;
                break;
            }
            if (scenario.cancelAtUs >= 0 && !cancelled && this->m_elapsedUs >= static_cast<U64>(scenario.cancelAtUs)) {
                cancelled = true;
                this->invokeCancel();
                this->drain();
                continue;
            }
            if (this->m_elapsedUs >= limit) {
                break;
            }
            this->advance();
        }
        if (!this->m_done) {
            break;
        }

        if (scenario.cancelAfterDone && run + 1 == scenario.runs) {
            this->invokeCancel();
            this->drain();
        }

        // Keep the rate group running: nothing more may be dispatched for this run
        const U64 settled = this->m_elapsedUs + scenario.settleUs;
        while (this->m_elapsedUs < settled) {
            while (this->deliverDue()) {
            }
            this->advance();
        }
    }
    return this->m_trace;
}

void Bench::advance() {
    this->m_elapsedUs += this->m_scenario.tickUs;
    this->m_now = this->m_scenario.start;
    this->m_now.add(static_cast<U32>(this->m_elapsedUs / 1000000), static_cast<U32>(this->m_elapsedUs % 1000000));
    this->invokeTick();
    this->drain();
}

bool Bench::deliverDue() {
    if (!this->m_pending.active || this->m_pending.silent || this->m_elapsedUs < this->m_pending.dueUs) {
        return false;
    }
    const Pending pending = this->m_pending;
    this->m_pending.active = false;
    this->invokeResponse(pending.opcode, pending.context, pending.response);
    this->drain();
    return true;
}

void Bench::onCommand(const Fw::ComBuffer& packet, U32 context) {
    Event event{Event::COMMAND, this->m_elapsedUs, {}, Fw::CmdResponse::OK};
    event.packet.assign(packet.getBuffAddr(), packet.getBuffAddr() + packet.getSize());
    this->m_trace.push_back(event);

    // A sequencer has one command in flight at a time; a second before the
    // first is answered is itself a behavior worth failing on
    EXPECT_FALSE(this->m_pending.active && !this->m_pending.silent)
        << "command dispatched while another awaits its response: " << describe(event);

    Fw::ComBuffer copy(packet);
    FwPacketDescriptorType descriptor = 0;
    FwOpcodeType opcode = 0;
    EXPECT_EQ(copy.deserializeTo(descriptor), Fw::FW_SERIALIZE_OK);
    EXPECT_EQ(descriptor, static_cast<FwPacketDescriptorType>(Fw::ComPacketType::FW_PACKET_COMMAND));
    EXPECT_EQ(copy.deserializeTo(opcode), Fw::FW_SERIALIZE_OK);

    const I32 index = this->m_commandIndex++;
    this->m_pending.active = true;
    this->m_pending.silent = (index == this->m_scenario.silentAt);
    this->m_pending.opcode = opcode;
    this->m_pending.context = context;
    this->m_pending.dueUs = this->m_elapsedUs + this->m_scenario.latencyUs;
    this->m_pending.response = (index == this->m_scenario.failAt) ? this->m_scenario.failWith : Fw::CmdResponse::OK;
}

void Bench::onSeqStart() {
    this->m_trace.push_back(Event{Event::SEQ_START, this->m_elapsedUs, {}, Fw::CmdResponse::OK});
}

void Bench::onSeqDone(Fw::CmdResponse::T response) {
    this->m_trace.push_back(Event{Event::SEQ_DONE, this->m_elapsedUs, {}, response});
    this->m_done = true;
    // A command still awaiting its response belongs to the run that just ended;
    // its answer is still delivered, late, as a real dispatcher would
}

void Bench::onText(const Fw::TextLogString& text) {
    this->m_log += "    [" + std::to_string(this->m_elapsedUs) + "us] " + text.toChar() + "\n";
}

void Bench::commandIn(Fw::PassiveComponentBase* comp, FwIndexType, Fw::ComBuffer& data, U32 context) {
    static_cast<Bench*>(comp)->onCommand(data, context);
}

void Bench::seqDoneIn(Fw::PassiveComponentBase* comp, FwIndexType, FwOpcodeType, U32, const Fw::CmdResponse& response) {
    static_cast<Bench*>(comp)->onSeqDone(response.e);
}

void Bench::seqStartIn(Fw::PassiveComponentBase* comp, FwIndexType, const Fw::StringBase&, const Svc::SeqArgs&) {
    static_cast<Bench*>(comp)->onSeqStart();
}

void Bench::timeIn(Fw::PassiveComponentBase* comp, FwIndexType, Fw::Time& time) {
    static_cast<Bench*>(comp)->onTime(time);
}

#if FW_ENABLE_TEXT_LOGGING
void Bench::textIn(Fw::PassiveComponentBase* comp,
                   FwIndexType,
                   FwEventIdType,
                   Fw::Time&,
                   const Fw::LogSeverity&,
                   Fw::TextLogString& text) {
    static_cast<Bench*>(comp)->onText(text);
}
#endif

// ----------------------------------------------------------------------
// CmdSequencerBench
// ----------------------------------------------------------------------

CmdSequencerBench::CmdSequencerBench() : Bench("CmdSequencerBench"), m_sequencer("cmdSeq") {
    this->initPorts();
    this->m_sequencer.init(QUEUE_DEPTH, 0);
    this->m_sequencer.set_comCmdOut_OutputPort(0, &this->m_commandIn);
    this->m_sequencer.set_seqDone_OutputPort(0, &this->m_seqDoneIn);
    this->m_sequencer.set_seqStartOut_OutputPort(0, &this->m_seqStartIn);
    this->m_sequencer.set_timeCaller_OutputPort(0, &this->m_timeIn);
#if FW_ENABLE_TEXT_LOGGING
    this->m_sequencer.set_LogText_OutputPort(0, &this->m_textIn);
#endif
    this->m_sequencer.allocateBuffer(0, this->m_allocator, CMD_SEQ_BUFFER_SIZE);
    this->m_sequencer.setTimeout(COMMAND_TIMEOUT_S);
}

CmdSequencerBench::~CmdSequencerBench() {
    this->m_sequencer.deallocateBuffer(this->m_allocator);
    this->m_sequencer.deinit();
}

void CmdSequencerBench::invokeRun(const char* file) {
    this->m_sequencer.get_seqRunIn_InputPort(0)->invoke(Fw::String(file), Svc::SeqArgs());
}

void CmdSequencerBench::invokeTick() {
    this->m_sequencer.get_schedIn_InputPort(0)->invoke(0);
}

void CmdSequencerBench::invokeCancel() {
    this->m_sequencer.get_seqCancelIn_InputPort(0)->invoke();
}

void CmdSequencerBench::invokeResponse(FwOpcodeType opcode, U32 context, Fw::CmdResponse::T response) {
    this->m_sequencer.get_cmdResponseIn_InputPort(0)->invoke(opcode, context, Fw::CmdResponse(response));
}

void CmdSequencerBench::drain() {
    CmdSequencerTester::drain(this->m_sequencer);
}

// ----------------------------------------------------------------------
// WasmSequencerBench
// ----------------------------------------------------------------------

WasmSequencerBench::WasmSequencerBench() : Bench("WasmSequencerBench"), m_sequencer("wasmSeq") {
    this->initPorts();
    this->m_prmIn.init();
    this->m_prmIn.addCallComp(this, prmIn);
    this->m_getParamIn.init();
    this->m_getParamIn.addCallComp(this, prmIn);
    this->m_getTlmIn.init();
    this->m_getTlmIn.addCallComp(this, tlmIn);

    this->m_sequencer.init(QUEUE_DEPTH, 0);
    this->m_sequencer.set_cmdOut_OutputPort(0, &this->m_commandIn);
    this->m_sequencer.set_seqDoneOut_OutputPort(0, &this->m_seqDoneIn);
    this->m_sequencer.set_seqStartOut_OutputPort(0, &this->m_seqStartIn);
    this->m_sequencer.set_timeCaller_OutputPort(0, &this->m_timeIn);
    this->m_sequencer.set_prmGet_OutputPort(0, &this->m_prmIn);
    this->m_sequencer.set_getParam_OutputPort(0, &this->m_getParamIn);
    this->m_sequencer.set_getTlmChan_OutputPort(0, &this->m_getTlmIn);
#if FW_ENABLE_TEXT_LOGGING
    this->m_sequencer.set_logTextOut_OutputPort(0, &this->m_textIn);
#endif
    // As TestDeploymentsProject/Ref/Top/instances.fpp configures wasmSeq
    this->m_sequencer.configure(Svc::WasmSequencer::Config(), this->m_allocator);
    // No parameter database: every parameter takes its default, so
    // HOST_FUNCTION_TIMEOUT_SECS is the COMMAND_TIMEOUT_S CmdSequencer is given
    this->m_sequencer.loadParameters();
    this->drain();
}

WasmSequencerBench::~WasmSequencerBench() {
    this->m_sequencer.deinit();
}

void WasmSequencerBench::invokeRun(const char* file) {
    this->m_sequencer.get_seqRunIn_InputPort(0)->invoke(Fw::String(file), Svc::SeqArgs());
}

void WasmSequencerBench::invokeTick() {
    this->m_sequencer.get_checkTimers_InputPort(0)->invoke(0);
}

void WasmSequencerBench::invokeCancel() {
    this->m_sequencer.get_seqCancelIn_InputPort(0)->invoke();
}

void WasmSequencerBench::invokeResponse(FwOpcodeType opcode, U32 context, Fw::CmdResponse::T response) {
    this->m_sequencer.get_cmdResponseIn_InputPort(0)->invoke(opcode, context, Fw::CmdResponse(response));
}

void WasmSequencerBench::drain() {
    WasmSequencerTester::drain(this->m_sequencer);
}

Fw::ParamValid WasmSequencerBench::prmIn(Fw::PassiveComponentBase*, FwIndexType, FwPrmIdType, Fw::ParamBuffer&) {
    return Fw::ParamValid::INVALID;
}

Fw::TlmValid WasmSequencerBench::tlmIn(Fw::PassiveComponentBase*,
                                       FwIndexType,
                                       FwChanIdType,
                                       Fw::Time&,
                                       Fw::TlmBuffer&) {
    return Fw::TlmValid::INVALID;
}

}  // namespace WasmSeqE2e
}  // namespace Svc
