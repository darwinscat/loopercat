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
// One row from what one operation recorded about one slot. `newest` is
// whether that operation is the last one on the slot — the rule above — and
// it is the caller's to say, because a slot's own timeline and the whole
// card's read it differently.
inline Row one(const HistoryStore::TimelineEntry& entry, bool newest)
{
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
    return row;
}

inline std::vector<Row> forSlot(const std::vector<HistoryStore::TimelineEntry>& entries)
{
    std::vector<Row> out;
    out.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
        out.push_back(one(entries[i], i + 1 == entries.size()));
    return out;
}

// --- the whole card (the History window, #73) ---

// One row per operation, over the same facts and the same words as the
// slot's rows: one model, two views. A swap is one row with two slots.
//
// What the row offers: Play when any take it holds is kept (the first kept
// one is what plays; Export takes the same bytes); Restore when it recorded
// at least one state and every state it recorded can go back, by the slot's
// own rule. `state` says what the operation's own status or origin adds —
// failed, interrupted, recorded on the pedal — and is empty for a plain
// finished operation of the app's.
struct CardRow {
    struct Take {
        int slot = 0;
        std::string audio;      // "take kept", "in the slot now", ...
        bool playable = false;
        bool restorable = false;
        std::string takeHash;
    };

    std::int64_t op = 0;
    std::int64_t at = 0;
    std::string kind;
    std::string actor;
    std::string status;
    bool pinned = false;
    std::string action;
    std::string detail;
    std::string state;
    std::vector<Take> takes; // one per touched slot, ascending

    std::vector<int> slots() const
    {
        std::vector<int> out;
        for (const Take& take : takes)
            out.push_back(take.slot);
        return out;
    }
    bool playable() const
    {
        for (const Take& take : takes)
            if (take.playable)
                return true;
        return false;
    }
    bool restorable() const
    {
        if (takes.empty())
            return false;
        for (const Take& take : takes)
            if (!take.restorable)
                return false;
        return true;
    }
    std::string takeHash() const
    {
        for (const Take& take : takes)
            if (take.playable)
                return take.takeHash;
        return {};
    }
};

inline std::vector<CardRow> forCard(const std::vector<HistoryStore::CardEntry>& entries)
{
    std::vector<CardRow> out;
    out.reserve(entries.size());
    for (const HistoryStore::CardEntry& entry : entries) {
        CardRow row;
        row.op = entry.op;
        row.at = entry.at;
        row.kind = entry.kind;
        row.actor = entry.actor;
        row.status = entry.status;
        row.pinned = entry.pinned;
        if (entry.status == "failed")
            row.state = "failed";
        else if (entry.status == "interrupted")
            row.state = "interrupted";
        else if (entry.status == "pending")
            row.state = "still running";
        else if (entry.actor == "pedal")
            row.state = "recorded on the pedal";

        for (const HistoryStore::CardEntry::Slot& touched : entry.slots) {
            const Row slotRow = one(touched.facts, touched.newest);
            if (row.action.empty()) {
                row.action = slotRow.line.action;
                row.detail = slotRow.line.detail;
            }
            row.takes.push_back({ touched.slot, slotRow.line.audio, slotRow.playable,
                                  slotRow.restorable, slotRow.takeHash });
        }
        if (entry.kind == "swap" && entry.slots.size() == 2) {
            row.action = "Swapped slots " + std::to_string(entry.slots[0].slot) + " and "
                + std::to_string(entry.slots[1].slot);
            row.detail.clear(); // one slot's numbers would speak for both
        }
        if (row.action.empty()) {
            // Nothing recorded about any slot: the operation's own name and
            // its line are all there is, as story::tell says for a kind it
            // has no words for.
            row.action = entry.kind;
            row.detail = entry.note;
        }
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace loopercat::history::rows
