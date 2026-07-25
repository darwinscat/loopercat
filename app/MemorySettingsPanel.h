// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/MemorySettings.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::MemorySettingsPanel — the Memory Settings editor, Tier 1
// (issue #30): playback shaping and the onboard drums for one slot. Only
// gate-passing fields appear — booleans and manual-documented numeric ranges.
// Enum fields (Start/Stop modes, Pattern, Kit, Beat, Variation) stay out
// until their value maps are hardware-verified; the footer says so.
//
// Every committed change fires onEdit exactly once with a single-field edit
// set; the owner turns it into one worker job (the usual backup + pair-write
// + generation discipline), and the row pulses like any other mutation.
//==============================================================================
namespace loopercat
{

class MemorySettingsPanel final : public juce::Component
{
public:
    MemorySettingsPanel(int slot, const memsettings::Values& current);

    int slot() const { return slot_; }

    // (slot, single-field edits, banner description)
    std::function<void(int, memsettings::Edits, juce::String)> onEdit;

    void setValues(const memsettings::Values& values); // live snapshot refresh
    void setBusy(bool busy);                           // a worker job is running

    void resized() override;
    void paint(juce::Graphics&) override; // the stage colour — offscreen renders too

private:
    void styleSlider(juce::Slider& slider, juce::Label& label, const char* title,
                     long long max, long long snapValue);
    void commit(memsettings::Edits edits, const juce::String& what);

    const int slot_;
    memsettings::Values values_; // last state seen on the pedal — the change base

    juce::Label playbackTitle_, rhythmTitle_, gatedNote_;
    juce::ToggleButton reverse_ { "Reverse playback" };
    juce::Label playLevelLabel_, panLabel_, rhythmLevelLabel_;
    juce::Slider playLevel_, pan_, rhythmLevel_;
    juce::ToggleButton rhythmOn_ { "Rhythm (onboard drums)" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MemorySettingsPanel)
};

} // namespace loopercat
