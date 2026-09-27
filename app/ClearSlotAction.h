// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalWorker.h"
#include "history/HistoryRecorder.h"

namespace loopercat::clearSlotAction {

// Own the confirmation and recorded job together. The caller only releases
// playback and submits the resulting job to its worker after confirmation.
void request(int slot, std::shared_ptr<history::HistoryRecorder> recorder,
             std::function<void(PedalWorker::Job)> enqueue);

} // namespace loopercat::clearSlotAction
