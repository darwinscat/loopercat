// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalWorker.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::SlotTable — the slot browser: one row per memory slot (number,
// name, duration, bars, tempo, One Shot, Count-In, on-pedal wav file). Rows with
// audio carry the full text colour; empty slots stay dim so a loaded pedal
// reads at a glance.
//==============================================================================
namespace loopercat
{

class SlotTable final : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        public juce::DragAndDropContainer, // row drags stay inside the table
                        public juce::DragAndDropTarget,    // public: found by dynamic_cast, like FileDragAndDropTarget
                        private juce::TableListBoxModel,
                        private juce::Timer
{
public:
    SlotTable();

    void setRows(std::vector<SlotRow> rows);

    // Callbacks carry SLOT NUMBERS (1..99), not row indexes — the visible
    // rows may be a filtered subset (see the show-empty-slots toggle).
    std::function<void(int)> onSlotSelected;                        // selection moved
    std::function<void(int)> onSlotActivated;                       // double-click: select AND play
    std::function<void(int, juce::Point<int>)> onSlotContextMenu;   // right-click (screen position)
    std::function<void(int, juce::String)> onAudioDropped;          // an importable audio file landed on a slot's row
    std::function<void(int, int)> onSwapRequested;                  // row dragged onto a row: (from, to) trade places
    std::function<void(int, juce::String)> onRenameCommitted;       // inline edit finished with a new name
    std::function<void(int, long long)> onTempoCommitted;           // inline edit finished with new tenths of BPM
    std::function<void(int)> onOneShotToggled;                      // click on the One Shot cell
    std::function<void(int)> onCountInToggled;                      // click on the Count-In cell
    std::function<void(int)> onEmptyWavCellClicked;                 // click the "drop audio here" hint

    void selectSlot(int slot);

    // The two behaviour columns are a preference (Settings -> Columns): the
    // pedal's own facts always show, these are the ones a player opts into.
    void setOptionalColumns(bool oneShot, bool countIn);

    // Inline edits live on double-click (name, tempo): an editor right in the
    // cell. Enter commits, Esc cancels; the field enforces the pedal's
    // constraints — 12 printable ASCII for names, digits and a dot for tempo.

    // The slot a worker job is touching right now (0 = none): its row pulses.
    void setBusySlot(int slot);

    static bool isImportableAudio(const juce::String& path); // the one import gate: drag + drop share it
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
    enum Columns { kSlot = 1, kName, kDuration, kBars, kTempo, kOneShot, kCountIn, kWavFile };

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int columnId, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void cellDoubleClicked(int row, int columnId, const juce::MouseEvent&) override;
    void cellClicked(int row, int columnId, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override;

    // Internal row drag (the swap gesture). File drags use the interface above.
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    void updateSwapTarget();

    int rowAt(int x, int y);
    int rowOfSlot(int slot) const;
    int slotOfRow(int rowIndex) const; // 0 when out of range
    void startCellEdit(int rowIndex, int columnId);
    void finishCellEdit(bool commit);
    void timerCallback() override;
    void updateTimerState(); // one timer serves the busy pulse and drag autoscroll

    std::vector<SlotRow> rows_;
    int dragRow_ = -1;  // row highlighted under a drag (wav file or row swap)
    bool rowDragActive_ = false;         // an internal row drag is over the table
    int rowDragSourceSlot_ = 0;          // the slot being dragged (its own row never highlights)
    juce::Point<int> rowDragPosition_;   // its last position, for edge autoscroll
    int busySlot_ = 0;  // slot being mutated; its row pulses
    float busyPhase_ = 0.0f;
    int editingRow_ = -1;
    int editingColumn_ = 0;
    juce::String editOriginal_;
    std::unique_ptr<juce::TextEditor> cellEditor_;
    juce::TableListBox table_ { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTable)
};

} // namespace loopercat
