// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "AppSettings.h"
#include "TabStrip.h"

#include <felitronics/appkit/AudioSettingsPanel.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::SettingsDialog — the app's own settings, behind the gear in the
// header: the audio output on one tab, what the slot table shows on another.
//
// Table columns are a preference, not a mode: the pedal's own facts (name,
// duration, bars, tempo, file) always show, and the per-slot behaviour flags
// are opt-in — One Shot on by default because most players use it, Play
// Count-In off until someone asks for it.
//==============================================================================
namespace loopercat
{

class SettingsDialog final : public juce::Component
{
public:
    struct Columns {
        bool oneShot;
        bool countIn;
    };

    SettingsDialog(juce::AudioDeviceManager& devices, AppSettings& settings,
                   Columns columns, std::function<void(Columns)> onColumnsChanged)
        : audio_(devices, { .minInputs = 0, .maxInputs = 0, .minOutputs = 2, .maxOutputs = 2 }),
          settings_(settings),
          onColumnsChanged_(std::move(onColumnsChanged))
    {
        tabs_.onTabChanged = [this](int index) { showTab(index); };

        columnsTitle_.setText("Show these columns in the slot table",
                              juce::dontSendNotification);
        columnsTitle_.setFont(juce::FontOptions(12.0f));
        columnsTitle_.setColour(juce::Label::textColourId, juce::Colour(0xff8a8a92));

        oneShot_.setToggleState(columns.oneShot, juce::dontSendNotification);
        countIn_.setToggleState(columns.countIn, juce::dontSendNotification);
        for (auto* toggle : { &oneShot_, &countIn_ }) {
            toggle->setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd8d8d8));
            toggle->setColour(juce::ToggleButton::tickColourId,
                              felitronics::appkit::brand::violet);
            toggle->onClick = [this] { commitColumns(); };
            addChildComponent(toggle);
        }
        addChildComponent(columnsTitle_);

        addAndMakeVisible(tabs_);
        addAndMakeVisible(audio_);
        showTab(0);
        setSize(520, 300);
    }

    ~SettingsDialog() override
    {
        if (auto* file = settings_.file()) {
            file->setValue(kDeviceStateKey, audio_.saveState());
            file->saveIfNeeded();
        }
    }

    static constexpr const char* kDeviceStateKey = "audioDeviceState";

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff121218)); }

    void resized() override
    {
        auto area = getLocalBounds();
        tabs_.setBounds(area.removeFromTop(30));
        area = area.reduced(12, 10);
        audio_.setBounds(area);

        columnsTitle_.setBounds(area.removeFromTop(20));
        area.removeFromTop(8);
        oneShot_.setBounds(area.removeFromTop(26));
        countIn_.setBounds(area.removeFromTop(26));
    }

private:
    void showTab(int index)
    {
        audio_.setVisible(index == 0);
        columnsTitle_.setVisible(index == 1);
        oneShot_.setVisible(index == 1);
        countIn_.setVisible(index == 1);
    }

    void commitColumns()
    {
        if (onColumnsChanged_)
            onColumnsChanged_({ oneShot_.getToggleState(), countIn_.getToggleState() });
    }

    TabStrip tabs_ { { "Audio", "Columns" } };
    felitronics::appkit::AudioSettingsPanel audio_;
    AppSettings& settings_;
    std::function<void(Columns)> onColumnsChanged_;
    juce::Label columnsTitle_;
    juce::ToggleButton oneShot_ { "One Shot" };
    juce::ToggleButton countIn_ { "Play Count-In" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsDialog)
};

} // namespace loopercat
