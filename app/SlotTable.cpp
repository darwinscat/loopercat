// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SlotTable.h"

#include <felitronics/appkit/Brand.h>

#include <loopercat/Wav.hpp>

namespace loopercat
{

namespace
{
    const juce::Colour kText { 0xffd8d8d8 };
    const juce::Colour kDim { 0xff63636d };
    const juce::Colour kRowEven { 0xff16161c };
    const juce::Colour kRowOdd { 0xff121218 };
    const juce::Colour kSelected { 0xff2a2440 }; // violet-tinted selection
}

SlotTable::SlotTable()
{
    table_.setHeaderHeight(26);
    table_.setRowHeight(24);
    table_.setColour(juce::ListBox::backgroundColourId, kRowOdd);
    table_.getViewport()->setScrollBarsShown(true, false);

    auto& header = table_.getHeader();
    header.setColour(juce::TableHeaderComponent::backgroundColourId, juce::Colour(0xff1c1c24));
    header.setColour(juce::TableHeaderComponent::textColourId, kText);
    header.setColour(juce::TableHeaderComponent::outlineColourId, juce::Colour(0xff2a2a34));
    using Flags = juce::TableHeaderComponent::ColumnPropertyFlags;
    header.addColumn("#", kSlot, 40, 40, 40, Flags::notSortable);
    header.addColumn("Name", kName, 220, 120, -1, Flags::notSortable);
    header.addColumn("Duration", kDuration, 84, 84, 84, Flags::notSortable);
    header.addColumn("Tempo", kTempo, 76, 76, 76, Flags::notSortable);
    header.addColumn("One Shot", kOneShot, 84, 84, 84, Flags::notSortable);
    header.addColumn("WAV file", kWavFile, 300, 120, -1, Flags::notSortable);
    header.setStretchToFitActive(true);

    addAndMakeVisible(table_);
}

void SlotTable::setRows(std::vector<SlotRow> rows)
{
    rows_ = std::move(rows);
    table_.updateContent();
    table_.repaint();
}

void SlotTable::resized()
{
    table_.setBounds(getLocalBounds());
}

juce::String SlotTable::formatDuration(long long frames)
{
    if (frames <= 0)
        return {};
    const long long totalSeconds = frames / wav::kSampleRate;
    const long long minutes = totalSeconds / 60;
    const long long seconds = totalSeconds % 60;
    return juce::String(minutes) + ":" + juce::String(seconds).paddedLeft('0', 2);
}

juce::String SlotTable::formatTempo(long long tenths)
{
    return juce::String(tenths / 10) + "." + juce::String(tenths % 10);
}

int SlotTable::getNumRows()
{
    return static_cast<int>(rows_.size());
}

void SlotTable::selectedRowsChanged(int lastRowSelected)
{
    if (onSlotSelected && lastRowSelected >= 0)
        onSlotSelected(lastRowSelected);
}

void SlotTable::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    if (onSlotActivated)
        onSlotActivated(row);
}

void SlotTable::cellClicked(int row, int, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onRowContextMenu)
        onRowContextMenu(row, e.getScreenPosition());
}

// --- wav drag-and-drop onto rows ---

int SlotTable::rowAt(int x, int y)
{
    const auto inTable = table_.getLocalPoint(this, juce::Point<int>(x, y));
    return table_.getRowContainingPosition(inTable.x, inTable.y);
}

bool SlotTable::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& file : files)
        if (file.endsWithIgnoreCase(".wav"))
            return true;
    return false;
}

void SlotTable::fileDragMove(const juce::StringArray&, int x, int y)
{
    const int row = rowAt(x, y);
    if (row != dragRow_) {
        dragRow_ = row;
        table_.repaint();
    }
}

void SlotTable::fileDragExit(const juce::StringArray&)
{
    dragRow_ = -1;
    table_.repaint();
}

void SlotTable::filesDropped(const juce::StringArray& files, int x, int y)
{
    const int row = rowAt(x, y);
    dragRow_ = -1;
    table_.repaint();
    if (row < 0 || !onWavDropped)
        return;
    for (const auto& file : files)
        if (file.endsWithIgnoreCase(".wav")) {
            onWavDropped(row, file);
            return; // one wav per slot; the first one wins
        }
}

void SlotTable::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? kSelected : (row % 2 == 0 ? kRowEven : kRowOdd));
    if (row == dragRow_) // a wav hovers here — show the push target
        g.fillAll(felitronics::appkit::brand::violet.withAlpha(0.18f));
}

void SlotTable::paintCell(juce::Graphics& g, int row, int columnId, int width, int height, bool)
{
    if (row < 0 || static_cast<std::size_t>(row) >= rows_.size())
        return;
    const SlotRow& r = rows_[static_cast<std::size_t>(row)];
    const bool loaded = r.info.hasAudio;

    juce::String text;
    switch (columnId) {
    case kSlot:     text = juce::String(r.info.slot); break;
    case kName:     text = juce::String(r.info.name).trimEnd(); break;
    case kDuration: text = loaded ? formatDuration(r.info.frames) : juce::String(); break;
    case kTempo:    text = loaded ? formatTempo(r.info.tempoTenths) : juce::String(); break;
    case kOneShot:  break; // drawn as a dot below
    case kWavFile:  text = juce::String(r.wavFile); break;
    default:        break;
    }

    const auto area = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);

    if (columnId == kOneShot) {
        if (r.info.oneShot) {
            const float d = 7.0f;
            g.setColour(felitronics::appkit::brand::orange);
            g.fillEllipse(static_cast<float>(area.getX()) + 2.0f,
                          (static_cast<float>(height) - d) * 0.5f, d, d);
        }
        return;
    }

    g.setColour(columnId == kSlot ? kDim : (loaded ? kText : kDim));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(text, area, juce::Justification::centredLeft, true);
}

} // namespace loopercat
