// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/Brand.h>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::TabStrip — a flat row of tab titles with the brand underline on
// the live one. JUCE's TabbedComponent brings its own chrome; this pane sits
// inside a dark stage where a rule and a colour say everything a border would.
//==============================================================================
namespace loopercat
{

class TabStrip final : public juce::Component
{
public:
    explicit TabStrip(juce::StringArray titles) : titles_(std::move(titles)) {}

    std::function<void(int)> onTabChanged; // fires only on an actual change

    void select(int index)
    {
        if (index < 0 || index >= titles_.size() || index == selected_)
            return;
        selected_ = index;
        repaint();
        if (onTabChanged)
            onTabChanged(selected_);
    }

    int selected() const { return selected_; }

    void mouseDown(const juce::MouseEvent& e) override { select(tabAt(e.x)); }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int over = tabAt(e.x);
        if (over != hovered_) {
            hovered_ = over;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hovered_ = -1;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        for (int i = 0; i < titles_.size(); ++i) {
            const auto tab = tabBounds(i);
            const bool live = i == selected_;
            g.setColour(live ? felitronics::appkit::brand::lilac
                             : juce::Colour(0xff8a8a92).withAlpha(i == hovered_ ? 0.9f : 0.6f));
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(titles_[i], tab, juce::Justification::centred, false);
            if (live) {
                g.setColour(felitronics::appkit::brand::violet);
                g.fillRect(tab.getX() + 6, getHeight() - 2, tab.getWidth() - 12, 2);
            }
        }
        // The rule the tabs sit on, so the strip reads as one surface.
        g.setColour(juce::Colour(0xff1e1e26));
        g.fillRect(0, getHeight() - 1, getWidth(), 1);
    }

private:
    juce::Rectangle<int> tabBounds(int index) const
    {
        return { index * kTabWidth, 0, kTabWidth, getHeight() };
    }

    int tabAt(int x) const
    {
        const int index = x / kTabWidth;
        return index >= 0 && index < titles_.size() ? index : -1;
    }

    static constexpr int kTabWidth = 96;

    const juce::StringArray titles_;
    int selected_ = 0;
    int hovered_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabStrip)
};

} // namespace loopercat
