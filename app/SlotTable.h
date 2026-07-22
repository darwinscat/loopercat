// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalWorker.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::SlotTable — the read-only slot browser: one row per memory slot
// (number, name, duration, tempo, One Shot, on-pedal wav file). Rows with
// audio carry the full text colour; empty slots stay dim so a loaded pedal
// reads at a glance.
//==============================================================================
namespace loopercat
{

class SlotTable final : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::TableListBoxModel
{
public:
    SlotTable();

    void setRows(std::vector<SlotRow> rows);

    // Row index (0-based), fired on the message thread.
    std::function<void(int)> onSlotSelected;                        // selection moved
    std::function<void(int)> onSlotActivated;                       // double-click: select AND play
    std::function<void(int, juce::Point<int>)> onRowContextMenu;    // right-click (screen position)
    std::function<void(int, juce::String)> onWavDropped;            // a .wav landed on a row
    std::function<void(int, juce::String)> onRenameCommitted;       // inline edit finished with a new name

    void selectRow(int rowIndex) { table_.selectRow(rowIndex); }

    // Inline rename: an editor right in the Name cell (double-click on the
    // name does this too). Enter commits, Esc cancels, 12 printable ASCII
    // enforced at the field.
    void startRenameEdit(int rowIndex);

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    void resized() override;

    // Display formatting, kept as pure functions (and reused by tests-to-be
    // once display rules grow): frames at 44.1 kHz -> "m:ss", tenths -> "98.7".
    static juce::String formatDuration(long long frames);
    static juce::String formatTempo(long long tenths);

private:
    enum Columns { kSlot = 1, kName, kDuration, kTempo, kOneShot, kWavFile };

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int columnId, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void cellDoubleClicked(int row, int columnId, const juce::MouseEvent&) override;
    void cellClicked(int row, int columnId, const juce::MouseEvent&) override;

    int rowAt(int x, int y);
    void finishRenameEdit(bool commit);

    std::vector<SlotRow> rows_;
    int dragRow_ = -1; // row highlighted under a wav drag
    int editingRow_ = -1;
    juce::String editOriginal_;
    std::unique_ptr<juce::TextEditor> nameEditor_;
    juce::TableListBox table_ { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTable)
};

} // namespace loopercat
