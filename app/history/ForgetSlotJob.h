// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "HistoryRecorder.h"
#include "../OperationId.h"
#include "../PedalWorker.h"

namespace loopercat::history {
// The journal row has no slot rows of its own: clearing never repopulates
// the history just forgotten, and maintenance never becomes an Undo target.
inline PedalWorker::Job forgetSlotJob(std::shared_ptr<HistoryRecorder> recorder,
    std::int64_t card, int slot, HistoryStore::ForgetPlan plan, bool confirmedHolds,
    std::function<void()> refreshed)
{
    auto note = std::make_shared<juce::String>();
    auto id = opid::make("forget-history");
    PedalWorker::Job job {
        "Clear the history of slot " + juce::String(slot), slot,
        [recorder, card, slot, plan, confirmedHolds, note](const volume::fs::path&) {
            auto& store = recorder->store();
            const auto result = store.forgetSlot(card, slot,
                static_cast<std::int64_t>(juce::Time::currentTimeMillis()), confirmedHolds, &plan);
            *note = "Slot " + juce::String(slot) + ": " + juce::String(result.rowsRemoved) + " entries, "
                + juce::String(result.takesFreed) + " takes, "
                + juce::String(result.bytesFreed) + " bytes freed";
            while (store.vacuum(64) > 0) {}
        }, note, 0, false, false, false
    };
    job.before = [recorder, card, id](const volume::fs::path&) {
        recorder->beginMaintenance(id, card);
    };
    job.after = [recorder, id, note, refreshed](const std::string& error) {
        try { recorder->finish(id, error, note->toStdString()); }
        catch (...) { refreshed(); throw; }
        refreshed();
    };
    return job;
}
} // namespace loopercat::history
