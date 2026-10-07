// ======================================================================
// \title  SequencerBench.hpp
// \brief  Drives Svc::CmdSequencer and Svc::WasmSequencer through the same
//         scenario on a simulated clock and records what each does
// ======================================================================

#ifndef Svc_WasmSequencer_E2e_SequencerBench_HPP
#define Svc_WasmSequencer_E2e_SequencerBench_HPP

#include <string>
#include <vector>

#include "Fw/Comp/PassiveComponentBase.hpp"
#include "Fw/Types/MallocAllocator.hpp"
#include "Svc/CmdSequencer/CmdSequencerImpl.hpp"
#include "Svc/WasmSequencer/WasmSequencer.hpp"

namespace Svc {
namespace WasmSeqE2e {

//! Something a sequencer did, as seen at its output ports
struct Event {
    enum Kind : U8 {
        SEQ_START,  //!< seqStartOut
        COMMAND,    //!< A command sent to the dispatcher
        SEQ_DONE,   //!< seqDone / seqDoneOut
    };

    Kind kind;
    U64 timeUs;                   //!< When, in microseconds after the scenario's start time
    std::vector<U8> packet;       //!< COMMAND: the Fw::Com packet, descriptor included
    Fw::CmdResponse::T response;  //!< SEQ_DONE: the response reported

    bool operator==(const Event& other) const;
    bool operator!=(const Event& other) const { return !(*this == other); }
};

using Trace = std::vector<Event>;

//! The constant's name, for failure messages and test names
const char* responseName(Fw::CmdResponse::T response);

//! One line per event, for failure messages
std::string describe(const Event& event);
std::string describe(const Trace& trace);

//! How the world around the sequencer behaves for one run
struct Scenario {
    std::string name;

    //! Clock reading when the sequence is started
    Fw::Time start = Fw::Time(TimeBase::TB_WORKSTATION_TIME, 0, 1767268800, 0);  // 2026-001T12:00:00Z

    //! Period of the rate group driving schedIn / checkTimers
    U32 tickUs = 1000;

    //! Time the dispatcher takes to answer each command
    U32 latencyUs = 0;

    //! Index of the command (counted across runs, from 0) answered with failWith; -1 for none
    I32 failAt = -1;
    Fw::CmdResponse::T failWith = Fw::CmdResponse::EXECUTION_ERROR;

    //! Index of the command that is never answered; -1 for none
    I32 silentAt = -1;

    //! Offset after the start at which seqCancelIn is invoked; -1 for never
    I64 cancelAtUs = -1;

    //! Invoke seqCancelIn once the (last) run has finished, with nothing running
    bool cancelAfterDone = false;

    //! Number of times the sequence is run back to back on the same instance
    U32 runs = 1;

    //! How long to keep the clock running after a run finishes, watching for stray commands
    U64 settleUs = 5000000;

    //! Give up on a run that has not finished after this long
    U64 limitUs = 3600ull * 1000000ull;
};

//! A sequencer instance wired to a simulated dispatcher, clock and rate group.
//!
//! The bench owns the ports the sequencer's outputs are connected to. Every
//! stimulus (run, tick, response, cancel) is followed by draining the
//! sequencer's queue, so the run is single threaded and deterministic: two
//! sequencers given the same scenario see the same clock readings at the same
//! points, and any difference in what they do is a difference in behavior.
class Bench : public Fw::PassiveComponentBase {
  public:
    //! Time both sequencers are configured to wait for a command response
    static constexpr U32 COMMAND_TIMEOUT_S = 60;

    explicit Bench(const char* name);
    virtual ~Bench();

    //! Run `file` (a path the sequencer can open) through `scenario`
    Trace run(const char* file, const Scenario& scenario);

    //! Text events the sequencer emitted during the last run, for failure messages
    const std::string& log() const { return this->m_log; }

    //! Whether the last run finished every requested run within the scenario's limit
    bool finished() const { return this->m_finishedRuns == this->m_scenario.runs; }

  protected:
    // ----------------------------------------------------------------------
    // What a concrete bench provides
    // ----------------------------------------------------------------------

    virtual void invokeRun(const char* file) = 0;
    virtual void invokeTick() = 0;
    virtual void invokeCancel() = 0;
    virtual void invokeResponse(FwOpcodeType opcode, U32 context, Fw::CmdResponse::T response) = 0;

    //! Dispatch everything on the sequencer's queue
    virtual void drain() = 0;

    // ----------------------------------------------------------------------
    // Handlers shared by both benches
    // ----------------------------------------------------------------------

    void onCommand(const Fw::ComBuffer& packet, U32 context);
    void onSeqStart();
    void onSeqDone(Fw::CmdResponse::T response);
    void onTime(Fw::Time& time) const { time = this->m_now; }
    void onText(const Fw::TextLogString& text);

    //! Connect the standard input ports the concrete bench's sequencer drives
    void initPorts();

    Fw::InputComPort m_commandIn;
    Fw::InputCmdResponsePort m_seqDoneIn;
    Svc::InputCmdSeqInPort m_seqStartIn;
    Fw::InputTimePort m_timeIn;
#if FW_ENABLE_TEXT_LOGGING
    Fw::InputLogTextPort m_textIn;
#endif

    Fw::MallocAllocator m_allocator;

  private:
    struct Pending {
        bool active = false;
        bool silent = false;
        FwOpcodeType opcode = 0;
        U32 context = 0;
        U64 dueUs = 0;
        Fw::CmdResponse::T response = Fw::CmdResponse::OK;
    };

    void advance();
    bool deliverDue();

    static void commandIn(Fw::PassiveComponentBase* comp, FwIndexType, Fw::ComBuffer& data, U32 context);
    static void seqDoneIn(Fw::PassiveComponentBase* comp,
                          FwIndexType,
                          FwOpcodeType,
                          U32,
                          const Fw::CmdResponse& response);
    static void seqStartIn(Fw::PassiveComponentBase* comp, FwIndexType, const Fw::StringBase&, const Svc::SeqArgs&);
    static void timeIn(Fw::PassiveComponentBase* comp, FwIndexType, Fw::Time& time);
#if FW_ENABLE_TEXT_LOGGING
    static void textIn(Fw::PassiveComponentBase* comp,
                       FwIndexType,
                       FwEventIdType,
                       Fw::Time&,
                       const Fw::LogSeverity&,
                       Fw::TextLogString& text);
#endif

    Scenario m_scenario;
    Trace m_trace;
    std::string m_log;
    Fw::Time m_now;
    U64 m_elapsedUs = 0;
    Pending m_pending;
    I32 m_commandIndex = 0;
    bool m_done = false;
    U32 m_finishedRuns = 0;
};

//! Svc::CmdSequencer configured as a deployment would run it
class CmdSequencerBench final : public Bench {
  public:
    CmdSequencerBench();
    ~CmdSequencerBench();

  private:
    void invokeRun(const char* file) override;
    void invokeTick() override;
    void invokeCancel() override;
    void invokeResponse(FwOpcodeType opcode, U32 context, Fw::CmdResponse::T response) override;
    void drain() override;

    Svc::CmdSequencerComponentImpl m_sequencer;
};

//! Svc::WasmSequencer configured as the Ref deployment runs it
class WasmSequencerBench final : public Bench {
  public:
    WasmSequencerBench();
    ~WasmSequencerBench();

  private:
    void invokeRun(const char* file) override;
    void invokeTick() override;
    void invokeCancel() override;
    void invokeResponse(FwOpcodeType opcode, U32 context, Fw::CmdResponse::T response) override;
    void drain() override;

    static Fw::ParamValid prmIn(Fw::PassiveComponentBase*, FwIndexType, FwPrmIdType, Fw::ParamBuffer&);
    static Fw::TlmValid tlmIn(Fw::PassiveComponentBase*, FwIndexType, FwChanIdType, Fw::Time&, Fw::TlmBuffer&);

    Fw::InputPrmGetPort m_prmIn;
    Fw::InputPrmGetPort m_getParamIn;
    Fw::InputTlmGetPort m_getTlmIn;

    Svc::WasmSequencer m_sequencer;
};

}  // namespace WasmSeqE2e
}  // namespace Svc

#endif
