// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "HistoryRecorder.h"
#include "../JobWords.h"
#include "../PedalWorker.h"
#include <atomic>

namespace loopercat::history {

struct FirstSeenRun {
    HistoryRecorder::Snapshot snapshot;
    std::atomic<bool> cancelled { false };
    explicit FirstSeenRun(HistoryRecorder::Snapshot value) : snapshot(std::move(value)) {}
};

// One slot per job: foreground work can run between any two slots. Completion
// runs on the worker even when the lifecycle gate refuses the volume, and is
// handed the step's outcome whole: a refusal is the interruption the run
// resumes from on the next connect (issue #146), a failure of the step's own
// work is a failure — the words for each are JobWords.h's.
inline PedalWorker::Job firstSeenJob(const std::shared_ptr<HistoryRecorder>& recorder,
                                     const std::shared_ptr<FirstSeenRun>& run, int slot,
                                     std::function<void(int, const JobOutcome&)> completed)
{
    auto count = std::make_shared<int>(0);
    PedalWorker::Job job {
        "Record the card's first snapshot", 0,
        [recorder, run, slot, count](const volume::fs::path& volume) {
            if (run->cancelled) return;
            if (volume != run->snapshot.volume)
                throw Error("the mounted volume changed before its first snapshot finished");
            *count = recorder->snapshotStep(run->snapshot, slot);
        }, nullptr, 0, true, true, true
    };
    job.after = [recorder, run, count, onComplete = std::move(completed)](const JobOutcome& outcome) {
        if (run->cancelled) return;
        // The reason goes into the History row after the take's name: a
        // step that did not run says so in the player's words, a failed
        // step says what failed. The core's sentence is the log's.
        if (!outcome.ok())
            recorder->interruptSnapshot(run->snapshot,
                                        outcome.didNotRun()
                                            ? jobwords::firstSnapshotInterruptedReason(outcome)
                                            : outcome.error());
        onComplete(*count, outcome);
    };
    return job;
}

} // namespace loopercat::history
