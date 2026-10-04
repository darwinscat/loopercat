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
// to succeed, and they are not the same event: `work` can fail, or the
// lifecycle gate can refuse to run the job at all. A refusal ran nothing and
// touched nothing on the card, so the first snapshot treats it as the
// interruption it resumes from and the player hears "did not run" rather
// than the core's sentence (issue #146). The core's sentence stays as the
// log text in both cases — the gate's words are right for a log and wrong
// for a window.
//
// JUCE-free on purpose: the words for an outcome (JobWords.h) are tested by
// theory without a window or a worker thread.
//==============================================================================
namespace loopercat {

struct JobOutcome {
    std::string error;                         // empty = the job succeeded
    std::optional<lifecycle::State> refusedIn; // the gate's state when it refused;
                                               // unset = the job ran (and may have failed)

    static JobOutcome success() { return {}; }

    // The gate refuses in every state but connected (lifecycle::Machine::
    // writable); being asked to refuse while connected is a caller bug.
    static JobOutcome refused(lifecycle::State state)
    {
        if (state == lifecycle::State::connected)
            throw Error("the gate never refuses a job while the pedal is connected");
        JobOutcome outcome;
        outcome.error = std::string("pedal is ") + lifecycle::stateName(state)
                      + " \xe2\x80\x94 refusing to touch the volume";
        outcome.refusedIn = state;
        return outcome;
    }

    // A failure with no words would read as a success (ok() is the empty
    // error) — the type refuses the contradiction.
    static JobOutcome failure(std::string what)
    {
        if (what.empty())
            throw Error("a job failure needs its reason");
        JobOutcome outcome;
        outcome.error = std::move(what);
        return outcome;
    }

    bool ok() const { return error.empty(); }
    bool refusedAtGate() const { return refusedIn.has_value(); }
    bool failed() const { return !ok() && !refusedAtGate(); }

    // Whether the ending is told at all (PedalWorker::onJobResult). A job the
    // player asked for tells every ending. A quiet job — one nobody asked to
    // be told about — tells only its failures, and a refusal at the gate is
    // not one: nothing ran, so there is nothing to report.
    bool told(bool quiet) const { return !quiet || failed(); }

    bool operator==(const JobOutcome&) const = default;
};

} // namespace loopercat
