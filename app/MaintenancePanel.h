// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "MaintenanceOffer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

//==============================================================================
// loopercat::MaintenancePanel — Settings -> Maintenance: the pedal's card
// sweep, where the Maintenance menu used to be (0.9.6).
//
// Like the storage panel beside it, this one shows and asks. The owner hands
// it an offer (maintenance::offer) whenever the pedal changes, and hears back
// one press; the panel reads no snapshot and sweeps nothing itself. Until the
// first offer arrives nothing is on offer: a button that is live before
// anyone has said a pedal is there would be a promise the card cannot keep.
//
// A tab has room a menu item did not, so the sweep says what it removes. It
// is the same list the core walks (Volume.hpp findJunk): the sidecars a host
// computer leaves at the card's root and anywhere under the pedal's own tree.
//==============================================================================
namespace loopercat
{

class MaintenancePanel final : public juce::Component
{
public:
    static constexpr const char* kCleanJunk = "Clean junk from the pedal";

    MaintenancePanel()
    {
        title_.setText("Housekeeping on the card", juce::dontSendNotification);
        title_.setFont(juce::FontOptions(12.0f));
        title_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        addAndMakeVisible(title_);

        clean_.setButtonText(kCleanJunk);
        clean_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1e1e26));
        clean_.setEnabled(false); // nothing is known about the pedal yet
        clean_.onClick = [this] { cleanJunk(); };
        addAndMakeVisible(clean_);

        why_.setFont(juce::FontOptions(11.0f));
        why_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));
        addAndMakeVisible(why_);

        hint_.setText("Removes the sidecar files a computer leaves on the card "
                      "(._ files, .DS_Store, Thumbs.db, desktop.ini) from the card's root "
                      "and from the pedal's own folders, where they can stop it booting. "
                      "Your loops and settings are not touched.",
                      juce::dontSendNotification);
        hint_.setFont(juce::FontOptions(11.0f));
        hint_.setColour(juce::Label::textColourId, juce::Colour(0xff6f6f78));
        hint_.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(hint_);
    }

    // The owner's word on the pedal in front of it.
    void show(const maintenance::Offer& offer)
    {
        ++shown_;
        clean_.setEnabled(offer.enabled);
        why_.setText(juce::String::fromUTF8(offer.why.c_str()), juce::dontSendNotification);
    }

    std::function<void()> onCleanJunk;

    // The button's action, callable without a mouse: the button calls this.
    // The enablement is asked again here, so the panel can never ask for a
    // sweep it was told to refuse -- whoever reaches the action.
    void cleanJunk()
    {
        if (clean_.isEnabled() && onCleanJunk)
            onCleanJunk();
    }

    // What the panel says and offers, for the owner and the tests.
    juce::String cleanButtonText() const { return clean_.getButtonText(); }
    bool cleanEnabled() const { return clean_.isEnabled(); }
    juce::String whyLine() const { return why_.getText(); }
    int offersShown() const { return shown_; }

    void resized() override
    {
        auto area = getLocalBounds().reduced(12, 10);
        title_.setBounds(area.removeFromTop(20));
        area.removeFromTop(8);
        clean_.setBounds(area.removeFromTop(26).removeFromLeft(kButtonWidth));
        area.removeFromTop(6);
        why_.setBounds(area.removeFromTop(18));
        area.removeFromTop(12);
        hint_.setBounds(area.removeFromTop(kHintHeight));
    }

private:
    // The widest of the three refusals at 11 pt needs the full width; the
    // button needs only its own words.
    static constexpr int kButtonWidth = 200;
    static constexpr int kHintHeight = 72; // four lines of hint at 11 pt

    int shown_ = 0;

    juce::Label title_;
    juce::TextButton clean_;
    juce::Label why_;
    juce::Label hint_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MaintenancePanel)
};

} // namespace loopercat
