// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "JobOutcome.h"

#include <loopercat/Error.hpp>
#include <loopercat/Lifecycle.hpp>
#include <loopercat/Rc0.hpp>

#include <string>

//==============================================================================
// loopercat::jobwords — what a job's ending says to the player, and what it
// says to the operations log (issue #146).
//
// Before this the banner pasted `description + ": " + error` for every
// ending, so a job the lifecycle gate refused read "Push loop.wav to slot 3:
// pedal is ejecting — refusing to touch the volume": the core's sentence,
// right for a log, wrong for a window. Here a refusal becomes "<job> did not
// run: <why, in the player's words>", one wording per state the gate refuses
// in; a failure of the job's own work keeps the shape it always had. The
// first snapshot has its own two sentences: the interruption it resumes from
// is information for the toast and an engineering line for the log, never a
// banner.
//
// JUCE-free on purpose: tested by theory in job_words_tests and banner_tests.
//==============================================================================
namespace loopercat::jobwords {

// Why the gate refused, told to a musician. The lifecycle line in
// BannerModel.h already explains ghost and ejected in these words.
inline std::string gateReason(lifecycle::State state)
{
    switch (state) {
    case lifecycle::State::ejecting:
        return "the pedal was being disconnected";
    case lifecycle::State::disconnected:
        return "no pedal is connected";
    case lifecycle::State::ghost:
        return "the pedal left without an eject";
    case lifecycle::State::ejected:
        return "the card was already ejected";
    case lifecycle::State::connected:
        throw Error("the gate never refuses a job while the pedal is connected");
    }
    throw Error("unknown lifecycle state");
}

// The banner's line for a job that did not succeed. A job that did has no
// line here, and asking for one is a caller bug — the banner must never show
// an empty or a made-up sentence.
inline std::string banner(const std::string& description, const JobOutcome& outcome)
{
    if (description.empty())
        throw Error("a job's banner line needs the job's description");
    if (outcome.ok())
        throw Error("the banner is for a job that did not succeed: " + description);
    if (outcome.refusedAtGate())
        return description + " did not run: " + gateReason(*outcome.refusedIn);
    return description + ": " + outcome.error;
}

// The operations log's line for a job the gate refused: the core's sentence,
// kept where it belongs.
inline std::string refusalLog(const std::string& description, const JobOutcome& outcome)
{
    if (description.empty())
        throw Error("a refusal's log line needs the job's description");
    if (!outcome.refusedAtGate())
        throw Error("only a job the gate refused has a refusal to log: " + description);
    return description + " did not run: " + outcome.error;
}

// The first snapshot is taken one slot per job and resumes on the next
// connect from the slot it reached, so a step the gate refused is an
// interruption, not a fault: one quiet sentence saying what happened and what
// happens next. It follows the departure's own sentence on the toast
// (FirstSnapshotNotice), so it does not say again that the pedal left. A step
// whose own work failed is a failure and takes the banner's words — asking
// for the interruption's words for it is a caller bug.
inline std::string firstSnapshotInterrupted(const JobOutcome& outcome)
{
    if (!outcome.refusedAtGate())
        throw Error("only a step the gate refused interrupts the first snapshot; "
                    "a failed step has the banner's words");
    return "The card's first snapshot stopped; it will finish next time you connect.";
}

// The same interruption for the operations log: which slot the run stopped
// at, and the core's sentence.
inline std::string firstSnapshotInterruptedLog(int slot, const JobOutcome& outcome)
{
    if (slot < 1 || slot > rc0::kSlotCount)
        throw Error("the first snapshot has no slot " + std::to_string(slot));
    if (!outcome.refusedAtGate())
        throw Error("only a step the gate refused interrupts the first snapshot");
    return "first snapshot interrupted at slot " + std::to_string(slot) + ": " + outcome.error;
}

} // namespace loopercat::jobwords
