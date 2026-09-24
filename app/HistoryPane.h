// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <vector>

//==============================================================================
// loopercat::HistoryPane — one slot's history on the History tab (#50): what
// happened to this slot, oldest first, with the newest row at the bottom
// where the hand already is.
//
// The pane knows nothing about the store. It is handed finished rows —
// sentences already written (app/history/SlotStory.h) and two flags saying
// what may be offered for each — and hands back the row a player acted on.
//
// The buttons live under the list rather than inside every row: a row is a
// sentence, and a sentence with two buttons in it is a form. One selected
// row, one pair of buttons, and the name of a take keeps the width it needs.
//==============================================================================
namespace loopercat
{

class HistoryPane final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Row {
        juce::String when;   // "21:54", or "23 Sep 21:54" once it is not today
        juce::String action; // "Trimmed"
        juce::String detail; // "4:36 -> 0:39"
        juce::String audio;  // "take kept"
        bool playable = false;   // its take's bytes are in the store
        bool restorable = false; // its state can go back onto the card
        std::int64_t op = 0;
    };

    HistoryPane();

    // The rows of the selected slot, oldest first. The view keeps the bottom
    // in sight: the newest row is the one a player is looking for.
    void setRows(std::vector<Row> rows, int slot);
    void setBusy(bool busy); // a worker job is running: the offers wait
    void clear();

    std::function<void(std::int64_t)> onPlay;
    std::function<void(std::int64_t)> onRestore;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // --- ListBoxModel ---
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    const Row* selected() const;
    void updateOffers();

    std::vector<Row> rows_;
    int slot_ = 0;
    bool busy_ = false;

    juce::ListBox list_ { "history", this };
    juce::Label empty_;
    juce::TextButton play_ { "Play" };
    juce::TextButton restore_ { "Restore this state" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryPane)
};

} // namespace loopercat
