// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Commands.hpp>

#include "Strings.h"

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::BannerStrip — the health banners: doctor findings from the last
// scan plus the last failed mutation, one line each. Findings reflect the
// volume and stay until the volume heals; the job error line is dismissed by
// clicking the strip (or by the next successful mutation). The owner asks
// preferredHeight() and relayouts when content changes — no banners, no strip.
//==============================================================================
namespace loopercat
{

class BannerStrip final : public juce::Component
{
public:
    BannerStrip() = default; // JUCE_DECLARE_NON_COPYABLE suppresses the implicit one

    static constexpr int kMaxLines = 4;
    static constexpr int kLineHeight = 19;

    std::function<void()> onLayoutChange; // preferredHeight changed — relayout me

    void setContent(std::vector<commands::Finding> findings, juce::String jobError)
    {
        const int before = preferredHeight();
        findings_ = std::move(findings);
        jobError_ = std::move(jobError);
        if (preferredHeight() != before && onLayoutChange)
            onLayoutChange();
        repaint();
    }

    void dismissJobError()
    {
        if (jobError_.isEmpty())
            return;
        setContent(findings_, {});
    }

    int preferredHeight() const
    {
        const int lines = lineCount();
        return lines == 0 ? 0 : lines * kLineHeight + 6;
    }

    void paint(juce::Graphics& g) override
    {
        if (lineCount() == 0)
            return;
        g.setColour(juce::Colour(0xff1a1512)); // dark amber-tinted strip
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setFont(juce::FontOptions(12.0f));

        auto area = getLocalBounds().reduced(10, 3);
        int drawn = 0;
        const int total = totalLines();

        const auto drawLine = [&](const juce::String& text, juce::Colour colour) {
            g.setColour(colour);
            g.drawText(text, area.removeFromTop(kLineHeight), juce::Justification::centredLeft, true);
            ++drawn;
        };

        if (jobError_.isNotEmpty())
            drawLine(jobError_ + "   (click to dismiss)", felitronics::appkit::brand::orange);
        for (const auto& finding : findings_) {
            if (drawn >= kMaxLines - (total > kMaxLines ? 1 : 0))
                break;
            drawLine(utf8(finding.message), colourFor(finding.level));
        }
        if (total > drawn)
            drawLine("+" + juce::String(total - drawn) + juce::String::fromUTF8(" more\xe2\x80\xa6"),
                     juce::Colour(0xff8a8a92));
    }

    void mouseDown(const juce::MouseEvent&) override { dismissJobError(); }

private:
    static juce::Colour colourFor(commands::Level level)
    {
        switch (level) {
        case commands::Level::error: return felitronics::appkit::brand::orange;
        case commands::Level::warn:  return felitronics::appkit::brand::orange.withAlpha(0.75f);
        case commands::Level::info:  return juce::Colour(0xff8a8a92);
        }
        return juce::Colour(0xff8a8a92);
    }

    int totalLines() const
    {
        return static_cast<int>(findings_.size()) + (jobError_.isNotEmpty() ? 1 : 0);
    }

    int lineCount() const { return juce::jmin(totalLines(), kMaxLines); }

    std::vector<commands::Finding> findings_;
    juce::String jobError_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BannerStrip)
};

} // namespace loopercat
