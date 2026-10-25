// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The History tab's pane, driven without a mouse or a window: the menu a row
// offers, what it says, and what choosing a line does. A row is clicked, a
// menu opens; the tab has no buttons left to press.
#include "support.hpp"
#include "../app/HistoryPane.h"

#include <cstdint>
#include <vector>

using loopercat::HistoryPane;
using Entry = loopercat::historymenu::Entry;

namespace
{

HistoryPane::Row row(std::int64_t op, bool takeKept, bool restorable)
{
    return { "now", "Pushed", "song.wav", "", takeKept ? "take kept" : "", { takeKept }, restorable, op };
}

const HistoryPane::MenuLine* lineOf(const std::vector<HistoryPane::MenuLine>& lines, Entry entry)
{
    for (const auto& line : lines)
        if (line.entry == entry)
            return &line;
    return nullptr;
}

bool enabled(const HistoryPane& pane, std::int64_t op, Entry entry)
{
    const auto lines = pane.menuFor(op);
    const auto* line = lineOf(lines, entry);
    return line != nullptr && line->enabled;
}

struct Calls {
    std::vector<std::int64_t> restored, exported;
    std::vector<int> cleared;
    std::vector<int> slots; // the slot each restore or export said it was about
};

void listen(HistoryPane& pane, Calls& calls)
{
    pane.onRestore = [&calls](int slot, std::int64_t op) { calls.restored.push_back(op); calls.slots.push_back(slot); };
    pane.onExportTake = [&calls](int slot, std::int64_t op, int) { calls.exported.push_back(op); calls.slots.push_back(slot); };
    pane.onClearHistory = [&calls](int slot) { calls.cleared.push_back(slot); };
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI runtime;

    // --- the menu's shape and words: the same four lines on every row ---
    {
        HistoryPane pane;
        pane.setRows({ row(7, true, true), row(8, false, false) }, 4);
        for (const std::int64_t op : { std::int64_t { 7 }, std::int64_t { 8 } }) {
            const auto lines = pane.menuFor(op);
            CHECK_EQ(lines.size(), std::size_t { 4 });
            if (lines.size() != 4)
                continue;
            CHECK(lines[0].entry == Entry::restore);
            CHECK(lines[1].entry == Entry::exportTake);
            CHECK(lines[2].entry == Entry::separator);
            CHECK(lines[3].entry == Entry::clearHistory);
            CHECK_EQ(lines[0].text.toStdString(), std::string("Restore this state"));
            CHECK(lines[1].text == juce::String::fromUTF8("Export take\xe2\x80\xa6"));
            CHECK(!lines[2].enabled);
            CHECK(lines[3].text == juce::String::fromUTF8("Clear history of this slot\xe2\x80\xa6"));
            for (const auto& line : lines)
                CHECK(!line.text.containsIgnoreCase("play")); // the tab no longer plays a take
        }
        CHECK(pane.menuFor(9).empty()); // no such row, no menu
    }

    // --- what a row can do, row by row: restore and export follow its own
    // facts, clearing is about the slot and always there ---
    for (const bool takeKept : { false, true })
        for (const bool restorable : { false, true }) {
            HistoryPane pane;
            Calls calls;
            listen(pane, calls);
            pane.setRows({ row(3, takeKept, restorable) }, 4);
            CHECK_EQ(enabled(pane, 3, Entry::restore), restorable);
            CHECK_EQ(enabled(pane, 3, Entry::exportTake), takeKept);
            CHECK(enabled(pane, 3, Entry::clearHistory));

            pane.choose(4, 3, Entry::restore, 0);
            pane.choose(4, 3, Entry::exportTake, 1);
            pane.choose(4, 3, Entry::separator, 0); // a separator is never a choice
            CHECK_EQ(calls.restored.size(), std::size_t { restorable ? 1u : 0u });
            CHECK_EQ(calls.exported.size(), std::size_t { takeKept ? 1u : 0u });
            CHECK(calls.restored.empty() || calls.restored.front() == 3);
            CHECK(calls.exported.empty() || calls.exported.front() == 3);
            for (const int slot : calls.slots)
                CHECK_EQ(slot, 4); // the slot the menu was opened on
            CHECK(calls.cleared.empty());

            pane.choose(4, 3, Entry::clearHistory, 0);
            CHECK(calls.cleared == std::vector<int> { 4 }); // the slot, not the row's operation
        }

    // --- the pedal busy: every line grey, no choice gets through, and the
    // menu comes back whole when the job ends ---
    {
        HistoryPane pane;
        Calls calls;
        listen(pane, calls);
        pane.setRows({ row(1, true, true), row(2, true, true) }, 4);
        pane.setBusy(true);
        for (const auto& line : pane.menuFor(1))
            CHECK(!line.enabled);
        for (const Entry entry : { Entry::restore, Entry::exportTake, Entry::clearHistory })
            pane.choose(4, 1, entry, entry == Entry::exportTake ? 1 : 0);
        CHECK(calls.restored.empty() && calls.exported.empty() && calls.cleared.empty());

        pane.setBusy(false);
        CHECK(enabled(pane, 1, Entry::restore));
        CHECK(enabled(pane, 1, Entry::exportTake));
        CHECK(enabled(pane, 1, Entry::clearHistory));
        pane.choose(4, 2, Entry::restore, 0);
        CHECK(calls.restored == std::vector<std::int64_t> { 2 }); // the row the menu was for
    }

    // --- a menu left open while the rows were read again: its choice is
    // about an operation, and one that is gone, or no longer offers the
    // line, does nothing ---
    {
        HistoryPane pane;
        Calls calls;
        listen(pane, calls);
        pane.setRows({ row(5, true, true), row(6, true, false) }, 4);
        pane.setRows({ row(5, false, false), row(6, true, false) }, 4); // its take released, a newer row
        pane.choose(4, 5, Entry::exportTake, 1);
        pane.choose(4, 5, Entry::restore, 0);
        CHECK(calls.exported.empty() && calls.restored.empty());

        pane.setRows({ row(6, true, false) }, 4); // row 5 forgotten
        for (const Entry entry : { Entry::restore, Entry::exportTake, Entry::clearHistory })
            pane.choose(4, 5, entry, entry == Entry::exportTake ? 1 : 0);
        CHECK(calls.cleared.empty());

        pane.setRows({ row(6, true, false) }, 9); // another slot selected, and its row's menu opened
        pane.choose(9, 6, Entry::clearHistory, 0);
        CHECK(calls.cleared == std::vector<int> { 9 });

        pane.clear(); // no slot at all: nothing to offer, nothing to clear
        CHECK(pane.menuFor(6).empty());
        pane.choose(9, 6, Entry::clearHistory, 0);
        pane.choose(0, 6, Entry::clearHistory, 0);
        CHECK_EQ(calls.cleared.size(), std::size_t { 1 });
    }

    // --- ATTACK (review of feat/history-row-menu): the menu was opened on one
    // slot's row and the slot changed under it. "Clear history of this slot..."
    // was read against the slot the row was shown in, and "Restore this state"
    // against that slot's row. A swap's operation is a row of BOTH its slots,
    // so the operation alone does not say which slot the menu was about: a
    // choice that arrives once another slot is selected must not act on the
    // slot now shown. (The case above, "another slot selected", clears the new
    // slot; by this theory it must clear none. The menu carries the slot it
    // was opened on: there, the choice is made on slot 9's own menu.) ---
    {
        HistoryPane pane;
        Calls calls;
        listen(pane, calls);
        pane.setRows({ row(6, false, true) }, 4); // the swap of slots 4 and 9, as slot 4 shows it
        // the menu opens here, on slot 4's row of operation 6, then...
        pane.setRows({ row(6, false, true) }, 9); // ...slot 9 is selected: the same operation, its other row
        pane.choose(4, 6, Entry::clearHistory, 0);
        pane.choose(4, 6, Entry::restore, 0);
        CHECK(calls.cleared.empty());
        CHECK(calls.restored.empty());
        pane.choose(9, 6, Entry::restore, 0); // slot 9's own menu does act, and says so
        CHECK(calls.restored == std::vector<std::int64_t> { 6 });
        CHECK(calls.slots == std::vector<int> { 9 });
    }

    // --- rows read again with a row added before the one the menu is about:
    // the choice stays with its operation, not with its place in the list ---
    {
        HistoryPane pane;
        Calls calls;
        listen(pane, calls);
        pane.setRows({ row(5, true, true) }, 4);
        pane.setRows({ row(2, false, false), row(5, true, true) }, 4);
        pane.choose(4, 5, Entry::exportTake, 1);
        CHECK(calls.exported == std::vector<std::int64_t> { 5 });
    }

    // --- a callback nobody set is not a crash ---
    {
        HistoryPane pane;
        pane.setRows({ row(1, true, true) }, 4);
        for (const Entry entry : { Entry::restore, Entry::exportTake, Entry::clearHistory })
            pane.choose(4, 1, entry, entry == Entry::exportTake ? 1 : 0);
    }

    // --- a memory of two tracks: an export line per track, and the track chosen ---
    {
        HistoryPane pane;
        Calls calls;
        listen(pane, calls);
        std::vector<int> tracks;
        pane.onExportTake = [&calls, &tracks](int slot, std::int64_t op, int track) {
            calls.exported.push_back(op);
            calls.slots.push_back(slot);
            tracks.push_back(track);
        };
        pane.setRows({ { "now", "Original", "T1 001_1.WAV, T2 001_2.WAV", "", "take kept", { true, false }, true, 7 } }, 1);
        const auto lines = pane.menuFor(7);
        CHECK_EQ(lines.size(), std::size_t { 5 });
        if (lines.size() == 5) {
            CHECK(lines[1].text == juce::String::fromUTF8("Export track 1 take\xe2\x80\xa6") && lines[1].enabled);
            CHECK(lines[2].text == juce::String::fromUTF8("Export track 2 take\xe2\x80\xa6") && !lines[2].enabled);
            CHECK(lines[1].track == 1 && lines[2].track == 2);
        }
        pane.choose(1, 7, Entry::exportTake, 2); // nothing kept on track 2: no export
        pane.choose(1, 7, Entry::exportTake, 0); // an export of no track is no line
        pane.choose(1, 7, Entry::exportTake, 3); // nor of a track the memory has not
        CHECK(calls.exported.empty());
        pane.choose(1, 7, Entry::exportTake, 1);
        CHECK((tracks == std::vector<int> { 1 }));
        CHECK((calls.exported == std::vector<std::int64_t> { 7 }));
        // A memory of one track keeps its one line and its old words.
        pane.setRows({ row(8, true, true) }, 1);
        const auto one = pane.menuFor(8);
        CHECK(one.size() == 4 && one[1].text == juce::String::fromUTF8("Export take\xe2\x80\xa6") && one[1].track == 1);
    }

    return testkit::summary("history_pane_tests");
}
