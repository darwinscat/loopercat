// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"
#include "SlotStory.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
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
    // What the operation's own outcome or origin adds — "failed",
    // "interrupted", "still running", "recorded on the pedal" — and empty
    // for a plain finished operation of the app's. The same words the
    // History window wears: a row that did not finish must not read as
    // something that happened.
    std::string state;
    bool playable = false;   // its take's bytes are in the store
    bool restorable = false; // its state can be put back onto the card
    std::string takeHash;    // the bytes Play would sound, empty when there are none
};

// The words for an operation's outcome or origin, shared by the slot's rows
// and the card's so the two views never disagree about what did not happen.
inline std::string stateWords(const std::string& status, const std::string& actor)
{
    if (status == "failed")
        return "failed";
    if (status == "interrupted")
        return "interrupted";
    if (status == "pending")
        return "still running";
    if (actor == "pedal")
        return "recorded on the pedal";
    return {};
}

// What a row says when the operation was about the slot and finished with
// no word for what it did there: nothing, by its own account.
inline constexpr std::string_view kNothingChanged = "nothing changed";

// The rows of one slot, in the order the store gave them (oldest first).
//
// The newest row is the state the slot is in now, so its take is the one on
// the card: the player already has it on the Audio tab, and the store holds
// no copy — it keeps a take when something replaces it, not before.
//
// A row can be restored when it recorded a state (a body), that state's take
// can be produced (none to produce, or bytes in the store), and it is not
// the newest row — the state the slot is already in is not somewhere to go
// back to.
// One row from what one operation recorded about one slot. `newest` is
// whether that operation is the last one on the slot — the rule above — and
// it is the caller's to say, because a slot's own timeline and the whole
// card's read it differently.
inline Row one(const HistoryStore::TimelineEntry& entry, bool newest)
{
    const bool hasTake = entry.takeHash.has_value() || !entry.takeName.empty();
    // A row that only names the slot recorded nothing here; one that says so
    // and carries a state or a take is not the store's — refused, not read.
    if (entry.subjectOnly && (entry.beforeBody || entry.afterBody || hasTake))
        throw Error("operation " + std::to_string(entry.op)
                    + " names the slot as its subject and still recorded a state in it");

    // Only a take the operation LEFT in the slot can be the one on the card.
    // A row whose take is the one it archived — a clear, an undo that emptied
    // the slot — does not say what the slot holds now, even as the newest
    // row: its take is kept, or lost.
    const bool onCard = newest && entry.takeIsAfter && entry.kind != "snapshot";
    story::Take take = story::Take::none;
    if (hasTake)
        take = onCard          ? story::Take::onCard
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
                             .status = entry.status,
                             .note = entry.note });
    row.state = stateWords(entry.status, entry.actor);
    // A row that did not finish carries its reason where its numbers would
    // be: what it recorded on the way is not what the slot came to hold.
    if (entry.status != "done" && !entry.note.empty())
        row.line.detail = entry.note;
    // A finished operation that was about the slot and has no word for what
    // it did there did nothing there — said plainly, so "Renamed" with no
    // name after it does not read as a change.
    if (entry.subjectOnly && entry.status == "done" && row.line.detail.empty())
        row.line.detail = std::string(kNothingChanged);
    row.playable = entry.takeKept;
    row.restorable = !newest && entry.afterBody.has_value() && (!hasTake || entry.takeKept);
    if (entry.takeKept && entry.takeHash)
        row.takeHash = *entry.takeHash;
    return row;
}

// The state the slot is in is the last row that recorded one. A row that
// only names the slot — an operation that was about it and changed nothing
// (#144) — is not a state: it leaves the slot where the row before it put
// it, and that row keeps saying "in the slot now".
inline std::vector<Row> forSlot(const std::vector<HistoryStore::TimelineEntry>& entries)
{
    std::size_t newest = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].subjectOnly)
            newest = i;
    std::vector<Row> out;
    out.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
        out.push_back(one(entries[i], i == newest));
    return out;
}

// --- the whole card (the History window, #73) ---

// One row per operation, over the same facts and the same words as the
// slot's rows: one model, two views. A swap is one row with two slots. An
// operation that was about a slot and changed nothing (#144) is a row with
// that slot's badge, its own words, and no take.
//
// What the row offers: Play when any take it holds is kept (the first kept
// one is what plays; Export takes the same bytes); Restore when it recorded
// at least one state and every state it recorded can go back, by the slot's
// own rule. A first-sighting snapshot instead offers each eligible slot
// independently. `state` says what the operation's own status or origin adds —
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
    std::vector<Take> takes;   // one per touched slot, ascending
    std::vector<int> subjects; // the slots the operation was about (#144), ascending

    // The slots the operation recorded a state or a take in, ascending —
    // what a restore of the row acts on. A subject is not among them.
    std::vector<int> touchedSlots() const
    {
        std::vector<int> out;
        for (const Take& take : takes)
            out.push_back(take.slot);
        return out;
    }
    // Every slot the row wears as a badge: the ones the operation touched
    // and the ones it was about, ascending, each once. A subject adds a
    // badge and nothing else — no take to play, no state to put back.
    std::vector<int> slots() const
    {
        std::vector<int> out = touchedSlots();
        out.insert(out.end(), subjects.begin(), subjects.end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
    bool playable() const
    {
        for (const Take& take : takes)
            if (take.playable)
                return true;
        return false;
    }
    std::vector<int> restorableSlots() const
    {
        std::vector<int> out;
        for (const Take& take : takes)
            if (take.restorable) out.push_back(take.slot);
        return out;
    }
    bool restorable() const
    {
        if (kind == "snapshot")
            return !restorableSlots().empty();
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
        row.subjects = entry.subjects;
        row.state = stateWords(entry.status, entry.actor);

        for (const HistoryStore::CardEntry::Slot& touched : entry.slots) {
            const Row slotRow = one(touched.facts, touched.newest);
            if (row.action.empty()) {
                row.action = slotRow.line.action;
                row.detail = slotRow.line.detail;
            }
            row.takes.push_back({ touched.slot, slotRow.line.audio, slotRow.playable,
                                  slotRow.restorable, slotRow.takeHash });
        }
        if (entry.kind == "snapshot") {
            std::int64_t count = 0, bytes = 0;
            std::string failures;
            for (const auto& touched : entry.slots) {
                count += touched.facts.takeCount;
                bytes += touched.facts.takeBytes;
                if (touched.facts.status == "failed")
                    failures += (failures.empty() ? "" : "; ") + std::string("slot ")
                        + std::to_string(touched.slot) + " failed: " + touched.facts.note;
            }
            row.detail = std::to_string(count) + (count == 1 ? " take, " : " takes, ")
                + retention::bytesText(bytes);
            if (!failures.empty()) row.state = std::move(failures);
        }
        if (entry.kind == "swap" && entry.slots.size() == 2) {
            row.action = "Swapped slots " + std::to_string(entry.slots[0].slot) + " and "
                + std::to_string(entry.slots[1].slot);
            row.detail.clear(); // one slot's numbers would speak for both
        }
        if (!entry.system.empty()) {
            // The pedal's own settings: their words join the slots' — or
            // stand alone, for an operation that touched no slot.
            std::vector<story::SystemFacts> facts;
            for (const auto& change : entry.system)
                facts.push_back({ change.section, change.before, change.after });
            const story::Line settings = story::tellSystem(facts);
            if (row.action.empty()) {
                row.action = settings.action;
                row.detail = settings.detail;
            } else if (!settings.detail.empty()) {
                row.detail += (row.detail.empty() ? "" : "; ") + settings.detail;
            }
        }
        if (row.action.empty()) {
            // Nothing recorded about any slot: the words story::tell has for
            // the operation itself, including a kind this build has no words for.
            const story::Line bare = story::tell({ .kind = entry.kind, .take = story::Take::none,
                                                   .status = entry.status, .note = entry.note });
            row.action = bare.action;
            row.detail = bare.detail.empty() ? entry.note : bare.detail; // a failed op's reason
            if (row.detail.empty() && entry.status == "done" && !entry.subjects.empty())
                row.detail = std::string(kNothingChanged); // about a slot, and did nothing there
        }
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace loopercat::history::rows
