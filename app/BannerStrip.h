// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "BannerModel.h"
#include "Strings.h"

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::BannerStrip — the health banners, drawn from banners::Model (the
// single owner of what shows, issue #3): error lanes, the lifecycle line,
// doctor findings. Clicking the strip dismisses the error lines — for good;
// only a new error event brings one back. The owner asks preferredHeight()
// and relayouts when content changes — no banners, no strip.
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

    // One scan's worth of truth: lifecycle state + doctor findings. The model
    // also runs its recovery policy here (a reconnect clears the connection
    // error lane).
    void scan(lifecycle::State state, std::vector<commands::Finding> findings)
    {
        mutate([&] { model_.scan(state, std::move(findings)); });
    }

    void showError(banners::Source source, const juce::String& text)
    {
        mutate([&] { model_.showError(source, text.toStdString()); });
    }

    void clearJobError()
    {
        mutate([&] { model_.clearJobError(); });
    }

    int preferredHeight() const
    {
        const int lines = juce::jmin(static_cast<int>(model_.lines().size()), kMaxLines);
        return lines == 0 ? 0 : lines * kLineHeight + 6;
    }

    void paint(juce::Graphics& g) override
    {
        const std::vector<banners::Line> lines = model_.lines();
        if (lines.empty())
            return;
        g.setColour(juce::Colour(0xff1a1512)); // dark amber-tinted strip
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
        g.setFont(juce::FontOptions(12.0f));

        auto area = getLocalBounds().reduced(10, 3);
        const int total = static_cast<int>(lines.size());
        const int shown = juce::jmin(total, total > kMaxLines ? kMaxLines - 1 : kMaxLines);

        for (int i = 0; i < shown; ++i) {
            const banners::Line& line = lines[static_cast<std::size_t>(i)];
            juce::String text = utf8(line.text);
            if (line.dismissible)
                text << "   (click to dismiss)";
            g.setColour(colourFor(line.level));
            g.drawText(text, area.removeFromTop(kLineHeight), juce::Justification::centredLeft,
                       true);
        }
        if (total > shown) {
            g.setColour(juce::Colour(0xff8a8a92));
            g.drawText("+" + juce::String(total - shown) + juce::String::fromUTF8(" more\xe2\x80\xa6"),
                       area.removeFromTop(kLineHeight), juce::Justification::centredLeft, true);
        }
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (model_.hasDismissible())
            mutate([&] { model_.dismiss(); });
    }

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

    template <typename Mutation>
    void mutate(Mutation&& mutation)
    {
        const int before = preferredHeight();
        mutation();
        if (preferredHeight() != before && onLayoutChange)
            onLayoutChange();
        repaint();
    }

    banners::Model model_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BannerStrip)
};

} // namespace loopercat
