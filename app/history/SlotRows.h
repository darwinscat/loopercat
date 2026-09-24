// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"
#include "SlotStory.h"

#include <cstdint>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::rows — what the History tab may offer for each row of a
// slot's timeline (#50). Between the store, which knows what was recorded,
// and the pane, which knows how to draw: this is the layer that decides
// whether a row can be listened to and whether its state can go back.
//
// It lives apart from the window so the decisions can be tested. Which take a
// row is about, and whether its bytes still exist, is the difference between
// a button that works and a button that lies.
//==============================================================================
namespace loopercat::history::rows
{

struct Row {
    std::int64_t op = 0;
    std::int64_t at = 0;
    story::Line line;
    bool playable = false;   // its take's bytes are in the store
    bool restorable = false; // its state can be put back onto the card
    std::string takeHash;    // the bytes Play would sound, empty when there are none
};

// The rows of one slot, in the order the store gave them (oldest first).
//
// The newest row is the state the slot is in now, so its take is the one on
// the card: the player already has it on the Audio tab, and the store holds
// no copy — it keeps a take when something replaces it, not before.
//
// A row can be restored when it recorded a state (a body), that state's take
// can be produced (none to produce, or bytes in the store), and it is not
// the newest row — the state the slot is already in is not somewhere to go
// back to. A legacy row has no body at all — the folders it was read from
// never recorded one — so it offers its take to listen to and nothing to
// return to.
inline std::vector<Row> forSlot(const std::vector<HistoryStore::TimelineEntry>& entries)
{
    std::vector<Row> out;
    out.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const HistoryStore::TimelineEntry& entry = entries[i];
        const bool newest = i + 1 == entries.size();
        const bool hasTake = entry.takeHash.has_value() || !entry.takeName.empty();

        story::Take take = story::Take::none;
        if (hasTake)
            take = newest          ? story::Take::onCard
                : entry.takeKept   ? story::Take::kept
                                   : story::Take::lost;

        Row row;
        row.op = entry.op;
        row.at = entry.at;
        row.line = story::tell({ .kind = entry.kind,
                                 .beforeBody = entry.beforeBody,
                                 .afterBody = entry.afterBody,
                                 .swappedWith = entry.swappedWith,
                                 .takeName = entry.takeName,
                                 .take = take,
                                 .note = entry.note });
        row.playable = entry.takeKept;
        row.restorable = !newest && entry.afterBody.has_value() && (!hasTake || entry.takeKept);
        if (entry.takeKept && entry.takeHash)
            row.takeHash = *entry.takeHash;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace loopercat::history::rows
