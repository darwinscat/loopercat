// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"
#include "SlotRows.h"

#include <loopercat/Commands.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

//==============================================================================
// loopercat::history::restoreOperation — "Restore this state" in the History
// window (#73): every slot a row touched goes back to what that operation
// left in it, body and take together, through the core's restore primitive.
//
// A row of the window is an operation, and an operation may have touched two
// slots (a swap), so this is the slot tab's restore once per slot, in one
// operation of its own. The row must be restorable as a whole — the window
// offers the button only then, and this refuses by the same rule, read from
// the store as it is now, before anything is written.
//==============================================================================
namespace loopercat::history
{

inline void restoreOperation(HistoryStore& store, std::int64_t op, const std::filesystem::path& volume,
                             const commands::WriteOptions& options)
{
    const std::vector<HistoryStore::CardEntry> timeline = store.cardTimeline();
    const HistoryStore::CardEntry* entry = nullptr;
    for (const auto& candidate : timeline)
        if (candidate.op == op)
            entry = &candidate;
    if (entry == nullptr)
        throw Error("that row is not in the history any more");
    const std::vector<rows::CardRow> row = rows::forCard({ *entry });
    if (row.empty() || !row.front().restorable())
        throw Error("that row recorded no state that can go back as a whole");

    // Everything read before the first write: a take no longer kept for the
    // second slot must not leave the first one restored.
    std::vector<std::pair<int, commands::SlotState>> states;
    for (const HistoryStore::CardEntry::Slot& touched : entry->slots) {
        if (!touched.facts.afterBody)
            throw Error("that row recorded no state for slot " + std::to_string(touched.slot));
        commands::SlotState state;
        state.body = *touched.facts.afterBody;
        if (touched.facts.takeHash) {
            std::optional<std::string> bytes = store.takeBytes(*touched.facts.takeHash);
            if (!bytes)
                throw Error("the take of slot " + std::to_string(touched.slot)
                            + " in that state is no longer kept");
            state.take = commands::Take { touched.facts.takeName, std::move(*bytes) };
        }
        states.emplace_back(touched.slot, std::move(state));
    }
    for (const auto& [slot, state] : states)
        commands::restore(volume, slot, state, options);
}

} // namespace loopercat::history
