// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <loopercat/Commands.hpp>

#include <string>

//==============================================================================
// loopercat::history::recordedState — the slot state one row of the timeline
// recorded, read back as the core's restore takes it: the body the operation
// left, and every take it left beside it, each on its own track, with its
// bytes out of the store.
//
// One reader for both buttons that put a state back — the slot tab's and the
// History window's — so the two cannot disagree about what a state is. The
// takes a row archived (a clear, an undo that emptied the slot) are what the
// operation took away, not part of what it left: the state is the empty slot.
// A take whose bytes are no longer kept refuses the whole state, before
// anything is written.
//==============================================================================
namespace loopercat::history
{

inline commands::SlotState recordedState(HistoryStore& store,
                                         const HistoryStore::TimelineEntry& facts, int slot)
{
    if (!facts.afterBody)
        throw Error("that row recorded no state for slot " + std::to_string(slot));
    commands::SlotState state;
    state.body = *facts.afterBody;
    if (!facts.takeIsAfter)
        return state;
    for (const HistoryStore::SlotTake& take : facts.takes) {
        const std::optional<std::string> bytes =
            take.hash ? store.takeBytes(*take.hash) : std::nullopt;
        if (!bytes)
            throw Error("the take of slot " + std::to_string(slot)
                        + (facts.takes.size() > 1 ? " on track " + std::to_string(take.track)
                                                  : std::string())
                        + " in that state is no longer kept");
        state.takes.push_back({ take.track, take.name, *bytes });
    }
    return state;
}

} // namespace loopercat::history
