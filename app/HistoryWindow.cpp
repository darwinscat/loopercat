// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "HistoryWindow.h"

#include <felitronics/appkit/Brand.h>
#include <loopercat/Error.hpp>

#include <algorithm>

namespace loopercat
{

namespace
{
constexpr int kRowHeight = 26;
constexpr int kBarHeight = 40;
constexpr int kFilterHeight = 30;
constexpr int kGutter = 12;
constexpr int kClockWidth = 110;
constexpr int kBadgeWidth = 30;
constexpr int kBadgeGap = 4;
constexpr int kBadgeSlots = 3; // room for this many badges before the sentence
constexpr int kAudioWidth = 190;
const juce::Colour kInk { 0xff8a8a99 };      // the quiet half of a row
const juce::Colour kPaper { 0xffe8e8f0 };    // what the row is about
const juce::Colour kSelected { 0xff26263a };
const juce::Colour kBadge { 0xff2a2a3a };
const juce::Colour kWarning { 0xffd9a441 };  // failed, interrupted

// Where the badges start and how wide each is: the hit test and the
// painting read the same numbers.
int badgesLeft() { return kGutter + kClockWidth; }
int badgeLeft(int index) { return badgesLeft() + index * (kBadgeWidth + kBadgeGap); }
} // namespace

HistoryWindow::HistoryWindow()
{
    list_.setRowHeight(kRowHeight);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list_.setOutlineThickness(0);
    addAndMakeVisible(list_);

    empty_.setJustificationType(juce::Justification::centred);
    empty_.setColour(juce::Label::textColourId, kInk);
    addChildComponent(empty_);

    filterLabel_.setColour(juce::Label::textColourId, kInk);
    filterLabel_.setFont(juce::FontOptions(12.0f));
    addAndMakeVisible(filterLabel_);
    allSlots_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff17171d));
    allSlots_.setColour(juce::TextButton::textColourOffId, kPaper);
    allSlots_.onClick = [this] { setFilter(std::nullopt); };
    addAndMakeVisible(allSlots_);

    for (juce::TextButton* button : { &play_, &restore_, &export_, &pin_ }) {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff17171d));
        button->setColour(juce::TextButton::textColourOffId, kPaper);
        addAndMakeVisible(*button);
    }
    play_.onClick = [this] { play(); };
    restore_.onClick = [this] { restore(); };
    export_.onClick = [this] { exportTake(); };
    pin_.onClick = [this] { togglePin(); };

    rebuildVisible(0);
    setSize(720, 420);
}

HistoryWindow::~HistoryWindow() { list_.setModel(nullptr); }

void HistoryWindow::show(std::vector<Row> rows)
{
    const std::int64_t keep = selected() != nullptr ? selected()->op : 0;
    rows_ = std::move(rows);
    rebuildVisible(keep);
}

void HistoryWindow::setFilter(std::optional<int> slot)
{
    if (slot && (*slot < 1 || *slot > 99))
        throw Error("a slot filter is 1..99");
    const std::int64_t keep = selected() != nullptr ? selected()->op : 0;
    filter_ = slot;
    rebuildVisible(keep);
}

void HistoryWindow::rebuildVisible(std::int64_t keepSelectedOp)
{
    visible_.clear();
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (!filter_ || std::find(row.slots.begin(), row.slots.end(), *filter_) != row.slots.end())
            visible_.push_back(i);
    }
    list_.updateContent();

    empty_.setText(rows_.empty() ? "Nothing has happened yet."
                   : filter_     ? "Nothing has happened to slot " + juce::String(*filter_) + " yet."
                                 : juce::String(),
                   juce::dontSendNotification);
    empty_.setVisible(visible_.empty());
    filterLabel_.setText(filter_ ? "Showing slot " + juce::String(*filter_) : "Showing all slots",
                         juce::dontSendNotification);
    allSlots_.setVisible(filter_.has_value());

    // Keep the row the player was looking at; otherwise show the newest,
    // which is the bottom — the timeline reads like a log, not a stack.
    int row = -1;
    for (std::size_t i = 0; i < visible_.size(); ++i)
        if (rows_[visible_[i]].op == keepSelectedOp)
            row = static_cast<int>(i);
    if (row < 0 && !visible_.empty())
        row = static_cast<int>(visible_.size()) - 1;
    list_.selectRow(row, juce::dontSendNotification);
    if (row >= 0)
        list_.scrollToEnsureRowIsOnscreen(row); // the selected row: the newest, unless one was kept
    updateOffers();
    repaint();
}

void HistoryWindow::setBusy(bool busy)
{
    busy_ = busy;
    updateOffers();
}

const HistoryWindow::Row* HistoryWindow::visibleRow(int index) const
{
    return index >= 0 && index < static_cast<int>(visible_.size())
        ? &rows_[visible_[static_cast<std::size_t>(index)]]
        : nullptr;
}

void HistoryWindow::selectVisible(int index)
{
    list_.selectRow(index, juce::dontSendNotification);
    updateOffers();
}

const HistoryWindow::Row* HistoryWindow::selected() const
{
    return visibleRow(list_.getSelectedRow());
}

void HistoryWindow::updateOffers()
{
    const Row* row = selected();
    play_.setEnabled(!busy_ && row != nullptr && row->playable);
    export_.setEnabled(!busy_ && row != nullptr && row->playable);
    restore_.setEnabled(!busy_ && row != nullptr && row->restorable);
    pin_.setEnabled(!busy_ && row != nullptr);
    pin_.setButtonText(row != nullptr && row->pinned ? "Unpin" : "Pin");
}

void HistoryWindow::play()
{
    if (const Row* row = selected(); row != nullptr && row->playable && !busy_ && onPlay)
        onPlay(row->op);
}

void HistoryWindow::restore()
{
    if (const Row* row = selected(); row != nullptr && row->restorable && !busy_ && onRestore)
        onRestore(row->op);
}

void HistoryWindow::exportTake()
{
    if (const Row* row = selected(); row != nullptr && row->playable && !busy_ && onExportTake)
        onExportTake(row->op);
}

void HistoryWindow::togglePin()
{
    const int index = list_.getSelectedRow();
    if (index < 0 || index >= static_cast<int>(visible_.size()) || busy_)
        return;
    Row& row = rows_[visible_[static_cast<std::size_t>(index)]];
    row.pinned = !row.pinned;
    updateOffers();
    list_.repaintRow(index);
    if (onPin)
        onPin(row.op, row.pinned);
}

std::optional<int> HistoryWindow::badgeAt(int visibleIndex, int x) const
{
    const Row* row = visibleRow(visibleIndex);
    if (row == nullptr)
        return std::nullopt;
    for (std::size_t i = 0; i < row->slots.size(); ++i) {
        const int left = badgeLeft(static_cast<int>(i));
        if (x >= left && x < left + kBadgeWidth)
            return row->slots[i];
    }
    return std::nullopt;
}

void HistoryWindow::selectedRowsChanged(int)
{
    updateOffers();
}

void HistoryWindow::listBoxItemClicked(int row, const juce::MouseEvent& event)
{
    if (const auto slot = badgeAt(row, event.x))
        setFilter(slot);
}

void HistoryWindow::listBoxItemDoubleClicked(int index, const juce::MouseEvent& event)
{
    if (badgeAt(index, event.x))
        return; // the click already filtered; a second click is not a play
    const Row* row = visibleRow(index);
    if (row != nullptr && row->playable && !busy_ && onPlay)
        onPlay(row->op);
}

void HistoryWindow::paintListBoxItem(int index, juce::Graphics& g, int width, int height,
                                     bool isSelected)
{
    const Row* row = visibleRow(index);
    if (row == nullptr)
        return;
    if (isSelected) {
        g.setColour(kSelected);
        g.fillRect(0, 0, width, height);
        g.setColour(felitronics::appkit::brand::violet);
        g.fillRect(0, 0, 2, height);
    }

    juce::Rectangle<int> area(kGutter, 0, width - 2 * kGutter, height);
    g.setFont(juce::FontOptions(13.0f));

    // The clock, the slot badges, then what happened; what the slots hold
    // is pushed right. The detail takes whatever is left in between.
    g.setColour(kInk);
    g.drawText(row->when, area.removeFromLeft(kClockWidth), juce::Justification::centredLeft, false);

    for (std::size_t i = 0; i < row->slots.size(); ++i) {
        const juce::Rectangle<int> badge(badgeLeft(static_cast<int>(i)), 4, kBadgeWidth, height - 8);
        g.setColour(filter_ && *filter_ == row->slots[i] ? felitronics::appkit::brand::violet
                                                          : kBadge);
        g.fillRoundedRectangle(badge.toFloat(), 4.0f);
        g.setColour(kPaper);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::String(row->slots[i]), badge, juce::Justification::centred, false);
    }
    area.removeFromLeft(kBadgeSlots * (kBadgeWidth + kBadgeGap));
    g.setFont(juce::FontOptions(13.0f));

    if (row->audio.isNotEmpty()) {
        g.setColour(kInk);
        g.drawText(row->audio, area.removeFromRight(kAudioWidth), juce::Justification::centredRight,
                   true);
    }
    if (row->state.isNotEmpty()) {
        g.setColour(kWarning);
        const int stateWidth = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), row->state) + 12;
        g.drawText(row->state, area.removeFromRight(juce::jmin(area.getWidth(), stateWidth)),
                   juce::Justification::centredRight, false);
    }

    g.setColour(row->pinned ? felitronics::appkit::brand::violet : kPaper);
    juce::String action = row->action;
    if (row->pinned)
        action = juce::String::fromUTF8("\xe2\x97\x86 ") + action; // a small diamond marks a pin
    const int actionWidth =
        juce::jmin(area.getWidth(), juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), action) + 16);
    g.drawText(action, area.removeFromLeft(actionWidth), juce::Justification::centredLeft, false);
    if (row->detail.isNotEmpty()) {
        g.setColour(kInk.brighter(0.35f));
        g.drawText(row->detail, area, juce::Justification::centredLeft, true);
    }
}

void HistoryWindow::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff121218));
    g.setColour(juce::Colour(0xff1f1f28));
    g.fillRect(getLocalBounds().removeFromBottom(kBarHeight).removeFromTop(1));
    g.fillRect(getLocalBounds().removeFromTop(kFilterHeight).removeFromBottom(1));
}

void HistoryWindow::resized()
{
    juce::Rectangle<int> area = getLocalBounds();
    juce::Rectangle<int> filter = area.removeFromTop(kFilterHeight).reduced(kGutter, 4);
    allSlots_.setBounds(filter.removeFromRight(90));
    filter.removeFromRight(8);
    filterLabel_.setBounds(filter);
    juce::Rectangle<int> bar = area.removeFromBottom(kBarHeight).reduced(kGutter, 6);
    restore_.setBounds(bar.removeFromRight(150));
    bar.removeFromRight(8);
    export_.setBounds(bar.removeFromRight(120));
    bar.removeFromRight(8);
    play_.setBounds(bar.removeFromRight(90));
    pin_.setBounds(bar.removeFromLeft(90));
    list_.setBounds(area);
    empty_.setBounds(area);
}

} // namespace loopercat
