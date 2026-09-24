// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryPane.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace
{
constexpr int kRowHeight = 26;
constexpr int kBarHeight = 40;
constexpr int kGutter = 12;
const juce::Colour kInk { 0xff8a8a99 };      // the quiet half of a row
const juce::Colour kPaper { 0xffe8e8f0 };    // what the row is about
const juce::Colour kSelected { 0xff26263a };
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

    for (juce::TextButton* button : { &play_, &restore_ }) {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff17171d));
        button->setColour(juce::TextButton::textColourOffId, kPaper);
        addAndMakeVisible(*button);
    }
    play_.onClick = [this] {
        if (const Row* row = selected(); row != nullptr && onPlay)
            onPlay(row->op);
    };
    restore_.onClick = [this] {
        if (const Row* row = selected(); row != nullptr && onRestore)
            onRestore(row->op);
    };
    updateOffers();
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
    updateOffers();
    repaint();
}

void HistoryPane::clear()
{
    setRows({}, 0);
}

void HistoryPane::setBusy(bool busy)
{
    busy_ = busy;
    updateOffers();
}

const HistoryPane::Row* HistoryPane::selected() const
{
    const int row = list_.getSelectedRow();
    return row >= 0 && row < static_cast<int>(rows_.size()) ? &rows_[static_cast<std::size_t>(row)]
                                                            : nullptr;
}

void HistoryPane::updateOffers()
{
    const Row* row = selected();
    play_.setEnabled(!busy_ && row != nullptr && row->playable);
    restore_.setEnabled(!busy_ && row != nullptr && row->restorable);
}

void HistoryPane::selectedRowsChanged(int)
{
    updateOffers();
}

void HistoryPane::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(rows_.size()) && rows_[static_cast<std::size_t>(row)].playable
        && !busy_ && onPlay)
        onPlay(rows_[static_cast<std::size_t>(row)].op);
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

void HistoryPane::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff1f1f28));
    g.fillRect(getLocalBounds().removeFromBottom(kBarHeight).removeFromTop(1));
}

void HistoryPane::resized()
{
    juce::Rectangle<int> area = getLocalBounds();
    juce::Rectangle<int> bar = area.removeFromBottom(kBarHeight).reduced(kGutter, 6);
    restore_.setBounds(bar.removeFromRight(150));
    bar.removeFromRight(8);
    play_.setBounds(bar.removeFromRight(90));
    list_.setBounds(area);
    empty_.setBounds(area);
}

} // namespace loopercat
