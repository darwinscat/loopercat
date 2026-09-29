// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "history/HistoryStore.h"
#include "history/Retention.h"
#include <functional>
#include <string>

namespace loopercat::clearhistory {
using Plan = history::HistoryStore::ForgetPlan;
struct Question {
    std::string title;
    std::string message;
    bool holds = false;
};
using Answer = std::function<void(bool)>;
using Confirm = std::function<void(Question, Answer)>;

inline Question question(int slot, const Plan& plan, bool holds = false)
{
    using history::retention::bytesText;
    using history::retention::countText;
    Question q { "Clear the history of slot " + std::to_string(slot) + "?",
        "This cannot be undone. " + countText(plan.rowsRemoved, "entry", "entries") + " and "
        + countText(plan.takesFreed, "take", "takes")
        + " (" + bytesText(plan.bytesFreed) + ") will be deleted.", holds };
    if (!plan.pinned.empty())
        q.message += "\n\nIt includes "
            + countText(static_cast<std::int64_t>(plan.pinned.size()), "pinned entry", "pinned entries");
    if (plan.cutsUndo || !plan.undoTargets.empty())
        q.message += "\n\nUndo will no longer be able to go back past this point.";
    if (holds) {
        q.title = "Clear protected history of slot " + std::to_string(slot) + "?";
        q.message += "\n\nConfirm separately that these protected entries may be deleted.";
    }
    return q;
}

// The UI supplies asynchronous dialogs; tests supply answers without JUCE or
// windows. Cancellation never reaches the worker's write path.
inline void ask(int slot, Plan plan, Confirm confirm, Answer completed)
{
    if (plan.rowsRemoved == 0) { completed(false); return; }
    auto first = question(slot, plan);
    confirm(std::move(first), [slot, offered = std::move(plan), confirm, completed](bool yes) {
        if (!yes) { completed(false); return; }
        if (offered.hasHolds()) confirm(question(slot, offered, true), completed);
        else completed(true);
    });
}
} // namespace loopercat::clearhistory
