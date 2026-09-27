// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "UseCaseCard.h"

#include <loopercat/usecases/PlayStop.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::PlayStopCard — how the loop starts and stops, on the Properties
// strip. The pedal's own three words for it (START, STOP, FADE TIME — the
// LOOP screen, reference manual p. 9), each as one choice in a row: three
// short lists fit a card; a groove did not (RhythmPane.h).
//
// Like every card it owns no memory bytes: it takes the slot's values and
// reports an Edits (PlayStop.hpp) with the one field the player changed.
// Every choice carries its field's name as its component id (startMode,
// stopMode, fadeTime).
//==============================================================================
namespace loopercat
{

class PlayStopCard final : public juce::Component
{
public:
    PlayStopCard();

    std::function<void(usecases::playstop::Edits)> onEdit;

    void setValues(const usecases::playstop::Values& values);

    // Two rows of controls: the pedal's own words are long ("IMMEDIATE",
    // "LOOP END"), and a third of the strip cannot hold three of them side
    // by side without cutting one short.
    int preferredHeight() const { return 96; }

    void enablementChanged() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refresh();
    void makeCaption(juce::Label& label, const juce::String& text);
    void makeChoice(juce::ComboBox& box, const char* id,
                    std::function<long long()> current,
                    std::function<usecases::playstop::Edits(long long)> makeEdit);

    usecases::playstop::Values values_ {};

    juce::Label startCaption_, stopCaption_, fadeCaption_;
    juce::ComboBox start_, stop_, fade_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayStopCard)
};

} // namespace loopercat
