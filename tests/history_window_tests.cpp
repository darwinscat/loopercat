// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The History window (#73), driven without a mouse, a store or a worker,
// from what it promises:
//
//   - it shows the rows it is handed, oldest first, newest selected
//   - a slot badge filters to that slot, and "All slots" widens again
//   - a swap is one row wearing two badges, and answers to both filters
//   - a row wears at most three badges; past that a "+N" chip counts the
//     rest and filters nothing, and a snapshot of all 99 wears none: "99
//     slots" stands where they would be (#143); the counted slots still
//     answer the filter, and the filtered slot is never the hidden one
//   - a row's hint is its whole line, however narrow the row: the slots
//     the strip only counts, and the pin, included
//   - the buttons offer only what a row can do: no Play or Export for a
//     take no longer kept, no Restore for a state that cannot go back
//   - a pin toggled here reaches the owner with the operation and the new
//     state, and the row shows it at once
//   - while a job runs, nothing is offered; an empty timeline says so

#include "support.hpp"

#include "../app/HistoryWindow.h"
#include "../app/AppMenu.h"
#include "../app/history/SlotRows.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace loopercat;

namespace {

HistoryWindow::Row row(std::int64_t op, const char* action, std::vector<int> slots, bool playable,
                       bool restorable)
{
    HistoryWindow::Row r;
    r.when = "23 Sep 21:5" + juce::String(op);
    r.action = action;
    r.slots = std::move(slots);
    r.playable = playable;
    r.restorable = restorable;
    r.op = op;
    return r;
}

// The card's first sighting as the store's timeline hands it and
// rows::forCard words it: one snapshot entry touching all 99 slots, of which
// `withTakes` held a take the store kept — those a restore can put back; the
// rest were empty. Mapped to the window's row as MainComponent maps it when
// it feeds the window. Nothing here types the row's words.
HistoryWindow::Row firstSeen(const std::vector<int>& withTakes)
{
    using Card = history::HistoryStore::CardEntry;
    Card entry;
    entry.op = 7;
    entry.at = 1758657000000;
    entry.kind = "snapshot";
    entry.actor = "app";
    entry.status = "done";
    for (int slot = 1; slot <= 99; ++slot) {
        Card::Slot touched;
        touched.slot = slot;
        touched.facts.op = entry.op;
        touched.facts.at = entry.at;
        touched.facts.kind = entry.kind;
        touched.facts.actor = entry.actor;
        touched.facts.status = entry.status;
        if (std::find(withTakes.begin(), withTakes.end(), slot) != withTakes.end()) {
            touched.facts.afterBody = rc0::factorySlotBody(slot);
            touched.facts.takeName = "TEST_" + std::to_string(slot) + ".WAV";
            touched.facts.takeHash = std::string(32, static_cast<char>(slot));
            touched.facts.takeKept = true;
            touched.facts.takeIsAfter = true;
            touched.facts.takeCount = 1;
            touched.facts.takeBytes = 1719900;
        }
        entry.slots.push_back(touched);
    }
    const std::vector<history::rows::CardRow> cardRows = history::rows::forCard({ entry });
    if (cardRows.size() != 1)
        throw Error("forCard worded " + std::to_string(cardRows.size()) + " rows for one entry");
    const history::rows::CardRow& row = cardRows.front();
    HistoryWindow::Row seen;
    seen.when = "23 Sep 21:50";
    seen.action = juce::String(row.action);
    seen.detail = juce::String(row.detail);
    seen.state = juce::String(row.state);
    seen.slots = row.slots();
    seen.playable = row.playable();
    seen.restorable = row.restorable();
    seen.pinned = row.pinned;
    seen.op = row.op;
    seen.isSnapshot = row.kind == "snapshot";
    seen.restorableSlots = row.restorableSlots();
    return seen;
}

// Where a badge position starts, as the window lays them out: the gutter,
// the clock, then 30-wide badges 4 apart. A click lands 5 in.
int badgeX(int position) { return 12 + 110 + position * (30 + 4) + 5; }

std::vector<HistoryWindow::Row> timeline()
{
    return {
        row(1, "Pushed", { 12 }, true, true),
        row(2, "Trimmed", { 12 }, false, false), // take no longer kept
        row(3, "Swapped slots 12 and 43", { 12, 43 }, false, false),
        row(4, "Renamed", { 7 }, false, true),
    };
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Maintenance exposes only the card sweep, and keeps its write gate.
    {
        bool connected = true;
        bool writable = true;
        int cleaned = 0;
        AppMenu menu({ .cleanJunk = [&] { ++cleaned; },
                       .maintenanceEnabled = [&] { return connected; },
                       .cleanJunkEnabled = [&] { return writable; } });
        const auto maintenance = menu.getMenuForIndex(1, "Maintenance");
        CHECK_EQ(maintenance.getNumItems(), 1);
        juce::PopupMenu::MenuItemIterator items(maintenance);
        while (items.next()) {
            const auto& item = items.getItem();
            CHECK_EQ(item.text, juce::String("Clean junk from the pedal"));
            CHECK(item.isEnabled);
            menu.menuItemSelected(item.itemID, 1);
        }
        CHECK_EQ(cleaned, 1);
        for (const bool cardWritable : { true, false }) {
            writable = cardWritable;
            connected = !cardWritable;
            const auto disabled = menu.getMenuForIndex(1, "Maintenance");
            juce::PopupMenu::MenuItemIterator disabledItems(disabled);
            while (disabledItems.next())
                CHECK(!disabledItems.getItem().isEnabled);
        }
    }

    // --- empty: says so, offers nothing ---
    {
        HistoryWindow window;
        CHECK_EQ(window.visibleRows(), 0);
        CHECK_EQ(window.emptyText(), juce::String("Nothing has happened yet."));
        CHECK(!window.playEnabled() && !window.restoreEnabled() && !window.exportEnabled()
              && !window.pinEnabled());
        int calls = 0;
        window.onPlay = [&](std::int64_t) { ++calls; };
        window.onRestore = [&](std::int64_t) { ++calls; };
        window.onExportTake = [&](std::int64_t) { ++calls; };
        window.onPin = [&](std::int64_t, bool) { ++calls; };
        window.play();
        window.restore();
        window.exportTake();
        window.togglePin();
        CHECK_EQ(calls, 0);
        CHECK(!window.filter().has_value());
    }

    // --- the rows, oldest first, newest selected; the offers follow the row ---
    {
        HistoryWindow window;
        window.show(timeline());
        CHECK_EQ(window.visibleRows(), 4);
        CHECK_EQ(window.emptyText(), juce::String());
        CHECK(window.selected() != nullptr && window.selected()->op == 4);
        CHECK(!window.playEnabled());   // the rename has no take
        CHECK(!window.exportEnabled());
        CHECK(window.restoreEnabled());
        CHECK(window.pinEnabled());

        window.selectVisible(0); // the push whose take is kept
        CHECK(window.selected()->op == 1);
        CHECK(window.playEnabled());
        CHECK(window.exportEnabled());
        CHECK(window.restoreEnabled());

        window.selectVisible(1); // the trim whose take is gone
        CHECK(!window.playEnabled());
        CHECK(!window.exportEnabled());
        CHECK(!window.restoreEnabled());
        CHECK(window.pinEnabled()); // a pin is about the row, not its bytes
    }

    // --- a badge filters to its slot; a swap answers to both; "All slots" widens ---
    {
        HistoryWindow window;
        window.show(timeline());
        window.setFilter(12);
        CHECK_EQ(window.visibleRows(), 3);
        CHECK(window.filter() == 12);
        CHECK(window.visibleRow(2)->op == 3); // the swap
        window.setFilter(43);
        CHECK_EQ(window.visibleRows(), 1);
        CHECK(window.visibleRow(0)->op == 3);
        CHECK(window.selected() != nullptr && window.selected()->op == 3);
        window.setFilter(7);
        CHECK_EQ(window.visibleRows(), 1);
        CHECK(window.visibleRow(0)->op == 4);
        window.setFilter(99);
        CHECK_EQ(window.visibleRows(), 0);
        CHECK_EQ(window.emptyText(), juce::String("Nothing has happened to slot 99 yet."));
        CHECK(!window.playEnabled() && !window.restoreEnabled() && !window.pinEnabled());
        window.setFilter(std::nullopt);
        CHECK_EQ(window.visibleRows(), 4);
        CHECK(!window.filter().has_value());
        CHECK_THROWS(window.setFilter(0), "1..99");
        CHECK_THROWS(window.setFilter(100), "1..99");

        // the badge under the pointer: the swap's first badge is 12, its second 43
        const int swapRow = 2;
        CHECK(window.badgeAt(swapRow, 0) == std::nullopt);       // the clock
        CHECK(window.badgeAt(swapRow, 12 + 110 + 5) == 12);      // first badge
        CHECK(window.badgeAt(swapRow, 12 + 110 + 34 + 5) == 43); // second badge
        CHECK(window.badgeAt(swapRow, 12 + 110 + 68 + 5) == std::nullopt); // no third
        CHECK(window.badgeAt(0, 12 + 110 + 34 + 5) == std::nullopt);       // the push has one
        CHECK(window.badgeAt(99, 130) == std::nullopt);                    // no such row
    }

    // --- the selection survives a filter and a refresh; a vanished row lets go ---
    {
        HistoryWindow window;
        window.show(timeline());
        window.selectVisible(0); // op 1
        window.setFilter(12);
        CHECK(window.selected() != nullptr && window.selected()->op == 1);
        window.show(timeline()); // refreshed by the owner
        CHECK(window.selected() != nullptr && window.selected()->op == 1);
        CHECK(window.filter() == 12);
        window.setFilter(7); // op 1 is not there: the newest visible is selected
        CHECK(window.selected() != nullptr && window.selected()->op == 4);
    }

    // --- the callbacks carry the row a player acted on ---
    {
        HistoryWindow window;
        std::vector<std::int64_t> played, restored, exported;
        std::vector<std::pair<std::int64_t, bool>> pinned;
        window.onPlay = [&](std::int64_t op) { played.push_back(op); };
        window.onRestore = [&](std::int64_t op) { restored.push_back(op); };
        window.onExportTake = [&](std::int64_t op) { exported.push_back(op); };
        window.onPin = [&](std::int64_t op, bool state) { pinned.push_back({ op, state }); };
        window.show(timeline());

        window.selectVisible(0);
        window.play();
        window.exportTake();
        window.restore();
        CHECK(played == (std::vector<std::int64_t> { 1 }));
        CHECK(exported == (std::vector<std::int64_t> { 1 }));
        CHECK(restored == (std::vector<std::int64_t> { 1 }));

        window.selectVisible(1); // the trim: nothing to play, nothing to restore
        window.play();
        window.exportTake();
        window.restore();
        CHECK_EQ(played.size(), 1u);
        CHECK_EQ(exported.size(), 1u);
        CHECK_EQ(restored.size(), 1u);

        // a pin: the owner hears the op and the new state, the row shows it
        CHECK_EQ(window.pinButtonText(), juce::String("Pin"));
        window.togglePin();
        CHECK(pinned == (std::vector<std::pair<std::int64_t, bool>> { { 2, true } }));
        CHECK(window.selected()->pinned);
        CHECK_EQ(window.pinButtonText(), juce::String("Unpin"));
        window.togglePin();
        CHECK_EQ(pinned.size(), 2u);
        CHECK(pinned.size() == 2 && pinned.back() == std::make_pair(std::int64_t { 2 }, false));
        CHECK(!window.selected()->pinned);
        CHECK_EQ(window.pinButtonText(), juce::String("Pin"));
        // the owner's refresh carries the pin the store now has
        auto rows = timeline();
        rows[1].pinned = true;
        window.show(rows);
        window.selectVisible(1);
        CHECK_EQ(window.pinButtonText(), juce::String("Unpin"));
    }


    // --- busy: nothing is offered, and nothing goes out ---
    {
        HistoryWindow window;
        int calls = 0;
        window.onPlay = [&](std::int64_t) { ++calls; };
        window.onRestore = [&](std::int64_t) { ++calls; };
        window.onExportTake = [&](std::int64_t) { ++calls; };
        window.onPin = [&](std::int64_t, bool) { ++calls; };
        window.show(timeline());
        window.selectVisible(0);
        window.setBusy(true);
        CHECK(!window.playEnabled() && !window.restoreEnabled() && !window.exportEnabled()
              && !window.pinEnabled());
        window.play();
        window.restore();
        window.exportTake();
        window.togglePin();
        CHECK_EQ(calls, 0);
        CHECK(!window.selected()->pinned);
        window.setBusy(false);
        CHECK(window.playEnabled() && window.restoreEnabled() && window.exportEnabled()
              && window.pinEnabled());
    }

    // A snapshot offers individual changed slots; filtering to an unchanged slot disables Restore.
    {
        HistoryWindow window;
        HistoryWindow::Row baseline;
        baseline.op = 42;
        baseline.action = "Card first seen";
        baseline.slots = { 1, 2, 3 };
        baseline.isSnapshot = true;
        baseline.restorable = true;
        baseline.restorableSlots = { 2 };
        window.show({ baseline });
        window.selectVisible(0);
        CHECK(window.restoreEnabled());
        window.setFilter(1);
        window.selectVisible(0);
        CHECK(!window.restoreEnabled());
        window.setFilter(2);
        window.selectVisible(0);
        CHECK(window.restoreEnabled());
        int restores = 0;
        window.onRestore = [&](std::int64_t op) { CHECK_EQ(op, 42); ++restores; };
        window.restore();
        CHECK_EQ(restores, 1);
        window.setFilter(3);
        window.restore();
        CHECK_EQ(restores, 1);
    }
    // --- #143: the snapshot row, as the store words it, says "99 slots" where
    // its badges would be, in the strip and in the hint; the words are the
    // window's, the sentence is forCard's ---
    {
        HistoryWindow window;
        window.show({ firstSeen({ 57 }) });
        const HistoryWindow::Row& seen = *window.visibleRow(0);
        CHECK_EQ(seen.slots.size(), 99u);
        CHECK(seen.isSnapshot);
        CHECK(seen.action.isNotEmpty());
        CHECK(seen.detail.isNotEmpty());
        CHECK(!seen.detail.contains("slots")); // forCard counts takes and bytes, not slots
        CHECK(!seen.action.contains("slots"));
        CHECK_EQ(window.badgeStripText(0), juce::String("99 slots"));
        const juce::String hint = window.hintAt(0);
        CHECK(hint.contains("99 slots"));
        CHECK(hint.contains(seen.action));
        CHECK(hint.contains(seen.detail));
        CHECK(hint.indexOf("99 slots") < hint.indexOf(seen.action)); // where the strip is: before the sentence
        window.setFilter(57); // the words stay, the filter has no badge to light
        CHECK_EQ(window.badgeStripText(0), juce::String("99 slots"));
        CHECK(window.hintAt(0).contains("99 slots"));
    }

    // --- #143: a snapshot of all 99 slots wears no badge; nothing on it filters ---
    {
        HistoryWindow window;
        window.show({ firstSeen({ 57 }), row(8, "Pushed", { 12 }, true, true) });
        for (int position = 0; position < 99; ++position) {
            CHECK(window.badgeAt(0, badgeX(position)) == std::nullopt);
            window.clickAt(0, badgeX(position));
        }
        CHECK(window.badgeAt(0, badgeX(99)) == std::nullopt);
        CHECK(window.badgeAt(0, window.getWidth() - 1) == std::nullopt);
        CHECK(window.badgeAt(0, 100000) == std::nullopt);
        window.clickAt(0, window.getWidth() - 1);
        window.clickAt(0, 0);
        CHECK(!window.filter().has_value()); // no click on the snapshot row filtered
        CHECK_EQ(window.visibleRows(), 2);

        // the push's badge still filters: the limit is the row's, not the window's
        window.clickAt(1, badgeX(0));
        CHECK(window.filter() == 12);
    }

    // --- #143: the filter finds the snapshot by what it touched; Restore goes by slot ---
    {
        HistoryWindow window;
        window.show({ firstSeen({ 57 }) });
        window.setFilter(57);
        CHECK_EQ(window.visibleRows(), 1);
        CHECK(window.visibleRow(0)->op == 7);
        window.selectVisible(0);
        CHECK(window.restoreEnabled());
        int restores = 0;
        window.onRestore = [&](std::int64_t op) { CHECK_EQ(op, 7); ++restores; };
        window.restore();
        CHECK_EQ(restores, 1);
        CHECK(window.badgeAt(0, badgeX(0)) == std::nullopt); // behind the filter, still no badge

        window.setFilter(58); // touched, so shown — but nothing of 58's to go back to
        CHECK_EQ(window.visibleRows(), 1);
        window.selectVisible(0);
        CHECK(!window.restoreEnabled());
        window.restore();
        CHECK_EQ(restores, 1);

        // the same slot in front, when the snapshot holds nothing for it
        window.show({ firstSeen({}) });
        window.setFilter(57);
        CHECK_EQ(window.visibleRows(), 1);
        window.selectVisible(0);
        CHECK(!window.restoreEnabled());
        window.restore();
        CHECK_EQ(restores, 1);
    }

    // --- #143: five slots: two badges, then a "+3" chip that filters nothing ---
    {
        HistoryWindow window;
        window.show({ row(1, "Normalized 5 slots", { 3, 7, 12, 40, 99 }, false, false) });
        CHECK(window.badgeAt(0, badgeX(0)) == 3);
        CHECK(window.badgeAt(0, badgeX(1)) == 7);
        CHECK(window.badgeAt(0, badgeX(2)) == std::nullopt); // the chip
        CHECK(window.badgeAt(0, badgeX(3)) == std::nullopt); // 40 is counted, not worn
        CHECK(window.badgeAt(0, badgeX(4)) == std::nullopt); // 99 too
        CHECK_EQ(window.badgeStripText(0), juce::String("3 7 +3"));
        window.clickAt(0, badgeX(2));
        CHECK(!window.filter().has_value());
        window.clickAt(0, badgeX(3));
        CHECK(!window.filter().has_value());
        window.clickAt(0, badgeX(1));
        CHECK(window.filter() == 7);
        CHECK_EQ(window.visibleRows(), 1);
        CHECK_EQ(window.badgeStripText(0), juce::String("3 7 +3")); // 7 was worn already

        // a counted slot still answers the filter — and once filtered to, it
        // is worn in the last badge position, the chip still counting three
        window.setFilter(40);
        CHECK_EQ(window.visibleRows(), 1);
        CHECK_EQ(window.badgeStripText(0), juce::String("3 40 +3"));
        CHECK(window.badgeAt(0, badgeX(0)) == 3);
        CHECK(window.badgeAt(0, badgeX(1)) == 40);
        CHECK(window.badgeAt(0, badgeX(2)) == std::nullopt);
        window.clickAt(0, badgeX(2));
        CHECK(window.filter() == 40);
        window.clickAt(0, badgeX(0)); // the badge that stayed still filters
        CHECK(window.filter() == 3);
        CHECK_EQ(window.badgeStripText(0), juce::String("3 7 +3"));
        window.setFilter(99);
        CHECK_EQ(window.badgeStripText(0), juce::String("3 99 +3"));
        CHECK(window.badgeAt(0, badgeX(1)) == 99);
        window.setFilter(std::nullopt);
        CHECK_EQ(window.badgeStripText(0), juce::String("3 7 +3"));
        window.clickAt(0, badgeX(0));
        CHECK(window.filter() == 3);
    }

    // --- #143: exactly three slots: three badges, no chip ---
    {
        HistoryWindow window;
        window.show({ row(1, "Normalized 3 slots", { 5, 6, 8 }, false, false) });
        CHECK(window.badgeAt(0, badgeX(0)) == 5);
        CHECK(window.badgeAt(0, badgeX(1)) == 6);
        CHECK(window.badgeAt(0, badgeX(2)) == 8);
        CHECK(window.badgeAt(0, badgeX(3)) == std::nullopt);
        CHECK_EQ(window.badgeStripText(0), juce::String("5 6 8"));
        window.clickAt(0, badgeX(2));
        CHECK(window.filter() == 8);
        CHECK_EQ(window.badgeStripText(0), juce::String("5 6 8"));
    }

    // --- #143: the hint is the whole line, in the row's order ---
    {
        HistoryWindow window;
        HistoryWindow::Row full = row(1, "Normalized slot 12", { 12 }, true, true);
        full.when = "23 Sep 21:54";
        full.detail = "already peaking at the -1 dBTP ceiling (measured -19.2 LUFS), nothing to do";
        full.state = "failed";
        full.audio = "take kept";
        HistoryWindow::Row terse = row(2, "Renamed", { 7 }, false, true);
        terse.when = "23 Sep 21:55";
        window.show({ full, terse });

        const juce::String hint = window.hintAt(0);
        for (const juce::String& part : { full.when, full.action, full.detail, full.state, full.audio })
            CHECK(hint.contains(part));
        CHECK(hint.indexOf(full.when) < hint.indexOf(full.action));
        CHECK(hint.indexOf(full.action) < hint.indexOf(full.detail));
        CHECK(hint.indexOf(full.detail) < hint.indexOf(full.state));
        CHECK(hint.indexOf(full.state) < hint.indexOf(full.audio));
        CHECK(hint.startsWith(full.when));
        CHECK(hint.endsWith(full.audio));

        // a row with nothing but a clock and an action: what joins them is
        // whatever the window chose, and it appears once between parts —
        // never doubled for a missing part, never at either end
        const juce::String bare = window.hintAt(1);
        CHECK(bare.startsWith(terse.when));
        CHECK(bare.endsWith(terse.action));
        const juce::String join =
            bare.substring(terse.when.length(), bare.length() - terse.action.length());
        CHECK(join.trim().isNotEmpty());
        CHECK_EQ(hint, full.when + join + full.action + join + full.detail + join + full.state
                           + join + full.audio);
        CHECK(!hint.contains(join + join));
        CHECK(!bare.contains(join + join));

        CHECK(window.hintAt(2).isEmpty()); // no such row
        CHECK(window.hintAt(-1).isEmpty());

        // what the strip only counts is spelled out, between the clock and
        // the sentence; a row whose badges say it all adds nothing
        HistoryWindow::Row many = row(3, "Normalized 5 slots", { 3, 7, 12, 40, 99 }, false, false);
        HistoryWindow::Row three = row(4, "Normalized 3 slots", { 5, 6, 8 }, false, false);
        window.show({ full, terse, many, three });
        const juce::String counted = window.hintAt(2);
        CHECK(counted.contains("slots 3, 7, 12, 40, 99"));
        CHECK(counted.indexOf(many.when) < counted.indexOf("slots 3, 7, 12, 40, 99"));
        CHECK(counted.indexOf("slots 3, 7, 12, 40, 99") < counted.indexOf(many.action));
        CHECK(!counted.contains(join + join));
        CHECK_EQ(window.hintAt(3), three.when + join + three.action); // its three badges say it all
        CHECK(!window.hintAt(0).contains(join + "slots"));
        window.setFilter(40); // the filtered slot came out of hiding; the list is still whole
        CHECK(window.hintAt(0).contains("slots 3, 7, 12, 40, 99"));
        window.setFilter(std::nullopt);

        // the pin, which the row paints as a diamond, is a word in the hint
        window.selectVisible(1);
        CHECK(!window.hintAt(1).contains("pinned"));
        window.togglePin();
        CHECK(window.hintAt(1).endsWith("pinned"));
        CHECK(window.hintAt(1).startsWith(terse.when));
        CHECK(!window.hintAt(1).contains(join + join));
        CHECK(!window.hintAt(0).contains("pinned"));
        window.togglePin();
        CHECK(!window.hintAt(1).contains("pinned"));
    }

    return testkit::summary("history_window_tests");
}
