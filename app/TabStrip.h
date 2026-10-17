// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <felitronics/appkit/Brand.h>
#include <loopercat/Error.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// loopercat::TabStrip — a flat row of tab titles with the brand underline on
// the live one. JUCE's TabbedComponent brings its own chrome; this pane sits
// inside a dark stage where a rule and a colour say everything a border would.
//
// A strip built with a lead column keeps room ahead of its first tab for a
// label naming what every tab is about — the studio's "SLOT nn" (#145), true
// whichever tab is open. The column is there whether or not a label is set,
// so the tabs never move; the label is not a tab: no hover, no underline,
// and a click on it selects nothing.
//==============================================================================
namespace loopercat
{

class TabStrip final : public juce::Component
{
public:
    enum class Lead { none, label };

    explicit TabStrip(juce::StringArray titles, Lead lead = Lead::none)
        : titles_(std::move(titles)), lead_(lead)
    {
    }

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

    // The words ahead of the tabs; empty clears them. The column was sized
    // for "SLOT nn", and a label that would not fit is refused rather than
    // cut: a number shown short is a different number. One line only: the
    // width is measured on the first line, and a second one would paint
    // past the row with the number out of sight.
    void setLeadingLabel(juce::String label)
    {
        if (lead_ != Lead::label)
            throw Error("this tab strip has no lead column for \"" + label.toStdString() + "\"");
        if (label.containsAnyOf("\r\n"))
            throw Error("a label with a line break cannot lead the tab strip");
        if (juce::GlyphArrangement::getStringWidthInt(labelFont(), label) > labelBounds().getWidth())
            throw Error("\"" + label.toStdString() + "\" does not fit the tab strip's lead column");
        if (label == leadingLabel_)
            return;
        leadingLabel_ = std::move(label);
        repaint();
    }

    const juce::String& leadingLabel() const { return leadingLabel_; }

    // Where a tab sits and which tab a point falls on: the lead column comes
    // off both in the one place (leadWidth), so what is drawn and what is
    // hit cannot disagree, and a click on the label names no tab.
    juce::Rectangle<int> tabBounds(int index) const
    {
        return { leadWidth() + index * kTabWidth, 0, kTabWidth, getHeight() };
    }

    int tabAt(int x) const
    {
        const int local = x - leadWidth();
        if (local < 0)
            return -1;
        const int index = local / kTabWidth;
        return index < titles_.size() ? index : -1;
    }

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
        if (leadingLabel_.isNotEmpty()) {
            // The panes' lilac, left-aligned where their own content starts,
            // and a divider in the rule's colour: a heading, not a fifth tab.
            g.setColour(felitronics::appkit::brand::lilac);
            g.setFont(labelFont());
            g.drawText(leadingLabel_, labelBounds(), juce::Justification::centredLeft, false);
            g.setColour(kRule);
            g.fillRect(leadWidth() - 1, kDividerInset, 1, getHeight() - 2 * kDividerInset);
        }
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
        g.setColour(kRule);
        g.fillRect(0, getHeight() - 1, getWidth(), 1);
    }

    static constexpr int kTabWidth = 96;
    // The panes padded their content by 14 px and kept 76 px for "SLOT nn"
    // before #145; the column holds the same label at the same x, with a gap
    // before the divider, and the tabs start past it.
    static constexpr int kLeadWidth = 88;

private:
    static constexpr int kLeadInset = 14; // the panes' kPad: the label keeps its x
    static constexpr int kLeadGap = 8;    // air between the label and the divider
    static constexpr int kDividerInset = 6;
    inline static const juce::Colour kRule { 0xff1e1e26 };

    int leadWidth() const { return lead_ == Lead::label ? kLeadWidth : 0; }

    juce::Rectangle<int> labelBounds() const
    {
        return { kLeadInset, 0, kLeadWidth - kLeadInset - kLeadGap, getHeight() };
    }

    // The size the panes drew it at: the label moved up, it did not change.
    static juce::Font labelFont() { return juce::Font(juce::FontOptions(14.0f)); }

    const juce::StringArray titles_;
    const Lead lead_;
    juce::String leadingLabel_;
    int selected_ = 0;
    int hovered_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TabStrip)
};

} // namespace loopercat
