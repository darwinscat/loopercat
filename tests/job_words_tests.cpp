// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A job's ending and its words, tested from the theory of issue #146 — not
// from the implementation. The gate's refusal is a distinct outcome: nothing
// ran, and the player hears that the job did not run, in a sentence about the
// pedal; a failure of the job's own work keeps the shape it always had; the
// first snapshot's interruption is information, with the core's sentence
// kept for the log. Every contradiction — a banner for a success, an
// interruption out of a failure, a refusal while connected — is a caller bug
// and throws rather than producing an empty or made-up line.

#include "support.hpp"

#include "../app/JobWords.h"

using namespace loopercat;
using lifecycle::State;

namespace {
const std::string kFirstSnapshot = "Record the card's first snapshot";
const std::string kCoreTail = " \xe2\x80\x94 refusing to touch the volume";
} // namespace

int main()
{
    // --- the outcome itself: three endings, each knowing what it is ---

    {
        const JobOutcome ok = JobOutcome::success();
        CHECK(ok.ok());
        CHECK(!ok.refusedAtGate());
        CHECK(!ok.failed());
        CHECK(ok.error.empty());

        const JobOutcome failed = JobOutcome::failure("disk full");
        CHECK(!failed.ok());
        CHECK(!failed.refusedAtGate());
        CHECK(failed.failed());
        CHECK_EQ(failed.error, "disk full");

        const JobOutcome refused = JobOutcome::refused(State::ejecting);
        CHECK(!refused.ok());
        CHECK(refused.refusedAtGate());
        CHECK(!refused.failed());
        CHECK(refused.refusedIn == State::ejecting);
        // The core's sentence survives as the log text, word for word.
        CHECK_EQ(refused.error, "pedal is ejecting" + kCoreTail);
        CHECK_EQ(JobOutcome::refused(State::ghost).error, "pedal is ghost" + kCoreTail);
        CHECK_EQ(JobOutcome::refused(State::disconnected).error, "pedal is disconnected" + kCoreTail);
        CHECK_EQ(JobOutcome::refused(State::ejected).error, "pedal is ejected" + kCoreTail);
    }

    // --- contradictions are caller bugs ---

    {
        CHECK_THROWS(JobOutcome::refused(State::connected), "never refuses");
        CHECK_THROWS(JobOutcome::failure(""), "needs its reason");
    }

    // --- who is told: the player's own job every time, a quiet job only when
    // its work failed — a refusal ran nothing ---

    {
        CHECK(JobOutcome::success().told(false));
        CHECK(JobOutcome::failure("x").told(false));
        CHECK(JobOutcome::refused(State::ejecting).told(false));
        CHECK(!JobOutcome::success().told(true));
        CHECK(JobOutcome::failure("x").told(true));
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected })
            CHECK(!JobOutcome::refused(state).told(true));
    }

    // --- the banner: a refusal in every state the gate refuses in reads as
    // "did not run", about the pedal, never the core's sentence ---

    {
        const std::string push = "Push loop.wav to slot 3";
        CHECK_EQ(jobwords::banner(push, JobOutcome::refused(State::ejecting)),
                 "Push loop.wav to slot 3 did not run: the pedal was being disconnected");
        CHECK_EQ(jobwords::banner(push, JobOutcome::refused(State::disconnected)),
                 "Push loop.wav to slot 3 did not run: no pedal is connected");
        CHECK_EQ(jobwords::banner(push, JobOutcome::refused(State::ghost)),
                 "Push loop.wav to slot 3 did not run: the pedal left without an eject");
        CHECK_EQ(jobwords::banner(push, JobOutcome::refused(State::ejected)),
                 "Push loop.wav to slot 3 did not run: the card was already ejected");
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const JobOutcome refused = JobOutcome::refused(state);
            const std::string line = jobwords::banner(push, refused);
            CHECK(line.find("refusing to touch") == std::string::npos);
            CHECK(line.find(refused.error) == std::string::npos); // the core's sentence stays in the log
        }
        CHECK_THROWS(jobwords::gateReason(State::connected), "never refuses");
    }

    // --- a failure of the job's own work keeps today's shape ---

    {
        CHECK_EQ(jobwords::banner("Trim slot 5", JobOutcome::failure("file vanished")),
                 "Trim slot 5: file vanished");
        CHECK_EQ(jobwords::banner(kFirstSnapshot,
                                  JobOutcome::failure("the mounted volume changed before its first snapshot finished")),
                 "Record the card's first snapshot: the mounted volume changed before its first snapshot finished");
    }

    // --- no line for a success, none without a description: a caller bug,
    // not an empty banner ---

    {
        CHECK_THROWS(jobwords::banner("Trim slot 5", JobOutcome::success()), "did not succeed");
        CHECK_THROWS(jobwords::banner("", JobOutcome::failure("x")), "needs the job's description");
        CHECK_THROWS(jobwords::banner("", JobOutcome::refused(State::ghost)), "needs the job's description");
    }

    // --- the log keeps the core's sentence for a refusal, and only for one ---

    {
        CHECK_EQ(jobwords::refusalLog("Trim slot 5", JobOutcome::refused(State::ejecting)),
                 "Trim slot 5 did not run: pedal is ejecting" + kCoreTail);
        CHECK_THROWS(jobwords::refusalLog("Trim slot 5", JobOutcome::failure("x")), "only a job the gate refused");
        CHECK_THROWS(jobwords::refusalLog("Trim slot 5", JobOutcome::success()), "only a job the gate refused");
        CHECK_THROWS(jobwords::refusalLog("", JobOutcome::refused(State::ghost)), "needs the job's description");
    }

    // --- the first snapshot: a refused step is an interruption, said once,
    // quietly, with what happens next; the same sentence whatever took the
    // pedal away ---

    {
        const std::string expected = "The card's first snapshot stopped when the pedal was disconnected "
                                     "\xe2\x80\x94 it will finish next time you connect.";
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const std::string words = jobwords::firstSnapshotInterrupted(JobOutcome::refused(state));
            CHECK_EQ(words, expected);
            CHECK(words.find("refusing") == std::string::npos);
            CHECK(words.find("error") == std::string::npos);
        }
        // A step that failed on its own is a failure and has no such sentence.
        CHECK_THROWS(jobwords::firstSnapshotInterrupted(JobOutcome::failure("card changed")), "failed step");
        CHECK_THROWS(jobwords::firstSnapshotInterrupted(JobOutcome::success()), "failed step");
    }

    // --- its log line: the slot the run stopped at and the core's sentence ---

    {
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(14, JobOutcome::refused(State::ejecting)),
                 "first snapshot interrupted at slot 14: pedal is ejecting" + kCoreTail);
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(1, JobOutcome::refused(State::ghost)),
                 "first snapshot interrupted at slot 1: pedal is ghost" + kCoreTail);
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(99, JobOutcome::refused(State::disconnected)),
                 "first snapshot interrupted at slot 99: pedal is disconnected" + kCoreTail);
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(0, JobOutcome::refused(State::ejecting)), "no slot 0");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(100, JobOutcome::refused(State::ejecting)), "no slot 100");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(14, JobOutcome::failure("card changed")),
                     "only a step the gate refused");
    }

    return testkit::summary("job_words");
}
