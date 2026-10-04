// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A job's ending and its words, tested from the theory of issue #146 — not
// from the implementation. A refusal is a distinct outcome: nothing ran, and
// the player hears that the job did not run, in a sentence about the pedal;
// a failure of the job's own work keeps the shape it always had; a history
// that could not record the ending says so beside it; the first snapshot's
// interruption is information, with the core's sentence kept for the log.
// Every contradiction — a banner for a success, an interruption out of a
// failure, a refusal while connected, a failure with no words — is a caller
// bug and throws rather than producing an empty or made-up line.

#include "support.hpp"

#include "../app/FirstSnapshotNotice.h"
#include "../app/JobWords.h"

using namespace loopercat;
using lifecycle::State;
using Refusal = JobOutcome::Refusal;

namespace {
const std::string kFirstSnapshot = "Record the card's first snapshot";
const std::string kCoreTail = " \xe2\x80\x94 refusing to touch the volume";
const std::string kStopped = "The card's first snapshot stopped; it will finish next time you connect.";
const std::string kDisconnected = "Pedal disconnected \xe2\x80\x94 back on the looper screen";
const std::string kHistory = "the history could not record it: ";
} // namespace

int main()
{
    // --- the outcome itself: the endings, each knowing what it is ---

    {
        const JobOutcome ok = JobOutcome::success();
        CHECK(ok.ok());
        CHECK(!ok.didNotRun());
        CHECK(!ok.failed());
        CHECK(!ok.historyFailed());
        CHECK(ok.error().empty());
        CHECK(!ok.refusal().has_value());

        const JobOutcome failed = JobOutcome::failure("disk full");
        CHECK(!failed.ok());
        CHECK(!failed.didNotRun());
        CHECK(failed.failed());
        CHECK_EQ(failed.error(), "disk full");

        const JobOutcome refused = JobOutcome::refused(State::ejecting);
        CHECK(!refused.ok());
        CHECK(refused.didNotRun());
        CHECK(!refused.failed());
        CHECK(refused.refusal() == Refusal::ejecting);
        // The core's sentence survives as the log text, word for word.
        CHECK_EQ(refused.error(), "pedal is ejecting" + kCoreTail);
        CHECK_EQ(JobOutcome::refused(State::ghost).error(), "pedal is ghost" + kCoreTail);
        CHECK(JobOutcome::refused(State::ghost).refusal() == Refusal::ghost);
        CHECK_EQ(JobOutcome::refused(State::disconnected).error(), "pedal is disconnected" + kCoreTail);
        CHECK(JobOutcome::refused(State::disconnected).refusal() == Refusal::disconnected);
        CHECK_EQ(JobOutcome::refused(State::ejected).error(), "pedal is ejected" + kCoreTail);
        CHECK(JobOutcome::refused(State::ejected).refusal() == Refusal::ejected);

        // Past the gate, the volume the last scan saw has gone: nothing ran
        // either — a refusal, with the worker's words of old for the log.
        const JobOutcome unmounted = JobOutcome::unmounted();
        CHECK(unmounted.didNotRun());
        CHECK(!unmounted.failed());
        CHECK(!unmounted.ok());
        CHECK(unmounted.refusal() == Refusal::noVolume);
        CHECK_EQ(unmounted.error(), "no pedal volume mounted");
        CHECK(unmounted != refused);
    }

    // --- contradictions are caller bugs ---

    {
        CHECK_THROWS(JobOutcome::refused(State::connected), "never refuses");
        CHECK_THROWS(JobOutcome::failure(""), "needs its reason");
        JobOutcome outcome = JobOutcome::success();
        CHECK_THROWS(outcome.historyThrew(""), "needs its reason");
        CHECK(outcome.ok());
    }

    // --- the history's own failure travels beside the ending and changes
    // none of it: a refusal stays a refusal, a success is no longer ok ---

    {
        JobOutcome refused = JobOutcome::refused(State::ghost);
        refused.historyThrew("database is locked");
        CHECK(refused.didNotRun());
        CHECK(refused.refusal() == Refusal::ghost);
        CHECK(!refused.failed());
        CHECK(refused.historyFailed());
        CHECK_EQ(refused.historyError(), "database is locked");
        CHECK_EQ(refused.error(), "pedal is ghost" + kCoreTail); // the core's sentence untouched

        // The ending stays what it was: the work succeeded, the job did not
        // fail — only nothing is ok any more.
        JobOutcome done = JobOutcome::success();
        done.historyThrew("database is locked");
        CHECK(!done.ok());
        CHECK(!done.failed());
        CHECK(!done.didNotRun());
        CHECK(done.historyFailed());
        CHECK(done.error().empty());

        JobOutcome failed = JobOutcome::failure("disk full");
        failed.historyThrew("database is locked");
        CHECK(failed.failed());
        CHECK_EQ(failed.error(), "disk full");
    }

    // --- a failure's detail is the log's, never the banner's: the type of
    // a throw that had no words ---

    {
        const JobOutcome plain = JobOutcome::failure("disk full");
        CHECK(plain.detail().empty());
        const JobOutcome wordless =
            JobOutcome::failure("the job stopped without saying why", "a throw without a message: St13runtime_error");
        CHECK(wordless.failed());
        CHECK_EQ(wordless.error(), "the job stopped without saying why");
        CHECK_EQ(wordless.detail(), "a throw without a message: St13runtime_error");
        CHECK_EQ(jobwords::banner("Trim slot 5", wordless), "Trim slot 5: the job stopped without saying why");
        CHECK(jobwords::banner("Trim slot 5", wordless).find("runtime_error") == std::string::npos);
        CHECK_EQ(jobwords::failureLog("Trim slot 5", wordless),
                 "Trim slot 5: the job stopped without saying why (a throw without a message: St13runtime_error)");
        // Nothing for the log that the banner did not say, and nothing for
        // an ending that is not a failure.
        CHECK_THROWS(jobwords::failureLog("Trim slot 5", plain), "detail the banner leaves out");
        CHECK_THROWS(jobwords::failureLog("Trim slot 5", JobOutcome::refused(State::ghost)), "only a job that failed");
        CHECK_THROWS(jobwords::failureLog("Trim slot 5", JobOutcome::success()), "only a job that failed");
        CHECK_THROWS(jobwords::failureLog("", wordless), "needs the job's description");
        CHECK(wordless != plain);
    }

    // --- who is told: the player's own job every time, a quiet job only when
    // its work failed — a refusal ran nothing — or when the history could
    // not record the ending, whatever it was ---

    {
        CHECK(JobOutcome::success().told(false));
        CHECK(JobOutcome::failure("x").told(false));
        CHECK(JobOutcome::refused(State::ejecting).told(false));
        CHECK(!JobOutcome::success().told(true));
        CHECK(JobOutcome::failure("x").told(true));
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected })
            CHECK(!JobOutcome::refused(state).told(true));
        CHECK(!JobOutcome::unmounted().told(true));
        JobOutcome refused = JobOutcome::refused(State::ejecting);
        refused.historyThrew("database is locked");
        CHECK(refused.told(true));
        JobOutcome done = JobOutcome::success();
        done.historyThrew("database is locked");
        CHECK(done.told(true));
    }

    // --- the banner: a refusal for every reason reads as "did not run",
    // about the pedal, never the core's sentence ---

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
        CHECK_EQ(jobwords::banner(push, JobOutcome::unmounted()),
                 "Push loop.wav to slot 3 did not run: no pedal volume is mounted");
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const JobOutcome refused = JobOutcome::refused(state);
            const std::string line = jobwords::banner(push, refused);
            CHECK(line.find("refusing to touch") == std::string::npos);
            CHECK(line.find(refused.error()) == std::string::npos); // the core's sentence stays in the log
        }
        CHECK(jobwords::banner(push, JobOutcome::unmounted()).find(JobOutcome::unmounted().error())
              == std::string::npos);
    }

    // --- a failure of the job's own work keeps today's shape ---

    {
        CHECK_EQ(jobwords::banner("Trim slot 5", JobOutcome::failure("file vanished")),
                 "Trim slot 5: file vanished");
        CHECK_EQ(jobwords::banner(kFirstSnapshot,
                                  JobOutcome::failure("the mounted volume changed before its first snapshot finished")),
                 "Record the card's first snapshot: the mounted volume changed before its first snapshot finished");
    }

    // --- the history's failure is said after the ending, and the ending
    // keeps its own words: a refused job still "did not run" ---

    {
        JobOutcome refused = JobOutcome::refused(State::ejecting);
        refused.historyThrew("database is locked");
        CHECK_EQ(jobwords::banner("Undo", refused),
                 "Undo did not run: the pedal was being disconnected; " + kHistory + "database is locked");
        JobOutcome failed = JobOutcome::failure("file vanished");
        failed.historyThrew("database is locked");
        CHECK_EQ(jobwords::banner("Trim slot 5", failed),
                 "Trim slot 5: file vanished; " + kHistory + "database is locked");
        JobOutcome done = JobOutcome::success();
        done.historyThrew("database is locked");
        CHECK_EQ(jobwords::banner("Trim slot 5", done), "Trim slot 5: " + kHistory + "database is locked");
    }

    // --- no line for a success, none without a description: a caller bug,
    // not an empty banner ---

    {
        CHECK_THROWS(jobwords::banner("Trim slot 5", JobOutcome::success()), "did not succeed");
        CHECK_THROWS(jobwords::banner("", JobOutcome::failure("x")), "needs the job's description");
        CHECK_THROWS(jobwords::banner("", JobOutcome::refused(State::ghost)), "needs the job's description");
    }

    // --- the log keeps the core's sentence for a refusal, and only for one;
    // the history's failure is the banner's, not the log line's ---

    {
        CHECK_EQ(jobwords::refusalLog("Trim slot 5", JobOutcome::refused(State::ejecting)),
                 "Trim slot 5 did not run: pedal is ejecting" + kCoreTail);
        CHECK_EQ(jobwords::refusalLog("Trim slot 5", JobOutcome::unmounted()),
                 "Trim slot 5 did not run: no pedal volume mounted");
        JobOutcome refused = JobOutcome::refused(State::ghost);
        refused.historyThrew("database is locked");
        CHECK_EQ(jobwords::refusalLog("Trim slot 5", refused), "Trim slot 5 did not run: pedal is ghost" + kCoreTail);
        CHECK_THROWS(jobwords::refusalLog("Trim slot 5", JobOutcome::failure("x")), "only a job that did not run");
        CHECK_THROWS(jobwords::refusalLog("Trim slot 5", JobOutcome::success()), "only a job that did not run");
        CHECK_THROWS(jobwords::refusalLog("", JobOutcome::refused(State::ghost)), "needs the job's description");
    }

    // --- the first snapshot: a step that did not run is an interruption,
    // said once, quietly, with what happens next; the same sentence whatever
    // took the pedal away, and it does not say again that the pedal left — it
    // follows the departure's own sentence ---

    {
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const std::string words = jobwords::firstSnapshotInterrupted(JobOutcome::refused(state));
            CHECK_EQ(words, kStopped);
            CHECK(words.find("refusing") == std::string::npos);
            CHECK(words.find("error") == std::string::npos);
            CHECK(words.find("disconnected") == std::string::npos);
        }
        CHECK_EQ(jobwords::firstSnapshotInterrupted(JobOutcome::unmounted()), kStopped);
        // A step that failed on its own is a failure and has no such sentence.
        CHECK_THROWS(jobwords::firstSnapshotInterrupted(JobOutcome::failure("card changed")), "failed step");
        CHECK_THROWS(jobwords::firstSnapshotInterrupted(JobOutcome::success()), "failed step");
    }

    // --- its History row reads a reason, never the core's sentence ---

    {
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected }) {
            const std::string reason = jobwords::firstSnapshotInterruptedReason(JobOutcome::refused(state));
            CHECK_EQ(reason, "stopped when the pedal was disconnected");
            CHECK(reason.find("refusing") == std::string::npos);
        }
        CHECK_EQ(jobwords::firstSnapshotInterruptedReason(JobOutcome::unmounted()),
                 "stopped when the pedal was disconnected");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedReason(JobOutcome::failure("card changed")),
                     "only a step that did not run");
    }

    // --- its log line: the slot the run stopped at and the core's sentence ---

    {
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(14, JobOutcome::refused(State::ejecting)),
                 "first snapshot interrupted at slot 14: pedal is ejecting" + kCoreTail);
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(1, JobOutcome::refused(State::ghost)),
                 "first snapshot interrupted at slot 1: pedal is ghost" + kCoreTail);
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(99, JobOutcome::refused(State::disconnected)),
                 "first snapshot interrupted at slot 99: pedal is disconnected" + kCoreTail);
        CHECK_EQ(jobwords::firstSnapshotInterruptedLog(7, JobOutcome::unmounted()),
                 "first snapshot interrupted at slot 7: no pedal volume mounted");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(0, JobOutcome::refused(State::ejecting)), "no slot 0");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(100, JobOutcome::refused(State::ejecting)), "no slot 100");
        CHECK_THROWS(jobwords::firstSnapshotInterruptedLog(14, JobOutcome::failure("card changed")),
                     "only a step that did not run");
    }

    // --- the notice, way in #1 — the refused step's own completion: it
    // logs the interruption and the departure's toast says both sentences;
    // a disconnect without an interruption says only its own ---

    {
        FirstSnapshotNotice notice;
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
        CHECK(!notice.departure("").has_value()); // nothing happened, nothing to say

        const auto logged = notice.interrupted(JobOutcome::refused(State::ejecting), 14);
        CHECK(logged == std::optional<std::string>("first snapshot interrupted at slot 14: pedal is ejecting" + kCoreTail));
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

    // --- way in #2 — the scan that sees the pedal gone while the snapshot
    // is in flight: the owner cancels the run and the step's completion is
    // dropped, so this is where the interruption is decided. The line names
    // the slot the refused step was for: the one after the last reached ---

    {
        FirstSnapshotNotice notice;
        const auto logged = notice.departed(State::ejecting, true, 13);
        CHECK(logged == std::optional<std::string>("first snapshot interrupted at slot 14: pedal is ejecting" + kCoreTail));
        CHECK(notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected + ". " + kStopped));

        // Seen as a ghost (a yank the probe saw first) and as a Finder eject.
        CHECK(notice.departed(State::ghost, true, 0)
              == std::optional<std::string>("first snapshot interrupted at slot 1: pedal is ghost" + kCoreTail));
        CHECK(notice.departure("") == std::optional<std::string>(kStopped));
        CHECK(notice.departed(State::disconnected, true, 98)
              == std::optional<std::string>("first snapshot interrupted at slot 99: pedal is disconnected" + kCoreTail));
        CHECK(notice.departure("") == std::optional<std::string>(kStopped));

        // No run in flight — none started, or it had settled — is no
        // interruption, whatever the scan saw.
        for (const State state : { State::ejecting, State::disconnected, State::ghost, State::ejected })
            CHECK(!notice.departed(state, false, 40).has_value());
        CHECK(!notice.pending());
        // A scan that still sees the pedal connected is not a departure: the
        // card may be unreadable, and the status line says so.
        CHECK(!notice.departed(State::connected, true, 40).has_value());
        CHECK(!notice.pending());
    }

    // --- the two ways in cannot both say it: whichever comes first logs
    // and the other is not news, in either order ---

    {
        FirstSnapshotNotice notice;
        CHECK(notice.interrupted(JobOutcome::refused(State::ejecting), 14).has_value());
        CHECK(!notice.departed(State::ejecting, true, 13).has_value());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected + ". " + kStopped));
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));

        CHECK(notice.departed(State::ghost, true, 13).has_value());
        CHECK(!notice.interrupted(JobOutcome::refused(State::ghost), 14).has_value());
        CHECK(notice.departure("") == std::optional<std::string>(kStopped));
        CHECK(!notice.departure("").has_value());
    }

    // --- where the departure was told another way (the lifecycle line, a
    // banner), the sentence stands alone — and it is the one sentence ---

    {
        FirstSnapshotNotice notice;
        notice.interrupted(JobOutcome::refused(State::ghost), 5);
        const auto alone = notice.departure("");
        CHECK(alone.has_value());
        if (alone)
            CHECK_EQ(*alone, jobwords::firstSnapshotInterrupted(JobOutcome::refused(State::ghost)));
        CHECK(!notice.departure("").has_value());
    }

    // --- a departure that already ends its sentence — a period, an
    // ellipsis, "!" or "?" — gets no second period ---

    {
        FirstSnapshotNotice notice;
        for (const std::string told : { "Volume ejected.", "Volume ejected\xe2\x80\xa6", "Volume ejected!", "Volume ejected?" }) {
            notice.interrupted(JobOutcome::refused(State::ejected), 5);
            CHECK(notice.departure(told) == std::optional<std::string>(told + " " + kStopped));
        }
        notice.interrupted(JobOutcome::refused(State::ejected), 5);
        CHECK(notice.departure("Volume ejected") == std::optional<std::string>("Volume ejected. " + kStopped));
        // A bare period is a sentence end too; a word ending in the letter
        // before an ellipsis's last byte is not an ellipsis.
        notice.interrupted(JobOutcome::refused(State::ejected), 5);
        CHECK(notice.departure("Done, ok.") == std::optional<std::string>("Done, ok. " + kStopped));
    }

    // --- the next connect resumes the snapshot and forgets the notice: the
    // disconnect after it says only its own sentence ---

    {
        FirstSnapshotNotice notice;
        notice.interrupted(JobOutcome::refused(State::disconnected), 3);
        notice.connectionStarted();
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
        // One interruption told once, however many times it was noticed.
        notice.interrupted(JobOutcome::refused(State::ejecting), 3);
        notice.interrupted(JobOutcome::refused(State::ejected), 3);
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected + ". " + kStopped));
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
    }

    // --- only a step that did not run is an interruption: a failure or a
    // success offered as one is a caller bug and leaves nothing pending ---

    {
        FirstSnapshotNotice notice;
        CHECK_THROWS(notice.interrupted(JobOutcome::failure("card changed"), 4), "only a step that did not run");
        CHECK_THROWS(notice.interrupted(JobOutcome::success(), 4), "only a step that did not run");
        CHECK_THROWS(notice.interrupted(JobOutcome::refused(State::ghost), 0), "no slot 0");
        CHECK(!notice.pending());
        CHECK(notice.departure(kDisconnected) == std::optional<std::string>(kDisconnected));
    }

    return testkit::summary("job_words");
}
