// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MemorySettingsPanel.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace
{
    const juce::Colour kText { 0xffd8d8d8 };
    const juce::Colour kDim { 0xff63636d };
    const juce::Colour kField { 0xff23232d };
} // namespace

MemorySettingsPanel::MemorySettingsPanel(const int slot, const memsettings::Values& current)
    : slot_(slot), values_(current)
{
    const auto title = [this](juce::Label& label, const char* text) {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        label.setColour(juce::Label::textColourId, kDim);
        addAndMakeVisible(label);
    };
    title(playbackTitle_, "PLAYBACK");
    title(rhythmTitle_, "RHYTHM");

    for (auto* toggle : { &reverse_, &rhythmOn_ }) {
        toggle->setColour(juce::ToggleButton::textColourId, kText);
        toggle->setColour(juce::ToggleButton::tickColourId, felitronics::appkit::brand::violet);
        toggle->setColour(juce::ToggleButton::tickDisabledColourId, kDim);
        addAndMakeVisible(*toggle);
    }

    styleSlider(playLevel_, playLevelLabel_, "Play level", memsettings::kLevelMax, 100);
    styleSlider(pan_, panLabel_, "Pan", memsettings::kPanMax, 50);
    styleSlider(rhythmLevel_, rhythmLevelLabel_, "Drum level", memsettings::kLevelMax, 100);

    gatedNote_.setText(juce::String::fromUTF8(
                           "Start/Stop modes, Pattern, Kit, Beat and Variation join once their "
                           "value maps are verified on hardware \xe2\x80\x94 no guessed labels."),
                       juce::dontSendNotification);
    gatedNote_.setFont(juce::FontOptions(11.0f));
    gatedNote_.setColour(juce::Label::textColourId, kDim);
    gatedNote_.setJustificationType(juce::Justification::topLeft);
    gatedNote_.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(gatedNote_);

    // Commits: each control compares against the last pedal-seen state and
    // fires a single-field edit. The snapshot after the write becomes the new
    // base (setValues), so a failed job snaps the control back by itself.
    reverse_.onClick = [this] {
        if (reverse_.getToggleState() != values_.reverse)
            commit({ .reverse = reverse_.getToggleState() },
                   reverse_.getToggleState() ? "reverse on" : "reverse off");
    };
    rhythmOn_.onClick = [this] {
        if (rhythmOn_.getToggleState() != values_.rhythmOn)
            commit({ .rhythmOn = rhythmOn_.getToggleState() },
                   rhythmOn_.getToggleState() ? "rhythm on" : "rhythm off");
    };
    playLevel_.onValueChange = [this] {
        const auto v = static_cast<long long>(playLevel_.getValue());
        if (v != values_.playLevel)
            commit({ .playLevel = v }, "play level " + juce::String(v));
    };
    pan_.onValueChange = [this] {
        const auto v = static_cast<long long>(pan_.getValue());
        if (v != values_.pan)
            commit({ .pan = v }, "pan " + juce::String(v));
    };
    rhythmLevel_.onValueChange = [this] {
        const auto v = static_cast<long long>(rhythmLevel_.getValue());
        if (v != values_.rhythmLevel)
            commit({ .rhythmLevel = v }, "drum level " + juce::String(v));
    };

    setValues(current);
    setSize(440, 314);
}

void MemorySettingsPanel::styleSlider(juce::Slider& slider, juce::Label& label,
                                      const char* title, const long long max,
                                      const long long snapValue)
{
    label.setText(title, juce::dontSendNotification);
    label.setFont(juce::FontOptions(13.0f));
    label.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(label);

    slider.setSliderStyle(juce::Slider::LinearHorizontal);
    slider.setRange(0.0, static_cast<double>(max), 1.0);
    slider.setDoubleClickReturnValue(true, static_cast<double>(snapValue)); // unity / center
    slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    // One committed value per gesture — a drag is one worker job, not fifty.
    slider.setChangeNotificationOnlyOnRelease(true);
    slider.setColour(juce::Slider::backgroundColourId, kField);
    slider.setColour(juce::Slider::trackColourId,
                     felitronics::appkit::brand::violet.withAlpha(0.55f));
    slider.setColour(juce::Slider::thumbColourId, felitronics::appkit::brand::violet);
    slider.setColour(juce::Slider::textBoxTextColourId, kText);
    slider.setColour(juce::Slider::textBoxBackgroundColourId, kField);
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colour(0xff2a2a34));
    addAndMakeVisible(slider);
}

void MemorySettingsPanel::commit(memsettings::Edits edits, const juce::String& what)
{
    if (onEdit)
        onEdit(slot_, edits, "Memory settings: " + what + " on slot " + juce::String(slot_));
}

void MemorySettingsPanel::setValues(const memsettings::Values& values)
{
    values_ = values;
    // A control mid-gesture keeps the user's hand; everything else follows
    // the pedal truth (this is also what reverts a failed or refused job).
    if (!reverse_.isDown())
        reverse_.setToggleState(values.reverse, juce::dontSendNotification);
    if (!rhythmOn_.isDown())
        rhythmOn_.setToggleState(values.rhythmOn, juce::dontSendNotification);
    const std::pair<juce::Slider*, long long> sliders[] = { { &playLevel_, values.playLevel },
                                                            { &pan_, values.pan },
                                                            { &rhythmLevel_, values.rhythmLevel } };
    for (const auto& [slider, value] : sliders)
        if (!slider->isMouseButtonDown())
            slider->setValue(static_cast<double>(value), juce::dontSendNotification);
}

void MemorySettingsPanel::setBusy(const bool busy)
{
    juce::Component* const controls[] = { &reverse_, &rhythmOn_, &playLevel_, &pan_,
                                          &rhythmLevel_ };
    for (auto* c : controls)
        c->setEnabled(!busy);
}

void MemorySettingsPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff121218)); // the family stage — matches the dialog background
}

void MemorySettingsPanel::resized()
{
    auto area = getLocalBounds().reduced(18, 14);
    const auto sliderRow = [&area](juce::Label& label, juce::Slider& slider) {
        auto row = area.removeFromTop(30);
        label.setBounds(row.removeFromLeft(88));
        slider.setBounds(row);
    };

    playbackTitle_.setBounds(area.removeFromTop(20));
    reverse_.setBounds(area.removeFromTop(26));
    sliderRow(playLevelLabel_, playLevel_);
    sliderRow(panLabel_, pan_);
    area.removeFromTop(12);

    rhythmTitle_.setBounds(area.removeFromTop(20));
    rhythmOn_.setBounds(area.removeFromTop(26));
    sliderRow(rhythmLevelLabel_, rhythmLevel_);
    area.removeFromTop(10);

    gatedNote_.setBounds(area);
}

} // namespace loopercat
