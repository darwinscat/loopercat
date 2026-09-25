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
              && rc5.normalize && rc5.clear);
        CHECK(rc5.anyWrite());
        for (const char* name : { "RC-500", "RC-505", "", "rc-5" }) {
            const CardPermissions none = CardPermissions::of(name);
            CHECK(!none.anyWrite());
            CHECK(!none.rename && !none.tempo && !none.oneShot && !none.countIn && !none.push
                  && !none.swap && !none.normalize && !none.clear);
        }
        CHECK_EQ(CardPermissions::writesOnlyTo(), "LooperCat writes only to the RC-5");
        CHECK(profile::findFamily("RC-500") == &profile::kRc500);
        CHECK(profile::findFamily("RC-505") == nullptr);
    }

    // --- the table, its rows, its clicks ---

    SlotTable table;
    table.setSize(900, 400);
    std::vector<SlotRow> rows;
    for (auto& info : catalog::listSlots(testkit::syntheticMemoryText()))
        rows.push_back({ std::move(info), "", "", { "" } });
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
    table.onOneShotToggled = [&oneShotClicks](int) { ++oneShotClicks; };
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

    // A card this app only reads: the same clicks call nobody, the drag is
    // not wanted, and the name's double-click plays rather than edits.
    table.permissions = [] { return CardPermissions::of("RC-500"); };
    click(oneShotColumn);
    click(countInColumn);
    click(wavColumn);
    CHECK_EQ(oneShotClicks, 0);
    CHECK_EQ(countInClicks, 0);
    CHECK_EQ(hintClicks, 0);
    CHECK(!table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));
    doubleClick(nameColumn);
    CHECK_EQ(activations, 1);

    // The RC-5: the pill is the toggle, the hint is a button, the drag is
    // wanted, and the name's double-click takes the editing path instead of
    // playing. (The editor itself grabs keyboard focus, which a component
    // off any screen cannot give; with no rename handler the path is taken
    // and the editor is not opened — the gesture is what is under test.)
    table.permissions = [] { return CardPermissions::of("RC-5"); };
    click(oneShotColumn);
    click(countInColumn);
    click(wavColumn);
    CHECK_EQ(oneShotClicks, 1);
    CHECK_EQ(countInClicks, 1);
    CHECK_EQ(hintClicks, 1);
    CHECK(table.isInterestedInFileDrag({ "/somewhere/loop.wav" }));
    table.onRenameCommitted = nullptr;
    doubleClick(nameColumn);
    CHECK_EQ(activations, 1); // unchanged: the edit path, no play

    // A right-click is the context menu on either card — a read of the
    // situation, never a write — so it is not gated here.
    int menus = 0;
    table.onSlotContextMenu = [&menus](int, juce::Point<int>) { ++menus; };
    table.permissions = [] { return CardPermissions::of("RC-500"); };
    model->cellClicked(0, nameColumn, clickAt(table, juce::ModifierKeys::rightButtonModifier));
    CHECK_EQ(menus, 1);

    return testkit::summary("slot_table_harness");
}
