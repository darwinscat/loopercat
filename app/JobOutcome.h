// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>
#include <loopercat/Lifecycle.hpp>

#include <optional>
#include <string>
#include <utility>

//==============================================================================
// loopercat::JobOutcome — how a PedalWorker job ended. There are two ways not
// to succeed, and they are not the same event: `work` can fail, or the job
// can be refused before it runs — by the lifecycle gate, or because the
// volume the last scan saw is no longer there. A refusal ran nothing and
// touched nothing on the card, so the first snapshot treats it as the
// interruption it resumes from and the player hears "did not run" rather
// than the core's sentence (issue #146). The core's sentence stays as the
// log text in both cases — the gate's words are right for a log and wrong
// for a window. The history's own failure — a throw from the job's `after`
// — travels beside the job's ending, so a refusal stays a refusal even when
// the bookkeeping of it failed.
//
// Only the factories build one, so an outcome is always one of these
// endings and never, say, a success refused at once. JUCE-free on purpose:
// the words for an outcome (JobWords.h) are tested by theory without a
// window or a worker thread.
//==============================================================================
namespace loopercat {

class JobOutcome {
public:
    // Why a job did not run. The first four are the lifecycle gate's states
    // (every state but connected — lifecycle::Machine::writable); the last
    // is past the gate: no pedal volume to run the job on, the one the last
    // scan saw having gone between then and now.
    enum class Refusal { ejecting, disconnected, ghost, ejected, noVolume };

    static JobOutcome success() { return {}; }

    // The gate refuses in every state but connected; being asked to refuse
    // while connected is a caller bug.
    static JobOutcome refused(lifecycle::State state)
    {
        JobOutcome outcome;
        outcome.refusal_ = refusalFor(state);
        outcome.error_ = coreSentence(*outcome.refusal_);
        return outcome;
    }

    static JobOutcome unmounted()
    {
        JobOutcome outcome;
        outcome.refusal_ = Refusal::noVolume;
        outcome.error_ = coreSentence(Refusal::noVolume);
        return outcome;
    }

    // A failure with no words would read as a success — the type refuses
    // the contradiction.
    static JobOutcome failure(std::string what)
    {
        if (what.empty())
            throw Error("a job failure needs its reason");
        JobOutcome outcome;
        outcome.error_ = std::move(what);
        return outcome;
    }

    // The job's `after` threw: the history could not record the ending,
    // whatever it was. The ending itself stays — a refusal is still a
    // refusal — and the ending is told whoever asked.
    void historyThrew(std::string what)
    {
        if (what.empty())
            throw Error("a history failure needs its reason");
        history_ = std::move(what);
    }

    // The core's sentence for a failure or a refusal — the log's text; empty
    // when the job's own work succeeded or never ran into trouble.
    const std::string& error() const { return error_; }
    std::optional<Refusal> refusal() const { return refusal_; }
    const std::string& historyError() const { return history_; }

    bool ok() const { return error_.empty() && history_.empty(); }
    bool didNotRun() const { return refusal_.has_value(); }
    bool failed() const { return !didNotRun() && !ok(); }
    bool historyFailed() const { return !history_.empty(); }

    // Whether the ending is told at all (PedalWorker::onJobResult). A job the
    // player asked for tells every ending. A quiet job — one nobody asked to
    // be told about — tells only its failures: a refusal ran nothing, so
    // there is nothing to report, unless the history failed to record it.
    bool told(bool quiet) const { return !quiet || failed() || historyFailed(); }

    // The core's words, as the worker always said them.
    static std::string coreSentence(Refusal refusal)
    {
        switch (refusal) {
        case Refusal::ejecting:
        case Refusal::disconnected:
        case Refusal::ghost:
        case Refusal::ejected:
            return std::string("pedal is ") + lifecycle::stateName(stateOf(refusal))
                 + " \xe2\x80\x94 refusing to touch the volume";
        case Refusal::noVolume:
            return "no pedal volume mounted";
        }
        throw Error("unknown refusal");
    }

    bool operator==(const JobOutcome&) const = default;

private:
    JobOutcome() = default;

    static Refusal refusalFor(lifecycle::State state)
    {
        switch (state) {
        case lifecycle::State::ejecting: return Refusal::ejecting;
        case lifecycle::State::disconnected: return Refusal::disconnected;
        case lifecycle::State::ghost: return Refusal::ghost;
        case lifecycle::State::ejected: return Refusal::ejected;
        case lifecycle::State::connected:
            throw Error("the gate never refuses a job while the pedal is connected");
        }
        throw Error("unknown lifecycle state");
    }

    static lifecycle::State stateOf(Refusal refusal)
    {
        switch (refusal) {
        case Refusal::ejecting: return lifecycle::State::ejecting;
        case Refusal::disconnected: return lifecycle::State::disconnected;
        case Refusal::ghost: return lifecycle::State::ghost;
        case Refusal::ejected: return lifecycle::State::ejected;
        case Refusal::noVolume:
            throw Error("a missing volume is not a lifecycle state");
        }
        throw Error("unknown refusal");
    }

    std::string error_;               // the core's sentence; empty = the work succeeded or never ran into trouble
    std::optional<Refusal> refusal_;  // set = the job did not run
    std::string history_;             // what `after` threw; empty = the history recorded the ending
};

} // namespace loopercat
