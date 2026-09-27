// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalWorker.h"
#include "history/HistoryRecorder.h"

namespace juce { class PopupMenu; }

namespace loopercat::clearSlotAction {

inline constexpr int menuItemId = 5;

// An empty slot has no take to clear.
void addToMenu(juce::PopupMenu& menu, bool hasTake);

// Completes asynchronously with 1 for Clear, or any other result to cancel.
// Tests can supply the decision without constructing a native window.
using AskConfirmation = std::function<void(int, std::function<void(int)>)>;

// Own the confirmation and recorded job together. The caller only releases
// playback and submits the resulting job to its worker after confirmation.
// With no ask function supplied, the app shows its real modal dialog.
void request(int slot, bool hasTake, std::shared_ptr<history::HistoryRecorder> recorder,
             std::function<void(PedalWorker::Job)> enqueue, AskConfirmation ask = {});

} // namespace loopercat::clearSlotAction
