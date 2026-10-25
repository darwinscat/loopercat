// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <cstddef>
#include <vector>

//==============================================================================
// loopercat::historymenu — what a row of the History tab offers when it is
// clicked: the menu's items, in order, and whether each one can be chosen.
//
// The tab has no buttons. A row is a sentence, and a click on it — either
// button — asks what can be done with it; the answer is this menu. What a row
// can do was decided by the rows layer (app/history/SlotRows.h); this only
// turns those facts into a menu, so the rule a player meets is a function
// that can be tested without a window. The pane draws what it returns and
// nothing else.
//==============================================================================
namespace loopercat::historymenu
{

enum class Entry {
    restore,     // "Restore this state": the row's state, back onto the card
    exportTake,  // "Export take...": a take of the row, written to a file on this computer
    separator,   // what follows is about the slot, not the row
    clearHistory // "Clear history of this slot...": the whole slot, as before
};

struct Item {
    Entry entry;
    bool enabled;
    // The track an export line writes the take of, 1 up; 0 on a line about
    // no take.
    int track = 0;
};

// What the rows layer decided about one row.
struct RowOffers {
    bool restorable = false; // its state can go back onto the card
    // One per track of the memory, track 1 first: whether that track's take
    // is in the history. A memory of one track has one.
    std::vector<bool> takeKept;
};

// The same entries for every row of a memory, so the menu never changes
// shape under the hand; what a row cannot do is there and grey. A take is
// exported per track: a memory of two tracks has two export lines, one per
// track, and a row with nothing kept on a track greys that track's line.
// While the pedal is busy nothing can be chosen: a restore would queue behind
// the running job, and an export and a clear go through the same worker.
inline std::vector<Item> forRow(const RowOffers& row, bool busy)
{
    if (row.takeKept.empty())
        throw Error("a History row offers the takes of at least one track");
    std::vector<Item> items { { Entry::restore, !busy && row.restorable, 0 } };
    for (std::size_t i = 0; i < row.takeKept.size(); ++i)
        items.push_back({ Entry::exportTake, !busy && row.takeKept[i], static_cast<int>(i) + 1 });
    items.push_back({ Entry::separator, false, 0 });
    items.push_back({ Entry::clearHistory, !busy, 0 });
    return items;
}

} // namespace loopercat::historymenu
