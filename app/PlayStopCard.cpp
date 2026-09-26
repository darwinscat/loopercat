// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PlayStopCard.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace playstop = loopercat::usecases::playstop;

namespace
{
    juce::String name(std::string_view text)
    {
        return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
    }

    constexpr int kCaptionHeight = 14;
    constexpr int kControlHeight = 24;
    constexpr int kCellGap = 8;
    constexpr int kRowHeight = kCaptionHeight + kControlHeight;
    constexpr int kRowGap = 4;
    constexpr int kPadX = 14;
    constexpr int kPadY = 8;
    constexpr int kTitleWidth = 104; // "Start & Stop" with room to breathe
}

PlayStopCard::PlayStopCard()
{
    makeCaption(startCaption_, "START");
    makeCaption(stopCaption_, "STOP");
    makeCaption(fadeCaption_, "FADE TIME");

    makeChoice(start_, "startMode", [this] { return values_.startMode; },
               [](long long n) { return playstop::Edits { .startMode = n }; });
    makeChoice(stop_, "stopMode", [this] { return values_.stopMode; },
               [](long long n) { return playstop::Edits { .stopMode = n }; });
    makeChoice(fade_, "fadeTime", [this] { return values_.fadeTime; },
               [](long long n) { return playstop::Edits { .fadeTime = n }; });

    for (const playstop::Choice& choice : playstop::kStartModes)
        start_.addItem(name(choice.name), static_cast<int>(choice.number) + 1);
    for (const playstop::Choice& choice : playstop::kStopModes)
        stop_.addItem(name(choice.name), static_cast<int>(choice.number) + 1);
    for (long long n = 0; n < playstop::kFadeTimeCount; ++n)
        fade_.addItem(juce::String(playstop::fadeTimeName(n)), static_cast<int>(n) + 1);

    refresh();
}

void PlayStopCard::makeCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(10.0f));
    label.setColour(juce::Label::textColourId, cardlook::kCaption);
    addAndMakeVisible(label);
}

// Item ids are the manual's numbers plus one: JUCE keeps 0 for "nothing". A
// choice reports itself only when it differs from what the slot has, and
// never from a greyed box: a lamp is mute whoever reaches it.
void PlayStopCard::makeChoice(juce::ComboBox& box, const char* id,
                              std::function<long long()> current,
                              std::function<playstop::Edits(long long)> makeEdit)
{
    box.setComponentID(id);
    box.setColour(juce::ComboBox::backgroundColourId, cardlook::kField);
    box.setColour(juce::ComboBox::textColourId, cardlook::kText);
    box.setColour(juce::ComboBox::outlineColourId, cardlook::kOutline);
    box.setColour(juce::ComboBox::arrowColourId, cardlook::kCaption);
    box.setColour(juce::ComboBox::focusedOutlineColourId, felitronics::appkit::brand::violet);
    box.onChange = [this, &box, current, makeEdit] {
        const int selected = box.getSelectedId();
        if (selected <= 0 || !box.isEnabled() || !onEdit)
            return;
        const long long number = selected - 1;
        if (number != current())
            onEdit(makeEdit(number));
    };
    addAndMakeVisible(box);
}

void PlayStopCard::setValues(const playstop::Values& values)
{
    values_ = values;
    refresh();
}

void PlayStopCard::refresh()
{
    start_.setSelectedId(static_cast<int>(values_.startMode) + 1, juce::dontSendNotification);
    stop_.setSelectedId(static_cast<int>(values_.stopMode) + 1, juce::dontSendNotification);
    fade_.setSelectedId(static_cast<int>(values_.fadeTime) + 1, juce::dontSendNotification);
    // The fade's length is heard only when a fade is set on either end; the
    // caption says so, the choice stays open — a length can be picked first.
    fadeCaption_.setText(values_.fadeInUse ? "FADE TIME" : "FADE TIME (no fade set)",
                         juce::dontSendNotification);
    repaint();
}

// A card that cannot be touched still shows what the slot holds: going grey
// (or coming back) puts the slot's own values on the boxes, so a selection
// that moved while nobody could act on it cannot outlive the greying.
void PlayStopCard::enablementChanged()
{
    for (auto* box : { &start_, &stop_, &fade_ })
        box->setEnabled(isEnabled());
    refresh();
}

void PlayStopCard::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff17171d));
    g.fillRoundedRectangle(area, 6.0f);
    if (values_.fadeInUse) { // a fade in use carries the brand's edge on its left
        g.setColour(felitronics::appkit::brand::lilac.withAlpha(isEnabled() ? 0.75f : 0.3f));
        g.fillRoundedRectangle(area.withWidth(3.0f), 1.5f);
    }
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    g.setColour(cardlook::kText.withMultipliedAlpha(alpha));
    g.setFont(juce::FontOptions(13.5f));
    // The title shares the first row with the fade's length, so it is
    // centred against that cell rather than sitting on its own line.
    g.drawText("Start & Stop",
               getLocalBounds().reduced(kPadX, kPadY).removeFromTop(kRowHeight)
                   .removeFromLeft(kTitleWidth),
               juce::Justification::centredLeft, true);
}

void PlayStopCard::resized()
{
    // The title and the fade's length on the first row, the two modes on the
    // second: "IMMEDIATE" and "LOOP END" need room, and a box that shows
    // "LOOP ..." is a control that hides the answer it was asked for.
    auto area = getLocalBounds().reduced(kPadX, kPadY);
    const auto cell = [](juce::Rectangle<int> c, juce::Label& caption, juce::ComboBox& box) {
        caption.setBounds(c.removeFromTop(kCaptionHeight));
        box.setBounds(c.withHeight(kControlHeight));
    };

    auto first = area.removeFromTop(kRowHeight);
    first.removeFromLeft(kTitleWidth); // the painted title
    cell(first, fadeCaption_, fade_);

    area.removeFromTop(kRowGap);
    auto second = area.removeFromTop(kRowHeight);
    const int half = (second.getWidth() - kCellGap) / 2;
    cell(second.removeFromLeft(half), startCaption_, start_);
    second.removeFromLeft(kCellGap);
    cell(second, stopCaption_, stop_);
}

} // namespace loopercat
