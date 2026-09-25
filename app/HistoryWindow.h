// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

//==============================================================================
// loopercat::HistoryWindow — the whole card's timeline (#73): what happened at
// all, across every slot, oldest first with the newest row at the bottom.
//
// Like the slot's tab, the window knows nothing about the store. It is handed
// finished rows — sentences already written (app/history/SlotRows.h,
// forCard) and the flags saying what may be offered for each — and hands
// back the row a player acted on. Whoever opens it feeds it, and hears back
// through four callbacks: pin, export, restore, play.
//
// Every row wears the slots it touched as badges; clicking one filters the
// timeline to that slot, and "All slots" widens it again. A swap is one row
// with two badges. The buttons sit under the list, for the selected row —
// a row is a sentence, not a form — and offer only what the row can do:
// nothing plays a take the store no longer keeps.
//==============================================================================
namespace loopercat
{

class HistoryWindow final : public juce::Component, private juce::ListBoxModel
{
public:
    struct Row {
        juce::String when;      // "23 Sep 21:54"
        juce::String action;    // "Swapped slots 12 and 43"
        juce::String detail;    // "4:36 -> 0:39"
        juce::String state;     // "failed", "interrupted", "recorded on the pedal", or empty
        juce::String audio;     // "take kept", or per slot when there are several
        std::vector<int> slots; // the badges, ascending
        bool playable = false;
        bool restorable = false;
        bool pinned = false;
        std::int64_t op = 0;
    };

    HistoryWindow();
    ~HistoryWindow() override;

    // The whole timeline, oldest first. The selection follows the row a
    // player was looking at; otherwise the newest visible row is selected.
    void show(std::vector<Row> rows);
    void setBusy(bool busy); // a worker job is running: the offers wait

    // Only rows touching this slot, or every row.
    void setFilter(std::optional<int> slot);
    std::optional<int> filter() const { return filter_; }

    std::function<void(std::int64_t op, bool pinned)> onPin;
    std::function<void(std::int64_t op)> onExportTake;
    std::function<void(std::int64_t op)> onRestore;
    std::function<void(std::int64_t op)> onPlay;

    // The controls' actions, callable without a mouse: the buttons call these.
    void play();
    void restore();
    void exportTake();
    void togglePin();

    // The view, for whoever feeds it and for the tests.
    int visibleRows() const { return static_cast<int>(visible_.size()); }
    const Row* visibleRow(int index) const;
    void selectVisible(int index);
    const Row* selected() const;
    bool playEnabled() const { return play_.isEnabled(); }
    bool restoreEnabled() const { return restore_.isEnabled(); }
    bool exportEnabled() const { return export_.isEnabled(); }
    bool pinEnabled() const { return pin_.isEnabled(); }
    juce::String pinButtonText() const { return pin_.getButtonText(); }
    juce::String emptyText() const { return empty_.isVisible() ? empty_.getText() : juce::String(); }
    // The slot whose badge sits under x in a visible row, if any: what a
    // click there filters to.
    std::optional<int> badgeAt(int visibleIndex, int x) const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // --- ListBoxModel ---
    int getNumRows() override { return static_cast<int>(visible_.size()); }
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;

    void rebuildVisible(std::int64_t keepSelectedOp);
    void updateOffers();

    std::vector<Row> rows_;
    std::vector<std::size_t> visible_; // indices into rows_
    std::optional<int> filter_;
    bool busy_ = false;

    juce::ListBox list_ { "history-window", this };
    juce::Label empty_;
    juce::Label filterLabel_;
    juce::TextButton allSlots_ { "All slots" };
    juce::TextButton play_ { "Play" };
    juce::TextButton restore_ { "Restore this state" };
    juce::TextButton export_ { "Export take..." };
    juce::TextButton pin_ { "Pin" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HistoryWindow)
};

} // namespace loopercat
