// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SlotTable.h"

#include "Strings.h"

#include <felitronics/appkit/Brand.h>

#include <loopercat/Wav.hpp>

#include <cmath>

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
    header.addColumn("Name", kName, 132, 132, 132, Flags::notSortable); // 12 chars, fixed
    header.addColumn("Duration", kDuration, 84, 84, 84, Flags::notSortable);
    header.addColumn("Bars", kBars, 56, 56, 56, Flags::notSortable);
    header.addColumn("Tempo", kTempo, 76, 76, 76, Flags::notSortable);
    header.addColumn("One Shot", kOneShot, 84, 84, 84, Flags::notSortable);
    header.addColumn("WAV file", kWavFile, 300, 120, -1, Flags::notSortable);
    header.setStretchToFitActive(true);

    addAndMakeVisible(table_);
}

void SlotTable::setRows(std::vector<SlotRow> rows)
{
    finishCellEdit(false); // rows may shift under the editor — never edit stale data
    rows_ = std::move(rows);
    table_.updateContent();
    table_.repaint();
}

// --- inline cell edits (name, tempo) ---

int SlotTable::rowOfSlot(int slot) const
{
    for (std::size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].info.slot == slot)
            return static_cast<int>(i);
    return -1;
}

int SlotTable::slotOfRow(int rowIndex) const
{
    return rowIndex >= 0 && static_cast<std::size_t>(rowIndex) < rows_.size()
             ? rows_[static_cast<std::size_t>(rowIndex)].info.slot
             : 0;
}

void SlotTable::selectSlot(int slot)
{
    const int row = rowOfSlot(slot);
    if (row >= 0)
        table_.selectRow(row);
}

void SlotTable::startRenameEditForSlot(int slot)
{
    const int row = rowOfSlot(slot);
    if (row >= 0)
        startCellEdit(row, kName);
}

void SlotTable::startTempoEditForSlot(int slot)
{
    const int row = rowOfSlot(slot);
    if (row >= 0)
        startCellEdit(row, kTempo);
}

void SlotTable::startCellEdit(int rowIndex, int columnId)
{
    if (rowIndex < 0 || static_cast<std::size_t>(rowIndex) >= rows_.size())
        return;
    if (columnId == kName ? !onRenameCommitted : !onTempoCommitted)
        return;
    finishCellEdit(false);

    const auto cell = table_.getCellPosition(columnId, rowIndex, true);
    if (cell.isEmpty())
        return;
    table_.scrollToEnsureRowIsOnscreen(rowIndex);

    const SlotRow& r = rows_[static_cast<std::size_t>(rowIndex)];
    editingRow_ = rowIndex;
    editingColumn_ = columnId;
    editOriginal_ = columnId == kName ? utf8(r.info.name).trimEnd()
                                      : formatTempo(r.info.tempoTenths);

    cellEditor_ = std::make_unique<juce::TextEditor>();
    if (columnId == kName) {
        // The pedal's name constraints, enforced at the field: 12 chars, the
        // printable ASCII range its display can show.
        juce::String printableAscii;
        for (juce::juce_wchar c = 0x20; c <= 0x7e; ++c)
            printableAscii += juce::String::charToString(c);
        cellEditor_->setInputRestrictions(rc0::kNameLength, printableAscii);
    } else {
        cellEditor_->setInputRestrictions(5, "0123456789."); // "300.0" is the widest legal value
    }
    cellEditor_->setFont(juce::FontOptions(13.0f));
    cellEditor_->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff23232d));
    cellEditor_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    cellEditor_->setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2a2a34));
    cellEditor_->setColour(juce::TextEditor::focusedOutlineColourId,
                           felitronics::appkit::brand::violet);
    cellEditor_->setText(editOriginal_, juce::dontSendNotification);
    cellEditor_->setSelectAllWhenFocused(true);
    cellEditor_->onReturnKey = [this] { finishCellEdit(true); };
    cellEditor_->onEscapeKey = [this] { finishCellEdit(false); };
    cellEditor_->onFocusLost = [this] { finishCellEdit(true); }; // Finder-style: click away commits

    addAndMakeVisible(*cellEditor_);
    cellEditor_->setBounds(cell.reduced(2, 1)); // table_ sits at (0,0), same coords
    cellEditor_->grabKeyboardFocus();
}

void SlotTable::finishCellEdit(bool commit)
{
    if (cellEditor_ == nullptr)
        return;
    // Move out first: removing the editor fires onFocusLost, which re-enters here.
    const std::unique_ptr<juce::TextEditor> editor = std::move(cellEditor_);
    const int row = editingRow_;
    const int column = editingColumn_;
    editingRow_ = -1;
    editingColumn_ = 0;
    const juce::String value = editor->getText().trim();
    if (!commit || value.isEmpty() || value == editOriginal_ || slotOfRow(row) <= 0)
        return;
    if (column == kName && onRenameCommitted) {
        onRenameCommitted(slotOfRow(row), value);
    } else if (column == kTempo && onTempoCommitted) {
        // Parse to tenths; range enforcement is the core's job — its typed
        // error lands on the banner with the pedal's 40.0-300.0 wording.
        const long long tenths = std::llround(value.getDoubleValue() * 10.0);
        if (tenths > 0)
            onTempoCommitted(slotOfRow(row), tenths);
    }
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
    if (onSlotSelected && slotOfRow(lastRowSelected) > 0)
        onSlotSelected(slotOfRow(lastRowSelected));
}

void SlotTable::cellDoubleClicked(int row, int columnId, const juce::MouseEvent&)
{
    if (columnId == kName || columnId == kTempo) { // double-click = edit in place
        startCellEdit(row, columnId);
        return;
    }
    if (onSlotActivated && slotOfRow(row) > 0)
        onSlotActivated(slotOfRow(row));
}

void SlotTable::cellClicked(int row, int columnId, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onSlotContextMenu) {
        if (slotOfRow(row) > 0)
            onSlotContextMenu(slotOfRow(row), e.getScreenPosition());
        return;
    }
    // The One Shot cell IS the toggle — click flips it (reference web UI).
    if (columnId == kOneShot && onOneShotToggled && slotOfRow(row) > 0) {
        onOneShotToggled(slotOfRow(row));
        return;
    }
    // The empty-slot hint is a button: click opens the WAV chooser.
    if (columnId == kWavFile && onEmptyWavCellClicked && slotOfRow(row) > 0) {
        const SlotRow& r = rows_[static_cast<std::size_t>(row)];
        if (!r.info.hasAudio && r.wavFile.empty())
            onEmptyWavCellClicked(r.info.slot);
    }
}

// --- per-row busy indication ---

void SlotTable::setBusySlot(int slot)
{
    if (busySlot_ == slot)
        return;
    busySlot_ = slot;
    busyPhase_ = 0.0f;
    if (slot > 0)
        startTimerHz(30);
    else
        stopTimer();
    table_.repaint();
}

void SlotTable::timerCallback()
{
    busyPhase_ += 0.12f;
    if (busySlot_ > 0 && rowOfSlot(busySlot_) >= 0)
        table_.repaintRow(rowOfSlot(busySlot_));
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
    if (slotOfRow(row) <= 0 || !onWavDropped)
        return;
    for (const auto& file : files)
        if (file.endsWithIgnoreCase(".wav")) {
            onWavDropped(slotOfRow(row), file);
            return; // one wav per slot; the first one wins
        }
}

void SlotTable::paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected)
{
    g.fillAll(selected ? kSelected : (row % 2 == 0 ? kRowEven : kRowOdd));
    if (row == dragRow_) // a wav hovers here — show the push target
        g.fillAll(felitronics::appkit::brand::violet.withAlpha(0.18f));
    if (busySlot_ > 0 && slotOfRow(row) == busySlot_) {
        // The working row: a breathing violet wash + a left edge bar.
        const float pulse = 0.5f + 0.5f * std::sin(busyPhase_);
        g.setColour(felitronics::appkit::brand::violet.withAlpha(0.10f + 0.14f * pulse));
        g.fillRect(0, 0, width, height);
        g.setColour(felitronics::appkit::brand::orange.withAlpha(0.5f + 0.5f * pulse));
        g.fillRect(0, 0, 3, height);
    }
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
    case kName:     text = utf8(r.info.name).trimEnd(); break;
    case kDuration: text = loaded ? formatDuration(r.info.frames) : juce::String(); break;
    case kBars:     text = loaded && r.info.measures > 0 ? juce::String(r.info.measures)
                                                         : juce::String(); break;
    case kTempo:    text = loaded ? formatTempo(r.info.tempoTenths) : juce::String(); break;
    case kOneShot:  break; // drawn as a dot below
    case kWavFile:
        text = r.wavFile.empty() && !loaded
                 ? juce::String::fromUTF8("\xe2\x80\x94 drop a WAV here, or click to choose")
                 : utf8(r.wavFile);
        break;
    default:        break;
    }

    const auto area = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);

    if (columnId == kOneShot) {
        // The cell is the toggle: filled = on, hollow = off (click flips it).
        const float d = 7.0f;
        const float x = static_cast<float>(area.getX()) + 2.0f;
        const float y = (static_cast<float>(height) - d) * 0.5f;
        if (r.info.oneShot) {
            g.setColour(felitronics::appkit::brand::orange);
            g.fillEllipse(x, y, d, d);
        } else {
            g.setColour(kDim.withAlpha(0.55f));
            g.drawEllipse(x, y, d, d, 1.2f);
        }
        return;
    }

    const bool isPickHint = columnId == kWavFile && !loaded && r.wavFile.empty();
    g.setColour(isPickHint ? felitronics::appkit::brand::lilac.withAlpha(0.45f)
                           : columnId == kSlot ? kDim
                                               : (loaded ? kText : kDim));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(text, area, juce::Justification::centredLeft, true);
}

} // namespace loopercat
