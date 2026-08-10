// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::UseCaseCard — one thing a player wants, as one control.
//
// The title is the pedal's own word for it (One Shot, Play Count-In), because
// that is the word on the hardware and in the manual; the line under it says
// what the setting does for the music. Anything the switch would cost gets a
// third line, in brand orange, BEFORE the click rather than after it.
//
// A card knows nothing about memory files: it takes strings and a state, and
// reports that its switch was clicked.
//==============================================================================
namespace loopercat
{

class UseCaseCard final : public juce::Component
{
public:
    explicit UseCaseCard(juce::String title) : title_(std::move(title))
    {
        setInterceptsMouseClicks(true, false);
    }

    std::function<void()> onToggle;

    void setState(bool on, juce::String meaning, juce::String cost = {})
    {
        on_ = on;
        meaning_ = std::move(meaning);
        cost_ = std::move(cost);
        repaint();
    }

    void enablementChanged() override { repaint(); } // greys out while a job runs

    // Height follows content: the cost line only exists when there is a cost.
    int preferredHeight() const { return cost_.isNotEmpty() ? 78 : 58; }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (isEnabled() && onToggle)
            onToggle();
    }

    void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = false; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(hovered_ && isEnabled() ? 0xff1b1b23 : 0xff17171d));
        g.fillRoundedRectangle(area, 6.0f);
        if (on_) { // a live setting carries the brand's edge on its left
            g.setColour(felitronics::appkit::brand::lilac.withAlpha(isEnabled() ? 0.75f : 0.3f));
            g.fillRoundedRectangle(area.withWidth(3.0f), 1.5f);
        }

        const float alpha = isEnabled() ? 1.0f : 0.45f;
        auto text = getLocalBounds().reduced(14, 10);
        const auto switchArea = text.removeFromRight(46);

        g.setColour(juce::Colour(0xffd8d8d8).withMultipliedAlpha(alpha));
        g.setFont(juce::FontOptions(13.5f));
        g.drawText(title_, text.removeFromTop(18), juce::Justification::centredLeft, true);

        g.setColour(juce::Colour(0xff8a8a92).withMultipliedAlpha(alpha));
        g.setFont(juce::FontOptions(11.5f));
        g.drawFittedText(meaning_, text.removeFromTop(cost_.isEmpty() ? 30 : 16),
                         juce::Justification::topLeft, 2);

        if (cost_.isNotEmpty()) {
            g.setColour(felitronics::appkit::brand::orange.withMultipliedAlpha(alpha * 0.9f));
            g.setFont(juce::FontOptions(11.5f));
            g.drawFittedText(cost_, text, juce::Justification::topLeft, 2);
        }

        paintSwitch(g, switchArea.withHeight(20).withY(12).toFloat(), alpha);
    }

private:
    void paintSwitch(juce::Graphics& g, juce::Rectangle<float> area, float alpha) const
    {
        const auto track = area.withSizeKeepingCentre(38.0f, 18.0f);
        const float radius = track.getHeight() * 0.5f;
        g.setColour((on_ ? felitronics::appkit::brand::violet.withAlpha(0.55f)
                         : juce::Colour(0xff2a2a34))
                        .withMultipliedAlpha(alpha));
        g.fillRoundedRectangle(track, radius);
        g.setColour((on_ ? felitronics::appkit::brand::lilac : juce::Colour(0xff5a5a66))
                        .withMultipliedAlpha(alpha));
        const float knob = track.getHeight() - 4.0f;
        g.fillEllipse(on_ ? track.getRight() - knob - 2.0f : track.getX() + 2.0f,
                      track.getY() + 2.0f, knob, knob);
    }

    const juce::String title_;
    juce::String meaning_, cost_;
    bool on_ = false;
    bool hovered_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(UseCaseCard)
};

} // namespace loopercat
