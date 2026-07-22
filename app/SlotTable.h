// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalMonitor.h"

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
                        private juce::TableListBoxModel
{
public:
    SlotTable();

    void setRows(std::vector<SlotRow> rows);

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

    std::vector<SlotRow> rows_;
    juce::TableListBox table_ { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTable)
};

} // namespace loopercat
