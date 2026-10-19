// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The menu a row of the History tab opens. The theory: a line that can be
// chosen is a promise. Restore promises a state to put back; Export take
// promises bytes to write; Clear history is about the slot, so every row
// offers it. Nothing is promised while the pedal is busy.
//
// The rows are the shapes a real store produces (the same ones the rows
// layer is tested on), each read into a menu twice: with the pedal free and
// with it busy.

#include "support.hpp"

#include "../app/HistoryRowMenu.h"
#include "../app/history/SlotRows.h"

#include <string>
#include <vector>

using namespace loopercat;
namespace rows = loopercat::history::rows;
using Entry = loopercat::history::HistoryStore::TimelineEntry;
using historymenu::Item;

namespace {

std::string bodyWith(long long wavLen, const std::string& name)
{
    std::string body = rc0::setSectionField(rc0::factorySlotBody(42), rc0::kSectionTrack1,
                                            "WavLen", wavLen);
    return rc0::setName(body, name);
}

Entry op(std::int64_t seq, const std::string& kind)
{
    Entry entry;
    entry.op = seq;
    entry.at = 1000 * seq;
    entry.kind = kind;
    entry.actor = "app";
    entry.status = "done";
    return entry;
}

struct Expect {
    const char* what;
    bool restore;
    bool exportTake;
};

// The menu of the row at `index` in the slot's timeline, against what the
// player must be offered — free, then busy.
void checkMenu(const std::vector<Entry>& timeline, std::size_t index, Expect expect)
{
    const auto made = rows::forSlot(timeline);
    CHECK(index < made.size());
    if (index >= made.size())
        return;
    const rows::Row& row = made[index];
    for (const bool busy : { false, true }) {
        const std::vector<Item> menu = historymenu::forRow({ row.restorable, row.playable }, busy);
        CHECK_EQ(menu.size(), std::size_t { 4 });
        if (menu.size() != 4)
            continue;
        CHECK(menu[0].entry == historymenu::Entry::restore);
        CHECK(menu[1].entry == historymenu::Entry::exportTake);
        CHECK(menu[2].entry == historymenu::Entry::separator);
        CHECK(menu[3].entry == historymenu::Entry::clearHistory);
        CHECK(!menu[2].enabled);
        if (busy) {
            for (const Item& item : menu)
                CHECK(!item.enabled);
            continue;
        }
        if (menu[0].enabled != expect.restore || menu[1].enabled != expect.exportTake)
            testkit::fail(std::string(expect.what) + ": restore " + (menu[0].enabled ? "on" : "off")
                              + ", export " + (menu[1].enabled ? "on" : "off"),
                          __FILE__, __LINE__);
        CHECK(menu[3].enabled); // the slot's history can always be cleared
    }
}

} // namespace

int main()
{
    const std::string empty = bodyWith(0, "Memory42");
    const std::string loaded = bodyWith(1719900, "TEST_42_HIST");
    const std::string hash(32, '\x11');

    // The newest push: its take is the one on the card, not one the history
    // keeps, and the slot is already in its state.
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = hash;
        pushed.takeIsAfter = true;
        checkMenu({ pushed }, 0, { "the newest push", false, false });
    }

    // An older push whose take the history kept: both.
    // The same push once its bytes were released: neither — the state needs
    // its take, and there is nothing to write.
    for (const bool kept : { true, false }) {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = hash;
        pushed.takeKept = kept;
        pushed.takeIsAfter = true;
        Entry renamed = op(2, "rename");
        renamed.beforeBody = loaded;
        renamed.afterBody = rc0::setName(loaded, "RENAMED");
        checkMenu({ pushed, renamed }, 0,
                  { kept ? "an older push, take kept" : "an older push, take released", kept, kept });
        // A rename records a body and no take: a state to go back to, nothing to export.
        Entry pushedAgain = op(3, "push");
        pushedAgain.beforeBody = renamed.afterBody;
        pushedAgain.afterBody = loaded;
        pushedAgain.takeName = "other.wav";
        pushedAgain.takeHash = std::string(32, '\x22');
        pushedAgain.takeIsAfter = true;
        checkMenu({ pushed, renamed, pushedAgain }, 1, { "an older rename", true, false });
    }

    // A clear: the newest row, so nothing to restore — its take left the
    // slot and the history kept it, so it can be exported.
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        pushed.takeName = "take.wav";
        pushed.takeHash = hash;
        pushed.takeKept = true;
        pushed.takeIsAfter = true;
        Entry cleared = op(2, "clear");
        cleared.beforeBody = loaded;
        cleared.afterBody = empty;
        cleared.takeName = "take.wav";
        cleared.takeHash = hash;
        cleared.takeKept = true;
        cleared.takeIsAfter = false; // the take it archived, not one it left
        checkMenu({ pushed, cleared }, 1, { "the newest clear", false, true });
    }

    // A failed push recorded no state: nothing to put back, nothing to write.
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        Entry failed = op(2, "push");
        failed.status = "failed";
        failed.note = "cannot read song.wav";
        Entry later = op(3, "rename");
        later.beforeBody = loaded;
        later.afterBody = rc0::setName(loaded, "LATER");
        checkMenu({ pushed, failed, later }, 1, { "a failed push", false, false });
    }

    // A normalize that found the slot at target was about the slot and
    // recorded nothing in it (#144): no state, no take.
    {
        Entry pushed = op(1, "push");
        pushed.beforeBody = empty;
        pushed.afterBody = loaded;
        Entry about = op(2, "normalize");
        about.subjectOnly = true;
        about.note = "already at -18.0 LUFS";
        checkMenu({ pushed, about }, 1, { "a normalize that changed nothing", false, false });
    }

    return testkit::summary("history_row_menu_tests");
}
