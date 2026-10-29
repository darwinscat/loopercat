// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the table's gestures against what the card permits
// (CardPermissions.h): the REAL SlotTable, its rows from the format's
// synthetic file, its clicks delivered the way JUCE delivers them — through
// the table's model with a mouse event — no test-only seam.
//
//   - on a card this app only reads (the two-track model's), a click on the
//     One Shot or Count-In pill calls nobody, the empty-slot hint is not a
//     button, a file drag is not wanted, a row is not draggable, and a
//     double-click on the name plays the slot instead of opening an editor
//   - on an RC-5 the same click flips the pill, the same drag is wanted
//   - a table never told what it may do offers nothing
//   - the permission table itself: everything for the RC-5, nothing for the
//     two-track model or an unknown name, and the sentence names the RC-5
//   - a LUFS cell out of the history (#141) is flagged inferred, wears no
//     attention whatever the reading's verdict, its hint leads with where
//     the number came from and the double-click that measures it, a
//     measured reading replaces it, a mutation's clearing takes it away, a
//     dropped read does not; a read in flight and a slot with no loop have
//     no hint; the table says whether the LUFS column is shown

#include "support.hpp"

#include "../app/CardPermissions.h"
#include "../app/SlotTable.h"

#include <loopercat/Catalog.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

using namespace loopercat;

namespace {

juce::TableListBox* listBoxOf(juce::Component& table)
{
    for (auto* child : table.getChildren())
        if (auto* box = dynamic_cast<juce::TableListBox*>(child))
            return box;
    return nullptr;
}

int columnNamed(juce::TableListBox& box, const juce::String& name)
{
    auto& header = box.getHeader();
    for (int i = 0; i < header.getNumColumns(false); ++i) {
        const int id = header.getColumnIdOfIndex(i, false);
        if (header.getColumnName(id) == name)
            return id;
    }
    return 0;
}

juce::MouseEvent clickAt(juce::Component& on,
                         juce::ModifierKeys mods = juce::ModifierKeys::leftButtonModifier)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { 4.0f, 4.0f },
                            mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &on, &on, now, { 4.0f, 4.0f },
                            now, 1, false);
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    // --- the permission table ---

    {
        const CardPermissions rc5 = CardPermissions::of("RC-5");
        CHECK(rc5.rename && rc5.tempo && rc5.oneShot && rc5.countIn && rc5.push && rc5.swap
              && rc5.normalize && rc5.clear && rc5.trim);
        CHECK(rc5.anyWrite());
        CHECK(rc5.closed.empty());
        // The two-track model: what carries every track of a memory, nothing else.
        const CardPermissions rc500 = CardPermissions::of("RC-500");
        CHECK(rc500.rename && rc500.oneShot && rc500.pull && rc500.clear && rc500.restore);
        CHECK(!rc500.tempo && !rc500.countIn && !rc500.push && !rc500.swap
              && !rc500.normalize && !rc500.trim && !rc500.downmix && !rc500.rhythm && !rc500.playStop);
        CHECK_EQ(rc500.closed, "On the RC-500, LooperCat does rename, set one-shot, pull, clear and restore only");
        for (const char* name : { "RC-505", "", "rc-5" }) {
            const CardPermissions none = CardPermissions::of(name);
            CHECK(!none.anyWrite());
            CHECK(!none.rename && !none.tempo && !none.oneShot && !none.countIn && !none.push
                  && !none.swap && !none.normalize && !none.clear && !none.trim && !none.pull
                  && !none.restore && !none.downmix);
            CHECK(none.closed.empty());
        }
        CHECK_EQ(CardPermissions::writesOnlyTo(), "LooperCat writes only to the RC-5 and the RC-500");
        CHECK(profile::findFamily("RC-500") == &profile::kRc500);
        CHECK(profile::findFamily("RC-505") == nullptr);
    }

    // --- the table, its rows, its clicks ---

    SlotTable table;
    table.setSize(900, 400);
    std::vector<SlotRow> rows;
    // Slot 2 (row 1) sits at 6/4 (Beat 4) with its rhythm silent: the
    // count-in's "on" there would write a 4/4 pattern number, which the core
    // refuses (#149). Slot 3 (row 2) is at 6/4 with the rhythm playing,
    // where the count alone is still the player's.
    std::string text = testkit::syntheticMemoryText();
    const auto set = [&text](int slot, const char* tag, long long value) {
        text = rc0::replaceSlotBody(
            text, slot, rc0::setSectionField(rc0::slotBody(text, slot), "RHYTHM", tag, value));
    };
    set(2, "Beat", 4);
    set(2, "Pattern", 3);
    set(3, "Beat", 4);
    set(3, "Pattern", 3);
    set(3, "State", rc0::kRhythmStateOn);
    for (auto& info : catalog::listSlots(text))
        rows.push_back({ std::move(info), "", "", { "" }, std::nullopt });
    table.setRows(rows);

    juce::TableListBox* box = listBoxOf(table);
    CHECK(box != nullptr);
    if (box == nullptr)
        return testkit::summary("slot_table_harness");
    juce::TableListBoxModel* model = box->getTableListBoxModel();
    CHECK(model != nullptr);
    const int oneShotColumn = columnNamed(*box, "One Shot");
    const int countInColumn = columnNamed(*box, "Play Count-In");
    const int nameColumn = columnNamed(*box, "Name");
    const int wavColumn = columnNamed(*box, "WAV file");
    CHECK(oneShotColumn > 0 && countInColumn > 0 && nameColumn > 0 && wavColumn > 0);

    int oneShotClicks = 0, countInClicks = 0, hintClicks = 0, activations = 0;
    table.onOneShotToggled = [&oneShotClicks](int, int) { ++oneShotClicks; };
    table.onCountInToggled = [&countInClicks](int) { ++countInClicks; };
    table.onEmptyWavCellClicked = [&hintClicks](int) { ++hintClicks; };
    table.onSlotActivated = [&activations](int) { ++activations; };
    table.onRenameCommitted = [](int, juce::String) {};
    table.onSwapRequested = [](int, int) {};

    const auto click = [&](int column) { model->cellClicked(0, column, clickAt(table)); };
    const auto doubleClick = [&](int column) {
        model->cellDoubleClicked(0, column, clickAt(table));
    };

    // Never told what it may do: nothing — and these rows are an RC-5's, the
    // model everything is open for. A table an owner forgot to wire offers
    // no gesture at all, so the forgotten wiring is caught here rather than
    // by a player whose pill stopped working.
    CHECK(CardPermissions::of("RC-5").oneShot); // the rows' own model would allow it
    click(oneShotColumn);
    click(countInColumn);
    click(wavColumn);
    CHECK_EQ(oneShotClicks, 0);
    CHECK_EQ(countInClicks, 0);
    CHECK_EQ(hintClicks, 0);
    CHECK(!table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));

    // A card nothing is open on: the same clicks call nobody, the drag is
    // not wanted, and the name's double-click plays rather than edits.
    table.permissions = [] { return CardPermissions::of("RC-505"); };
    click(oneShotColumn);
    click(countInColumn);
    click(wavColumn);
    CHECK_EQ(oneShotClicks, 0);
    CHECK_EQ(countInClicks, 0);
    CHECK_EQ(hintClicks, 0);
    CHECK(!table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));
    doubleClick(nameColumn);
    CHECK_EQ(activations, 1);

    // The two-track model: Count-In and push stay closed — a lamp, no drag
    // — while One Shot is open (a flag of each track) and its click goes
    // through; the name, also open, takes the editing path on a double-click
    // instead of playing.
    table.permissions = [] { return CardPermissions::of("RC-500"); };
    click(oneShotColumn);
    click(countInColumn);
    CHECK_EQ(oneShotClicks, 1);
    CHECK_EQ(countInClicks, 0);
    CHECK(!table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));
    table.onRenameCommitted = nullptr;
    doubleClick(nameColumn);
    CHECK_EQ(activations, 1); // unchanged: the edit path, no play

    // The RC-5: the pill is the toggle, the hint is a button, the drag is
    // wanted, and the name's double-click takes the editing path instead of
    // playing. (The editor itself grabs keyboard focus, which a component
    // off any screen cannot give; with no rename handler the path is taken
    // and the editor is not opened — the gesture is what is under test.)
    table.permissions = [] { return CardPermissions::of("RC-5"); };
    click(oneShotColumn);
    click(countInColumn);
    click(wavColumn);
    CHECK_EQ(oneShotClicks, 2);
    CHECK_EQ(countInClicks, 1);
    CHECK_EQ(hintClicks, 1);
    CHECK(table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));
    table.onRenameCommitted = nullptr;
    doubleClick(nameColumn);
    CHECK_EQ(activations, 1); // unchanged: the edit path, no play

    // A Count-In dot whose click the core would refuse is a lamp on an RC-5
    // too, and its hover says why in the card's words; a dot that can be
    // clicked, and every other cell, says nothing.
    model->cellClicked(1, countInColumn, clickAt(table));
    CHECK_EQ(countInClicks, 1);
    CHECK(model->getCellTooltip(1, countInColumn).contains("6/4"));
    CHECK(model->getCellTooltip(1, countInColumn).contains("switch the rhythm on at the pedal"));
    model->cellClicked(2, countInColumn, clickAt(table));
    CHECK_EQ(countInClicks, 2);
    CHECK(model->getCellTooltip(2, countInColumn).isEmpty());
    CHECK(model->getCellTooltip(0, countInColumn).isEmpty());
    CHECK(model->getCellTooltip(1, oneShotColumn).isEmpty());
    model->cellClicked(1, oneShotColumn, clickAt(table)); // One Shot is not beat-relative
    CHECK_EQ(oneShotClicks, 3);

    // A right-click is the context menu on either card — a read of the
    // situation, never a write — so it is not gated here.
    int menus = 0;
    table.onSlotContextMenu = [&menus](int, juce::Point<int>) { ++menus; };
    table.permissions = [] { return CardPermissions::of("RC-500"); };
    model->cellClicked(0, nameColumn, clickAt(table, juce::ModifierKeys::rightButtonModifier));
    CHECK_EQ(menus, 1);

    // --- the LUFS cell from the history (#141) ---
    {
        const int lufsColumn = columnNamed(*box, "LUFS");
        CHECK(lufsColumn > 0);
        // Row 2 holds a loop (the column draws a cell only for one); row 3
        // does not, whatever the table remembers for its slot.
        const int row = 2;
        rows[static_cast<std::size_t>(row)].info.hasAudio = true;
        table.setRows(rows);
        const int slot = rows[static_cast<std::size_t>(row)].info.slot;
        const int emptySlot = rows[3].info.slot;
        CHECK(!rows[3].info.hasAudio);
        const SlotTable::LoudnessCell measured { "-22.8", true, false,
                                                 juce::String::fromUTF8("-22.8 LUFS \xc2\xb7 4.8 dB below target -18.0"),
                                                 false,
                                                 juce::String::fromUTF8("Peak -3.1 dBTP \xc2\xb7 target -18.0 LUFS.") };
        const juce::Time when(2026, 8, 14, 9, 30); // 14 Sep 2026 09:30, local: JUCE counts months from 0
        // inferred: flagged, the reading's own words, no verdict — the
        // measured reading asked for attention, a guess does not — and the
        // provenance first in the hint, then the gesture that measures it
        CHECK(measured.attention);
        table.setLoudness(slot, SlotTable::LoudnessCell::fromHistory(measured, when));
        const SlotTable::LoudnessCell* cell = table.loudnessFor(slot);
        CHECK(cell != nullptr);
        if (cell != nullptr) {
            CHECK(cell->inferred);
            CHECK(cell->text == measured.text && cell->detail == measured.detail);
            CHECK(!cell->attention);
            CHECK(!cell->pending && !cell->damaged);
        }
        const juce::String hint = model->getCellTooltip(row, lufsColumn);
        CHECK(hint.startsWith("From the history"));
        CHECK(hint.contains("measured 14 Sep 2026 09:30"));
        CHECK(hint.contains("not re-measured"));
        CHECK(hint.contains("Double-click to measure it now."));
        CHECK(!hint.contains("Check loudness")); // not on every card, so not promised
        CHECK(hint.endsWith(measured.tooltip));
        // a cell that was measured is never mistaken for one out of the history
        CHECK(!measured.inferred);
        // a measured reading replaces it: no flag, its own hint
        table.setLoudness(slot, measured);
        CHECK(table.loudnessFor(slot) != nullptr && !table.loudnessFor(slot)->inferred);
        CHECK(model->getCellTooltip(row, lufsColumn) == measured.tooltip);
        // a read in flight has nothing to explain yet
        table.setLoudness(slot, { juce::String::fromUTF8("\xe2\x80\xa6"), false, true });
        CHECK(model->getCellTooltip(row, lufsColumn).isEmpty());
        // the slot's audio changed: cleared like any cell
        table.setLoudness(slot, SlotTable::LoudnessCell::fromHistory(measured, when));
        table.clearLoudness(slot);
        CHECK(table.loudnessFor(slot) == nullptr);
        CHECK(model->getCellTooltip(row, lufsColumn).isEmpty());
        // a read that will never land takes back only a pending cell: an
        // inferred one is not a read in flight
        table.setLoudness(slot, SlotTable::LoudnessCell::fromHistory(measured, when));
        table.clearPendingLoudness(slot);
        CHECK(table.loudnessFor(slot) != nullptr && table.loudnessFor(slot)->inferred);
        // a slot with no loop draws no cell, so its hint is empty too
        table.setLoudness(emptySlot, SlotTable::LoudnessCell::fromHistory(measured, when));
        CHECK(model->getCellTooltip(3, lufsColumn).isEmpty());
        // another column, a row that is not there: no hint
        CHECK(model->getCellTooltip(row, nameColumn).isEmpty());
        CHECK(model->getCellTooltip(500, lufsColumn).isEmpty());
        CHECK(model->getCellTooltip(-1, lufsColumn).isEmpty());
        // another card: every cell goes, the inferred with the rest
        table.clearAllLoudness();
        CHECK(table.loudnessFor(slot) == nullptr);
        CHECK(table.loudnessFor(emptySlot) == nullptr);
        // A double-click on the cell reads the slot — on a memory of one
        // track; on one with two the meter would read the first alone, and
        // its number would pass for the mix's, so nothing is asked.
        int reads = 0;
        table.onLoudnessCellDoubleClicked = [&reads](int) { ++reads; };
        model->cellDoubleClicked(row, lufsColumn, clickAt(table));
        CHECK_EQ(reads, 1);
        auto twoTrack = rows;
        twoTrack[static_cast<std::size_t>(row)].info.tracks.push_back(
            twoTrack[static_cast<std::size_t>(row)].info.tracks.front());
        twoTrack[static_cast<std::size_t>(row)].info.tracks.back().track = 2;
        table.setRows(twoTrack);
        model->cellDoubleClicked(row, lufsColumn, clickAt(table));
        CHECK_EQ(reads, 1);
        table.setRows(rows);
        // whether the column is on screen, as the preference set it
        table.setOptionalColumns(true, false, true);
        CHECK(table.loudnessColumnVisible());
        table.setOptionalColumns(true, false, false);
        CHECK(!table.loudnessColumnVisible());
    }

    // --- One Shot of a two-track memory: a dot per track, a click per dot ---
    //
    // The flag lives in each TRACK section. The cell draws a dot per track,
    // track 1 left, and a click flips the dot it lands on: the boundary is
    // the middle of the gap between them, and a click on no dot — the empty
    // rest of the cell, or before the first — writes nothing, never a third
    // track and never the last one by default.
    {
        CHECK_EQ(SlotTable::oneShotTrackAt(0, 1), 1);
        CHECK_EQ(SlotTable::oneShotTrackAt(500, 1), 1); // one track: the whole cell
        const int first = static_cast<int>(SlotTable::kDotLeft + SlotTable::kDotSize * 0.5f);
        const int second = static_cast<int>(SlotTable::kDotLeft + SlotTable::kDotSize
                                             + SlotTable::kDotGap + SlotTable::kDotSize * 0.5f);
        const int boundary = static_cast<int>(SlotTable::kDotLeft + SlotTable::kDotSize
                                               + SlotTable::kDotGap * 0.5f);
        CHECK_EQ(SlotTable::oneShotTrackAt(first, 2), 1);
        CHECK_EQ(SlotTable::oneShotTrackAt(second, 2), 2);
        CHECK_EQ(SlotTable::oneShotTrackAt(boundary - 1, 2), 1);
        CHECK_EQ(SlotTable::oneShotTrackAt(boundary + 1, 2), 2);
        const int dotsEnd = static_cast<int>(SlotTable::kDotLeft + 2 * SlotTable::kDotSize
                                              + SlotTable::kDotGap * 1.5f);
        CHECK_EQ(SlotTable::oneShotTrackAt(dotsEnd - 1, 2), 2);
        CHECK_EQ(SlotTable::oneShotTrackAt(dotsEnd + 1, 2), 0); // past the dots: nothing
        CHECK_EQ(SlotTable::oneShotTrackAt(80, 2), 0);
        CHECK_EQ(SlotTable::oneShotTrackAt(2, 2), 0);           // before the first: nothing
        CHECK_EQ(SlotTable::oneShotTrackAt(-20, 2), 0);

        std::string twoText = testkit::syntheticTwoTrackMemoryText();
        twoText = rc0::replaceSlotBody(twoText, 1,
                                       rc0::setSectionField(rc0::slotBody(twoText, 1), "TRACK2", "One", 1));
        std::vector<SlotRow> twoRows;
        for (auto& info : catalog::listSlots(twoText))
            twoRows.push_back({ std::move(info), "", "", { "" }, std::nullopt });
        table.setRows(twoRows);
        table.permissions = [] { return CardPermissions::of("RC-500"); };
        std::vector<std::pair<int, int>> flips;
        table.onOneShotToggled = [&flips](int slot, int track) { flips.emplace_back(slot, track); };
        auto& header = box->getHeader();
        const int columnX = header.getColumnPosition(header.getIndexOfColumnId(oneShotColumn, true)).getX();
        const auto clickAtX = [&](int cellX) {
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> at { static_cast<float>(columnX + cellX), 4.0f };
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at,
                                    juce::ModifierKeys::leftButtonModifier, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                    &table, &table, now, at, now, 1, false);
        };
        model->cellClicked(0, oneShotColumn, clickAtX(second));
        model->cellClicked(0, oneShotColumn, clickAtX(first));
        model->cellClicked(0, oneShotColumn, clickAtX(70)); // the empty rest of the cell
        CHECK((flips == std::vector<std::pair<int, int>> { { 1, 2 }, { 1, 1 } }));
        // The hover says what the two dots mean, track by track.
        CHECK_EQ(model->getCellTooltip(0, oneShotColumn), juce::String("T1 loops, T2 plays once"));
        // A card that keeps One Shot closed: the dots are lamps.
        table.permissions = [] { return CardPermissions::of("RC-505"); };
        model->cellClicked(0, oneShotColumn, clickAtX(second));
        CHECK_EQ(flips.size(), std::size_t { 2 });
        // A one-track row: one dot, no words on hover, every click track 1.
        table.setRows(rows);
        table.permissions = [] { return CardPermissions::of("RC-5"); };
        model->cellClicked(0, oneShotColumn, clickAtX(second));
        CHECK((flips.back() == std::pair<int, int> { 1, 1 }));
        CHECK(model->getCellTooltip(0, oneShotColumn).isEmpty());
    }

    return testkit::summary("slot_table_harness");
}
