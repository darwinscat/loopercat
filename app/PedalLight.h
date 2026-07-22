// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::PedalLight — the connection indicator, top-right of the header:
// a lit violet dot + the volume's name while a pedal is mounted, a dim hollow
// ring + "no pedal" while not. Read-only glanceable state; the status strip
// keeps the full path and counts.
//==============================================================================
namespace loopercat
{

class PedalLight final : public juce::Component
{
public:
    PedalLight() = default; // JUCE_DECLARE_NON_COPYABLE suppresses the implicit one

    void set(bool connected, juce::String label)
    {
        if (connected == connected_ && label == label_)
            return;
        connected_ = connected;
        label_ = std::move(label);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        const float d = 9.0f;
        const float cy = area.getCentreY();

        g.setFont(juce::FontOptions(12.0f));
        const juce::String text = connected_ ? label_ : juce::String("no pedal");
        juce::GlyphArrangement layout;
        layout.addLineOfText(g.getCurrentFont(), text, 0.0f, 0.0f);
        const float textWidth = layout.getBoundingBox(0, -1, true).getWidth();
        const float dotX = area.getRight() - textWidth - d - 18.0f;

        if (connected_) {
            g.setColour(felitronics::appkit::brand::violet.withAlpha(0.35f)); // soft halo
            g.fillEllipse(dotX - 3.0f, cy - d * 0.5f - 3.0f, d + 6.0f, d + 6.0f);
            g.setColour(felitronics::appkit::brand::violet);
            g.fillEllipse(dotX, cy - d * 0.5f, d, d);
            g.setColour(juce::Colour(0xffd8d8d8));
        } else {
            g.setColour(juce::Colour(0xff4a4a54));
            g.drawEllipse(dotX, cy - d * 0.5f, d, d, 1.4f);
            g.setColour(juce::Colour(0xff63636d));
        }
        g.drawText(text, juce::Rectangle<float>(dotX + d + 8.0f, 0.0f, textWidth + 4.0f,
                                                area.getHeight()),
                   juce::Justification::centredLeft, false);
    }

private:
    bool connected_ = false;
    juce::String label_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PedalLight)
};

} // namespace loopercat
