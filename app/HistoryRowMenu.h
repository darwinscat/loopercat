// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

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
    exportTake,  // "Export take...": the row's take, written to a file on this computer
    separator,   // what follows is about the slot, not the row
    clearHistory // "Clear history of this slot...": the whole slot, as before
};

struct Item {
    Entry entry;
    bool enabled;
};

// What the rows layer decided about one row.
struct RowOffers {
    bool restorable = false; // its state can go back onto the card
    bool takeKept = false;   // its take's bytes are in the history
};

// The same four entries for every row, so the menu never changes shape under
// the hand; what a row cannot do is there and grey. While the pedal is busy
// nothing can be chosen: a restore would queue behind the running job, and an
// export and a clear go through the same worker.
inline std::vector<Item> forRow(RowOffers row, bool busy)
{
    return { { Entry::restore, !busy && row.restorable },
             { Entry::exportTake, !busy && row.takeKept },
             { Entry::separator, false },
             { Entry::clearHistory, !busy } };
}

} // namespace loopercat::historymenu
