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

    // Where the owner wants the strip: centred on `centreX`, its top at
    // `top`. The width is the toast's own — one sentence's worth, wider for
    // a longer one (see place).
    void anchor(int centreX, int top)
    {
        centreX_ = centreX;
        top_ = top;
        place();
    }

    void show(juce::String message)
    {
        text_ = std::move(message);
        place();
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
        g.setFont(font());
        g.drawText(text_, getLocalBounds().reduced(kPadding, 0), juce::Justification::centred, true);
    }

private:
    void timerCallback() override
    {
        stopTimer();
        setVisible(false);
    }

    static juce::Font font() { return juce::Font(juce::FontOptions(13.0f)); }

    // The strip is sized for one sentence. A departure that carries the
    // first snapshot's interruption (issue #146) is two, and drawText would
    // cut it to an ellipsis: the strip widens around its anchor for the text
    // it has, as far as its parent allows, and is the one sentence's width
    // again for the next short one.
    void place()
    {
        int width = kWidth;
        if (auto* parent = getParentComponent()) {
            const int wanted = juce::GlyphArrangement::getStringWidthInt(font(), text_) + 2 * kPadding;
            const int room = juce::jmax(kWidth, parent->getWidth() - 2 * kMargin);
            width = juce::jlimit(kWidth, room, wanted);
        }
        setBounds(centreX_ - width / 2, top_, width, kHeight);
    }

    static constexpr int kWidth = 560;   // one sentence, as the layout always drew it
    static constexpr int kHeight = 34;
    static constexpr int kPadding = 14;  // text inset, each side
    static constexpr int kMargin = 16;   // kept clear of the window's edges when wider

    juce::String text_;
    int centreX_ = 0;
    int top_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Toast)
};

} // namespace loopercat
