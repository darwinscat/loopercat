// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "HistoryRecorder.h"
#include "../PedalWorker.h"
#include <atomic>

namespace loopercat::history {

struct FirstSeenRun {
    HistoryRecorder::Snapshot snapshot;
    std::atomic<bool> cancelled { false };
    explicit FirstSeenRun(HistoryRecorder::Snapshot value) : snapshot(std::move(value)) {}
};

// One slot per job: foreground work can run between any two slots. Completion
// runs on the worker even when the lifecycle gate refuses the volume.
inline PedalWorker::Job firstSeenJob(const std::shared_ptr<HistoryRecorder>& recorder,
                                     const std::shared_ptr<FirstSeenRun>& run, int slot,
                                     std::function<void(int, const std::string&)> completed)
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
    job.after = [recorder, run, count, onComplete = std::move(completed)](const std::string& error) {
        if (run->cancelled) return;
        if (!error.empty())
            recorder->interruptSnapshot(run->snapshot, error);
        onComplete(*count, error);
    };
    return job;
}

} // namespace loopercat::history
