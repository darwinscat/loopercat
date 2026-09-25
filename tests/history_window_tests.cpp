// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The History window (#73), driven without a mouse, a store or a worker,
// from what it promises:
//
//   - it shows the rows it is handed, oldest first, newest selected
//   - a slot badge filters to that slot, and "All slots" widens again
//   - a swap is one row wearing two badges, and answers to both filters
//   - the buttons offer only what a row can do: no Play or Export for a
//     take no longer kept, no Restore for a state that cannot go back
//   - a pin toggled here reaches the owner with the operation and the new
//     state, and the row shows it at once
//   - while a job runs, nothing is offered; an empty timeline says so

#include "support.hpp"

#include "../app/HistoryWindow.h"

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

    return testkit::summary("history_window_tests");
}
