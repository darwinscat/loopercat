// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PedalWorker.h"
#include "UseCaseCard.h"

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::SlotInspector — everything about ONE slot, next to the list of
// all of them: the table is the stage a player reads between songs, this is
// the studio where a memory gets set up.
//
// What it shows is deliberately narrow. Identity first (the name and the
// tempo the pedal will play at, with the bar count that follows from it),
// then one card per thing a player asks for — each card the pedal's own name
// for it, one switch, and no field we have not verified on hardware.
//
// It owns no memory bytes: the callbacks hand slot numbers and values to the
// owner, which turns each into one worker job like every other mutation.
//==============================================================================
namespace loopercat
{

class SlotInspector final : public juce::Component
{
public:
    SlotInspector();

    // The current selection, or nullptr when nothing is selected. Safe to
    // call on every snapshot: a field the user is typing in is left alone.
    void setSlot(const SlotRow* row);
    void setBusy(bool busy); // a worker job is running — the studio waits

    std::function<void(int, juce::String)> onRenameCommitted;
    std::function<void(int, long long)> onTempoCommitted;
    std::function<void(int)> onOneShotToggled;
    std::function<void(int)> onCountInToggled;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refresh();          // paint the current info into the controls
    void updateBarsHint();   // the consequence line under the tempo field
    void commitName();
    void commitTempo();
    void makeCaption(juce::Label& label, const juce::String& text);

    bool hasSlot_ = false;
    catalog::SlotInfo info_ {};
    bool busy_ = false;

    juce::Label nameCaption_, tempoCaption_, barsHint_, footer_;
    juce::TextEditor nameEditor_, tempoEditor_;
    UseCaseCard countIn_ { "Play Count-In" };
    UseCaseCard oneShot_ { "One Shot" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotInspector)
};

} // namespace loopercat
