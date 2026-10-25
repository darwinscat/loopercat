// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryPane.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace
{
constexpr int kRowHeight = 26;
constexpr int kGutter = 12;
const juce::Colour kInk { 0xff8a8a99 };      // the quiet half of a row
const juce::Colour kPaper { 0xffe8e8f0 };    // what the row is about
const juce::Colour kSelected { 0xff26263a };
const juce::Colour kWarning { 0xffd9a441 };  // failed, interrupted — the window's colour

// A memory of one track exports "the take"; one of more names the track.
juce::String words(const historymenu::Item& item, std::size_t tracks)
{
    switch (item.entry) {
        case historymenu::Entry::restore: return "Restore this state";
        case historymenu::Entry::exportTake:
            return tracks == 1 ? juce::String::fromUTF8("Export take\xe2\x80\xa6")
                               : "Export track " + juce::String(item.track)
                                     + juce::String::fromUTF8(" take\xe2\x80\xa6");
        case historymenu::Entry::separator: return {};
        case historymenu::Entry::clearHistory:
            return juce::String::fromUTF8("Clear history of this slot\xe2\x80\xa6");
    }
    return {};
}

// A PopupMenu item id is never 0 — 0 is "dismissed": the line's place in the
// menu, from 1.
int itemId(std::size_t line)
{
    return static_cast<int>(line) + 1;
}
} // namespace

HistoryPane::HistoryPane()
{
    list_.setRowHeight(kRowHeight);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list_.setOutlineThickness(0);
    addAndMakeVisible(list_);

    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, kInk);
    empty_.setText("Nothing has happened to this slot yet.", juce::dontSendNotification);
    addChildComponent(empty_);
}

void HistoryPane::setRows(std::vector<Row> rows, int slot)
{
    const bool sameSlot = slot == slot_;
    const std::int64_t wasSelected = sameSlot && selected() != nullptr ? selected()->op : 0;
    rows_ = std::move(rows);
    slot_ = slot;
    list_.updateContent();
    empty_.setVisible(rows_.empty());

    // Keep the row the player was looking at; otherwise show the newest,
    // which is the bottom — a slot's history reads like a log, not a stack.
    int row = -1;
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].op == wasSelected)
            row = static_cast<int>(i);
    if (row < 0 && !rows_.empty())
        row = static_cast<int>(rows_.size()) - 1;
    list_.selectRow(row, juce::dontSendNotification);
    if (!rows_.empty())
        list_.scrollToEnsureRowIsOnscreen(static_cast<int>(rows_.size()) - 1);
    repaint();
}

void HistoryPane::clear()
{
    setRows({}, 0);
}

void HistoryPane::setBusy(bool busy)
{
    busy_ = busy;
}

const HistoryPane::Row* HistoryPane::selected() const
{
    const int row = list_.getSelectedRow();
    return row >= 0 && row < static_cast<int>(rows_.size()) ? &rows_[static_cast<std::size_t>(row)]
                                                            : nullptr;
}

const HistoryPane::Row* HistoryPane::rowFor(std::int64_t op) const
{
    for (const Row& row : rows_)
        if (row.op == op)
            return &row;
    return nullptr;
}

std::vector<HistoryPane::MenuLine> HistoryPane::menuFor(std::int64_t op) const
{
    std::vector<MenuLine> lines;
    if (const Row* row = rowFor(op))
        for (const historymenu::Item& item : historymenu::forRow({ row->restorable, row->takeKept }, busy_))
            lines.push_back({ item.entry, words(item, row->takeKept.size()), item.enabled, item.track });
    return lines;
}

void HistoryPane::choose(int slot, std::int64_t op, historymenu::Entry entry, int track)
{
    if (slot != slot_)
        return; // the menu was about a slot the pane no longer shows
    bool enabled = false;
    for (const MenuLine& line : menuFor(op))
        if (line.entry == entry && line.track == track)
            enabled = line.enabled;
    if (!enabled)
        return;
    switch (entry) {
        case historymenu::Entry::restore:
            if (onRestore) onRestore(slot, op);
            break;
        case historymenu::Entry::exportTake:
            if (onExportTake) onExportTake(slot, op, track);
            break;
        case historymenu::Entry::clearHistory:
            if (onClearHistory) onClearHistory(slot_);
            break;
        case historymenu::Entry::separator:
            break; // never enabled
    }
}

// The menu belongs to the row, not to the selection: it remembers the slot
// and the operation, so rows read again, or another slot selected, while it
// is open cannot turn the choice onto another row.
void HistoryPane::showMenu(int row, juce::Rectangle<int> screenArea)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;
    const std::int64_t op = rows_[static_cast<std::size_t>(row)].op;
    const int slot = slot_;
    juce::PopupMenu menu;
    const std::vector<MenuLine> lines = menuFor(op);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].entry == historymenu::Entry::separator)
            menu.addSeparator();
        else
            menu.addItem(itemId(i), lines[i].text, lines[i].enabled);
    }
    juce::Component::SafePointer<HistoryPane> safe(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(screenArea),
                       [safe, slot, op, lines](int id) {
                           if (safe == nullptr)
                               return;
                           for (std::size_t i = 0; i < lines.size(); ++i)
                               if (lines[i].entry != historymenu::Entry::separator && itemId(i) == id)
                                   safe->choose(slot, op, lines[i].entry, lines[i].track);
                       });
}

// Either button: the list has already selected the row by the time it says
// it was clicked (a right-click and a Ctrl-click select too), so the menu
// opens over a row that is visibly the one it is about.
void HistoryPane::listBoxItemClicked(int row, const juce::MouseEvent& event)
{
    showMenu(row, { event.getScreenX(), event.getScreenY(), 1, 1 });
}

// The keyboard's way to the same menu: Return on the selected row, opened
// against the row itself.
void HistoryPane::returnKeyPressed(int lastRowSelected)
{
    const juce::Rectangle<int> area = list_.getRowPosition(lastRowSelected, true);
    showMenu(lastRowSelected, list_.localAreaToGlobal(area));
}

void HistoryPane::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height,
                                   bool selected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(rows_.size()))
        return;
    const Row& row = rows_[static_cast<std::size_t>(rowNumber)];
    if (selected) {
        g.setColour(kSelected);
        g.fillRect(0, 0, width, height);
        g.setColour(felitronics::appkit::brand::violet);
        g.fillRect(0, 0, 2, height);
    }

    juce::Rectangle<int> area(kGutter, 0, width - 2 * kGutter, height);
    g.setFont(juce::FontOptions(13.0f));

    // The clock, then what happened, then — pushed right — what the slot
    // holds. The detail takes whatever is left in between, and a take's name
    // is long enough to need all of it.
    g.setColour(kInk);
    g.drawText(row.when, area.removeFromLeft(96), juce::Justification::centredLeft, false);
    if (row.audio.isNotEmpty())
        g.drawText(row.audio, area.removeFromRight(150), juce::Justification::centredRight, false);
    if (row.state.isNotEmpty()) {
        // What did not happen, in the window's warning colour and place: a
        // row that failed must not read like one that landed.
        g.setColour(kWarning);
        const int stateWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row.state) + 12;
        g.drawText(row.state, area.removeFromRight(juce::jmin(area.getWidth(), stateWidth)),
                   juce::Justification::centredRight, false);
    }

    g.setColour(kPaper);
    const int actionWidth =
        juce::jmin(area.getWidth(), juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(),
                                                                             row.action)
                                        + 16);
    g.drawText(row.action, area.removeFromLeft(actionWidth), juce::Justification::centredLeft,
               false);
    if (row.detail.isNotEmpty()) {
        g.setColour(kInk.brighter(0.35f));
        g.drawText(row.detail, area, juce::Justification::centredLeft, true);
    }
}

void HistoryPane::resized()
{
    list_.setBounds(getLocalBounds());
    empty_.setBounds(getLocalBounds());
}

} // namespace loopercat
