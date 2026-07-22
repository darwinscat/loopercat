// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::Toast — the transient bottom-center confirmation (the reference
// web UI's toast): success messages fade after a few seconds, never steal the
// mouse, never require dismissal. Errors stay in the banner strip — a toast
// is for "it worked".
//==============================================================================
namespace loopercat
{

class Toast final : public juce::Component,
                    private juce::Timer
{
public:
    Toast() { setInterceptsMouseClicks(false, false); }

    void show(juce::String message)
    {
        text_ = std::move(message);
        setVisible(true);
        toFront(false);
        repaint();
        startTimer(3500);
    }

    void paint(juce::Graphics& g) override
    {
        if (text_.isEmpty())
            return;
        const auto area = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xf0242430));
        g.fillRoundedRectangle(area, 10.0f);
        g.setColour(felitronics::appkit::brand::violet.withAlpha(0.5f));
        g.drawRoundedRectangle(area.reduced(0.5f), 10.0f, 1.0f);
        g.setColour(juce::Colour(0xffe6e6ee));
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(text_, getLocalBounds().reduced(14, 0), juce::Justification::centred, true);
    }

private:
    void timerCallback() override
    {
        stopTimer();
        setVisible(false);
    }

    juce::String text_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Toast)
};

} // namespace loopercat
