// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "JobOutcome.h"
#include "JobWords.h"

#include <loopercat/Error.hpp>

#include <optional>
#include <string>

//==============================================================================
// loopercat::FirstSnapshotNotice — the first snapshot's interruption, kept
// until the pedal's departure is told (issue #146).
//
// The step the gate refuses ends while the lifecycle is still `ejecting`, a
// second or so before the disconnect's own toast; a toast then is replaced
// unread. So the interruption waits here and rides on the departure's last
// word — or stands alone where the departure was told another way (the
// lifecycle line, a banner). Said once: telling it forgets it. A new
// connection resumes the snapshot, so it forgets too.
//
// JUCE-free on purpose: the toast's words are tested by theory in
// job_words_tests, without a window.
//==============================================================================
namespace loopercat {

class FirstSnapshotNotice {
public:
    // Only a step the gate refused is an interruption; a failure has the
    // banner's words and is a caller bug here.
    void interrupted(const JobOutcome& outcome)
    {
        if (!outcome.refusedAtGate())
            throw Error("only a step the gate refused interrupts the first snapshot");
        pending_ = outcome;
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
            words = told.empty() ? stopped : told + ". " + stopped;
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
