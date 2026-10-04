// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "JobOutcome.h"
#include "JobWords.h"

#include <loopercat/Error.hpp>
#include <loopercat/Lifecycle.hpp>

#include <optional>
#include <string>

//==============================================================================
// loopercat::FirstSnapshotNotice — the first snapshot's interruption, kept
// until the pedal's departure is told (issue #146).
//
// The interruption comes in one of two ways. The refused step's own
// completion is one; the other is the scan that sees the pedal gone while
// the snapshot is still in flight — the owner then cancels the run and that
// step's completion is dropped, so the departure is where the interruption
// is decided, and the only place it can be for a Finder eject or a yank the
// probe sees first. Whichever comes first logs it; the second is not news.
//
// The step the gate refuses ends while the lifecycle is still `ejecting`, a
// second or so before the disconnect's own toast; a toast then is replaced
// unread. So the interruption waits here and rides on the departure's last
// word — or stands alone where the departure was told another way (the
// lifecycle line, a banner). Said once: telling it forgets it. A new
// connection resumes the snapshot, so it forgets too.
//
// JUCE-free on purpose: the toast's words and the two ways in are tested by
// theory in job_words_tests, without a window.
//==============================================================================
namespace loopercat {

class FirstSnapshotNotice {
public:
    // The refused step's own completion, for `slot`. Returns the operations
    // log's line, or nothing when the interruption is already known. Only a
    // step that did not run is an interruption; a failure has the banner's
    // words and is a caller bug here.
    std::optional<std::string> interrupted(const JobOutcome& outcome, int slot)
    {
        if (!outcome.didNotRun())
            throw Error("only a step that did not run interrupts the first snapshot");
        if (pending_)
            return std::nullopt;
        // The line is made before the notice is kept: a slot the snapshot
        // has no words for is a caller bug, and must not leave one pending.
        std::string line = jobwords::firstSnapshotInterruptedLog(slot, outcome);
        pending_ = outcome;
        return line;
    }

    // The scan that sees the pedal gone — `state` is what it saw — while
    // the snapshot is in flight at `slotReached` slots. The step for the
    // next slot is what the gate refuses, or would have. Returns the
    // operations log's line, or nothing: no run in flight, the interruption
    // already known, or a scan that saw the pedal still connected (the card
    // unreadable, say — not a departure, and the status line says so).
    std::optional<std::string> departed(lifecycle::State state, bool inFlight, int slotReached)
    {
        if (!inFlight || state == lifecycle::State::connected)
            return std::nullopt;
        return interrupted(JobOutcome::refused(state), slotReached + 1);
    }

    void connectionStarted() { pending_.reset(); }

    bool pending() const { return pending_.has_value(); }

    // The toast's words for the departure: `told` (empty = the story was
    // told another way) with the interruption appended, or nothing at all
    // when there is nothing to say.
    std::optional<std::string> departure(const std::string& told)
    {
        std::optional<std::string> words;
        if (pending_) {
            const std::string stopped = jobwords::firstSnapshotInterrupted(*pending_);
            if (told.empty())
                words = stopped;
            else
                words = told + (told.back() == '.' ? " " : ". ") + stopped;
        } else if (!told.empty()) {
            words = told;
        }
        pending_.reset();
        return words;
    }

private:
    std::optional<JobOutcome> pending_;
};

} // namespace loopercat
