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

#include "../app/FirstSnapshotNotice.h"
#include "../app/JobWords.h"

using namespace loopercat;
using lifecycle::State;

namespace {
const std::string kFirstSnapshot = "Record the card's first snapshot";
const std::string kCoreTail = " \xe2\x80\x94 refusing to touch the volume";
const std::string kStopped = "The card's first snapshot stopped; it will finish next time you connect.";
const std::string kDisconnected = "Pedal disconnected \xe2\x80\x94 back on the looper screen";
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
    // pedal away, and it does not say again that the pedal left — it follows
    // the departure's own sentence ---

    {
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const std::string words = jobwords::firstSnapshotInterrupted(JobOutcome::refused(state));
            CHECK_EQ(words, kStopped);
            CHECK(words.find("refusing") == std::string::npos);
            CHECK(words.find("error") == std::string::npos);
            CHECK(words.find("disconnected") == std::string::npos);
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

    // --- the notice: the interruption rides on the departure's toast, so it
    // is not replaced unread by it. An app-driven disconnect with an
    // interrupted snapshot says both sentences; one without says only its
    // own ---

    {
        FirstSnapshotNotice notice;
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
        CHECK(!notice.departure("").has_value()); // nothing happened, nothing to say

        notice.interrupted(JobOutcome::refused(State::ejecting));
        CHECK(notice.pending());
        const auto both = notice.departure(kDisconnected);
        CHECK(both.has_value());
        if (both) {
            CHECK_EQ(*both, kDisconnected + ". " + kStopped);
            CHECK(both->find("refusing to touch") == std::string::npos);
        }
        // Said once: a second disconnect without a new interruption does not
        // repeat it.
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
    }

    // --- where the departure was told another way (the lifecycle line, a
    // banner), the sentence stands alone — and it is the one sentence ---

    {
        FirstSnapshotNotice notice;
        notice.interrupted(JobOutcome::refused(State::ghost));
        const auto alone = notice.departure("");
        CHECK(alone.has_value());
        if (alone)
            CHECK_EQ(*alone, jobwords::firstSnapshotInterrupted(JobOutcome::refused(State::ghost)));
        CHECK(!notice.departure("").has_value());
    }

    // --- the next connect resumes the snapshot and forgets the notice: the
    // disconnect after it says only its own sentence ---

    {
        FirstSnapshotNotice notice;
        notice.interrupted(JobOutcome::refused(State::disconnected));
        notice.connectionStarted();
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
        // The newest interruption is the one told, never an older one twice.
        notice.interrupted(JobOutcome::refused(State::ejecting));
        notice.interrupted(JobOutcome::refused(State::ejected));
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected + ". " + kStopped));
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
    }

    // --- only a refused step is an interruption: a failure or a success
    // offered as one is a caller bug and leaves nothing pending ---

    {
        FirstSnapshotNotice notice;
        CHECK_THROWS(notice.interrupted(JobOutcome::failure("card changed")), "only a step the gate refused");
        CHECK_THROWS(notice.interrupted(JobOutcome::success()), "only a step the gate refused");
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
    }

    return testkit::summary("job_words");
}
