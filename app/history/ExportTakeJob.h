// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "HistoryRecorder.h"
#include "TakeExport.h"
#include "../PedalWorker.h"
#include "../Strings.h"

#include <filesystem>
#include <memory>
#include <string>

namespace loopercat::history {
// "Export take...", from a row of the History tab: a take the history keeps,
// written to `file` on this computer. Nothing of it is the pedal's — no read,
// no write, no lifecycle gate, so it runs while the pedal is away — and it is
// no operation in the history either: nothing on the card changed. It goes
// through the worker like every other job, so the tab waits while it writes
// and the outcome is told in the app's usual words; a take the history no
// longer keeps is exportTake's sentence, not silence.
inline PedalWorker::Job exportTakeJob(std::shared_ptr<HistoryRecorder> recorder, std::string takeHash,
                                      std::filesystem::path destination)
{
    const juce::String name = utf8(destination.filename().u8string());
    return { "Export take to " + name, 0,
             [recorder, hash = std::move(takeHash), file = std::move(destination)](const volume::fs::path&) {
                 exportTake(recorder->store(), hash, file);
             },
             nullptr, 0,
             false,   // the player asked for it, and waits for it
             false,   // and is told how it went
             false }; // the store is on this computer: no card
}
} // namespace loopercat::history
