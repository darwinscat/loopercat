// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryRowMenu.h"

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
// No buttons: a row is a sentence, and a click on it — either mouse button,
// or Return on the selected one — selects it and asks what can be done with
// it. The menu's items and their rules are historymenu::forRow; the pane
// only draws them and passes the choice on.
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
        juce::String state;  // "failed", "interrupted", "recorded on the pedal", or empty
        juce::String audio;  // "take kept"
        bool takeKept = false;   // its take's bytes are in the store: it can be exported
        bool restorable = false; // its state can go back onto the card
        std::int64_t op = 0;
    };

    HistoryPane();

    // The rows of the selected slot, oldest first. The view keeps the bottom
    // in sight: the newest row is the one a player is looking for.
    void setRows(std::vector<Row> rows, int slot);
    void setBusy(bool busy); // a worker job is running: the menu waits
    void clear();

    std::function<void(std::int64_t)> onRestore;
    std::function<void(std::int64_t)> onExportTake;
    std::function<void(int)> onClearHistory; // the slot, not the row

    // The menu the row of operation `op` offers, as it is drawn: what each
    // line says and whether it can be chosen. Empty when no row is that
    // operation. Reachable without a window, so the words can be tested.
    struct MenuLine {
        historymenu::Entry entry;
        juce::String text;
        bool enabled;
    };
    std::vector<MenuLine> menuFor(std::int64_t op) const;

    // A line of that menu, chosen. The rule is asked again at the moment of
    // the choice: the menu may have been open while the pedal became busy or
    // the rows were read again, and a choice made on a stale menu does nothing.
    void choose(std::int64_t op, historymenu::Entry entry);

    void resized() override;

private:
    // --- ListBoxModel ---
    int getNumRows() override { return static_cast<int>(rows_.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void returnKeyPressed(int lastRowSelected) override;

    const Row* selected() const;
    const Row* rowFor(std::int64_t op) const;
    void showMenu(int row, juce::Rectangle<int> screenArea);

    std::vector<Row> rows_;
    int slot_ = 0;
    bool busy_ = false;

    juce::ListBox list_ { "history", this };
    juce::Label empty_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryPane)
};

} // namespace loopercat
