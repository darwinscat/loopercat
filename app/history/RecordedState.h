// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <loopercat/Commands.hpp>

#include <algorithm>
#include <string>
#include <vector>

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

// The tracks a memory body describes: TRACK1, TRACK2, ... as far as they go.
inline int trackSectionsIn(const std::string& body)
{
    int tracks = 0;
    while (body.find(rc0::trackSectionOpen(tracks + 1)) != std::string::npos)
        ++tracks;
    return tracks;
}

// Whether a body says a track holds a loop: the pedal's own flag, WavStat,
// which is what the Audio tab reads and what the core's restore checks.
inline bool trackHoldsLoop(const std::string& body, int track)
{
    return rc0::sectionField(body, rc0::trackSectionName(track), "WavStat") == rc0::kWavStatIndexed;
}

// The takes of the state a row recorded: the ones the operation LEFT in the
// slot, never the ones it archived on the way out.
inline std::vector<HistoryStore::SlotTake> stateTakes(const HistoryStore::TimelineEntry& facts)
{
    return facts.takeIsAfter ? facts.takes : std::vector<HistoryStore::SlotTake> {};
}

// Whether the state a row recorded is one the core's restore puts back: a
// body, and beside it, track by track, exactly the takes the body says the
// tracks hold — each kept in the store. A track whose WavStat says it holds a
// loop needs its take; one that says it holds none needs none, and a stray
// file the row saw beside it is no part of the state. A take on a track the
// body does not describe makes no state at all.
inline bool restorableState(const HistoryStore::TimelineEntry& facts)
{
    if (!facts.afterBody)
        return false;
    const std::vector<HistoryStore::SlotTake> takes = stateTakes(facts);
    const int tracks = trackSectionsIn(*facts.afterBody);
    for (const HistoryStore::SlotTake& take : takes)
        if (take.track < 1 || take.track > tracks)
            return false;
    for (int track = 1; track <= tracks; ++track) {
        const auto take = std::find_if(takes.begin(), takes.end(),
                                       [track](const HistoryStore::SlotTake& t) { return t.track == track; });
        const bool hasTake = take != takes.end();
        if (trackHoldsLoop(*facts.afterBody, track) != hasTake)
            return false;
        if (hasTake && !take->kept)
            return false;
    }
    return true;
}

inline commands::SlotState recordedState(HistoryStore& store,
                                         const HistoryStore::TimelineEntry& facts, int slot)
{
    if (!facts.afterBody)
        throw Error("that row recorded no state for slot " + std::to_string(slot));
    commands::SlotState state;
    state.body = *facts.afterBody;
    for (const HistoryStore::SlotTake& take : stateTakes(facts)) {
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
