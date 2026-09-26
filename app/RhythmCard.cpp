// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "RhythmCard.h"

#include "Strings.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace rhythm = loopercat::usecases::rhythm;

namespace
{
    juce::String name(std::string_view text)
    {
        return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
    }

    // A number as the pedal's screen prints it: TONE carries its sign.
    juce::String signedNumber(long long value)
    {
        return (value > 0 ? "+" : "") + juce::String(value);
    }

    constexpr int kControlHeight = 24;
    constexpr int kCaptionHeight = 14;
    constexpr int kCellGap = 10;
}

//==============================================================================
RhythmOptions::RhythmOptions(const rhythm::Values& values,
                             std::function<void(rhythm::Edits)> onEdit)
    : values_(values), onEdit_(std::move(onEdit))
{
    makeCaption(patternCaption_, "PATTERN");
    makeCaption(kitCaption_, "KIT");
    makeCaption(beatCaption_, "BEAT");
    makeCaption(variationCaption_, "VARIATION");
    makeCaption(levelCaption_, "LEVEL");
    makeCaption(reverbCaption_, "REVERB");
    makeCaption(toneLowCaption_, "TONE LOW");
    makeCaption(toneHighCaption_, "TONE HIGH");

    // The grooves, without Blank: Blank is the count-in's silence, and the
    // way to it is the switch, not this list (Rhythm.hpp).
    makeChoice(pattern_, std::span(rhythm::kPatterns).first(rhythm::kPatterns.size() - 1),
               [](long long n) { return rhythm::Edits { .pattern = n }; });
    makeChoice(kit_, rhythm::kKits, [](long long n) { return rhythm::Edits { .kit = n }; });
    makeChoice(beat_, rhythm::kBeats, [](long long n) { return rhythm::Edits { .beat = n }; });
    makeChoice(variation_, rhythm::kVariations,
               [](long long n) { return rhythm::Edits { .variation = n }; });

    levelNow_ = [this] { return values_.level; };
    reverbNow_ = [this] { return values_.reverb; };
    toneLowNow_ = [this] { return values_.toneLow; };
    toneHighNow_ = [this] { return values_.toneHigh; };
    makeNumber(level_, 3, "0123456789", [](long long n) { return rhythm::Edits { .level = n }; });
    makeNumber(reverb_, 3, "0123456789", [](long long n) { return rhythm::Edits { .reverb = n }; });
    makeNumber(toneLow_, 3, "-0123456789", [](long long n) { return rhythm::Edits { .toneLow = n }; });
    makeNumber(toneHigh_, 3, "-0123456789",
               [](long long n) { return rhythm::Edits { .toneHigh = n }; });

    setSize(kWidth, kHeight);
    refresh();
}

void RhythmOptions::makeCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(10.0f));
    label.setColour(juce::Label::textColourId, cardlook::kCaption);
    addAndMakeVisible(label);
}

void RhythmOptions::makeChoice(juce::ComboBox& box, std::span<const rhythm::Choice> list,
                               std::function<rhythm::Edits(long long)> makeEdit)
{
    // Item ids are the manual's numbers plus one: JUCE keeps 0 for "nothing".
    for (const rhythm::Choice& choice : list)
        box.addItem(name(choice.name), static_cast<int>(choice.number) + 1);
    box.setColour(juce::ComboBox::backgroundColourId, cardlook::kField);
    box.setColour(juce::ComboBox::textColourId, cardlook::kText);
    box.setColour(juce::ComboBox::outlineColourId, cardlook::kOutline);
    box.setColour(juce::ComboBox::arrowColourId, cardlook::kCaption);
    box.setColour(juce::ComboBox::focusedOutlineColourId, felitronics::appkit::brand::violet);
    box.onChange = [this, &box, makeEdit] {
        const int id = box.getSelectedId();
        if (id > 0 && onEdit_)
            onEdit_(makeEdit(id - 1));
    };
    addAndMakeVisible(box);
}

void RhythmOptions::makeNumber(juce::TextEditor& editor, int cells, const juce::String& allowed,
                               std::function<rhythm::Edits(long long)> makeEdit)
{
    cardlook::styleFieldEditor(editor);
    editor.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                     juce::Font::plain));
    editor.setInputRestrictions(cells, allowed);
    editor.setJustification(juce::Justification::centredRight);
    editor.onReturnKey = [this, &editor, makeEdit] { commitNumber(editor, 0, makeEdit); };
    editor.onFocusLost = [this, &editor, makeEdit] { commitNumber(editor, 0, makeEdit); };
    editor.onEscapeKey = [this] { refresh(); };
    addAndMakeVisible(editor);
}

// A typed number becomes an edit only when it differs from what the slot
// has now; an empty or unchanged field is nothing. Range enforcement is the
// core's job — its typed error reaches the banner in the manual's words.
void RhythmOptions::commitNumber(juce::TextEditor& editor, long long,
                                 std::function<rhythm::Edits(long long)> makeEdit)
{
    const juce::String text = editor.getText().trim();
    if (text.isEmpty() || text == "-" || !onEdit_)
        return;
    const long long value = text.getLargeIntValue();
    const long long current = &editor == &level_      ? levelNow_()
                            : &editor == &reverb_     ? reverbNow_()
                            : &editor == &toneLow_    ? toneLowNow_()
                                                      : toneHighNow_();
    if (value == current)
        return;
    onEdit_(makeEdit(value));
}

void RhythmOptions::setValues(const rhythm::Values& values)
{
    values_ = values;
    refresh();
}

void RhythmOptions::refresh()
{
    pattern_.setSelectedId(static_cast<int>(values_.pattern) + 1, juce::dontSendNotification);
    kit_.setSelectedId(static_cast<int>(values_.kit) + 1, juce::dontSendNotification);
    beat_.setSelectedId(static_cast<int>(values_.beat) + 1, juce::dontSendNotification);
    variation_.setSelectedId(static_cast<int>(values_.variation) + 1, juce::dontSendNotification);
    // The manual's rule, said where it applies: a recorded take fixes the beat.
    beat_.setEnabled(isEnabled() && !values_.beatLocked);
    beatCaption_.setText(values_.beatLocked ? "BEAT (fixed by the take)" : "BEAT",
                         juce::dontSendNotification);

    // A field being typed in is the user's, not ours.
    const auto put = [](juce::TextEditor& editor, const juce::String& text) {
        if (!editor.hasKeyboardFocus(true))
            editor.setText(text, juce::dontSendNotification);
    };
    put(level_, juce::String(values_.level));
    put(reverb_, juce::String(values_.reverb));
    put(toneLow_, signedNumber(values_.toneLow));
    put(toneHigh_, signedNumber(values_.toneHigh));
}

void RhythmOptions::enablementChanged()
{
    for (auto* control : std::initializer_list<juce::Component*> { &pattern_, &kit_, &variation_,
                                                                   &level_, &reverb_, &toneLow_,
                                                                   &toneHigh_ })
        control->setEnabled(isEnabled());
    beat_.setEnabled(isEnabled() && !values_.beatLocked);
}

void RhythmOptions::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff17171d));
}

void RhythmOptions::resized()
{
    // Two rows of four: the choices above, the numbers below — the RHYTHM
    // screen's order, read left to right.
    auto area = getLocalBounds().reduced(14, 10);
    const int cellWidth = (area.getWidth() - 3 * kCellGap) / 4;
    const int rowHeight = kCaptionHeight + kControlHeight;

    auto row = area.removeFromTop(rowHeight);
    const auto cell = [&row, cellWidth](juce::Label& caption, juce::Component& control) {
        auto c = row.removeFromLeft(cellWidth);
        row.removeFromLeft(kCellGap);
        caption.setBounds(c.removeFromTop(kCaptionHeight));
        control.setBounds(c.withHeight(kControlHeight));
    };
    cell(patternCaption_, pattern_);
    cell(kitCaption_, kit_);
    cell(beatCaption_, beat_);
    cell(variationCaption_, variation_);

    area.removeFromTop(kCellGap);
    row = area.removeFromTop(rowHeight);
    // Numbers are short: the field is as wide as its widest value.
    const auto numberCell = [&row, cellWidth](juce::Label& caption, juce::TextEditor& editor) {
        auto c = row.removeFromLeft(cellWidth);
        row.removeFromLeft(kCellGap);
        caption.setBounds(c.removeFromTop(kCaptionHeight));
        editor.setBounds(c.withHeight(kControlHeight).withWidth(52));
    };
    numberCell(levelCaption_, level_);
    numberCell(reverbCaption_, reverb_);
    numberCell(toneLowCaption_, toneLow_);
    numberCell(toneHighCaption_, toneHigh_);
}

//==============================================================================
RhythmCard::RhythmCard()
{
    setInterceptsMouseClicks(true, false);
}

void RhythmCard::setValues(const rhythm::Values& values, std::optional<long long> patternLost)
{
    values_ = values;
    patternLost_ = patternLost;
    if (studio_ != nullptr)
        studio_->setValues(values_);
    repaint();
}

void RhythmCard::enablementChanged()
{
    if (studio_ != nullptr)
        studio_->setEnabled(isEnabled());
    repaint();
}

// The studio belongs to the slot on the card: the card going away (another
// tab, no selection, the pedal gone) takes the studio with it.
void RhythmCard::visibilityChanged()
{
    if (!isShowing() && studio_ != nullptr)
        if (auto* box = studio_->findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
}

void RhythmCard::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled() || !onEdit)
        return;
    if (e.x >= getWidth() - cardlook::kSwitchWidth - 14) {
        onEdit(rhythm::Edits { .on = !values_.on });
        return;
    }
    openStudio();
}

void RhythmCard::openStudio()
{
    if (studio_ != nullptr)
        return;
    auto studio = std::make_unique<RhythmOptions>(values_, [this](rhythm::Edits edits) {
        if (onEdit)
            onEdit(std::move(edits));
    });
    studio->setEnabled(isEnabled());
    studio_ = studio.get();
    // Anchored to the card, on top of the strip: the callout picks the side
    // with room and takes ownership of the studio.
    juce::CallOutBox::launchAsynchronously(std::move(studio), getScreenBounds(), nullptr);
}

void RhythmCard::mouseEnter(const juce::MouseEvent&) { hovered_ = true; repaint(); }
void RhythmCard::mouseExit(const juce::MouseEvent&) { hovered_ = false; repaint(); }

juce::String RhythmCard::summary() const
{
    return name(rhythm::patternName(values_.pattern)) + juce::String::fromUTF8(" \xc2\xb7 ")
         + name(rhythm::kitName(values_.kit)) + juce::String::fromUTF8(" \xc2\xb7 ")
         + name(rhythm::beatName(values_.beat)) + juce::String::fromUTF8(" \xc2\xb7 ")
         + name(rhythm::variationName(values_.variation));
}

void RhythmCard::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(juce::Colour(hovered_ && isEnabled() ? 0xff1b1b23 : 0xff17171d));
    g.fillRoundedRectangle(area, 6.0f);
    if (values_.on) { // a live setting carries the brand's edge on its left
        g.setColour(felitronics::appkit::brand::lilac.withAlpha(isEnabled() ? 0.75f : 0.3f));
        g.fillRoundedRectangle(area.withWidth(3.0f), 1.5f);
    }

    const float alpha = isEnabled() ? 1.0f : 0.45f;
    auto text = getLocalBounds().reduced(14, 10);
    const auto switchArea = text.removeFromRight(cardlook::kSwitchWidth);

    g.setColour(cardlook::kText.withMultipliedAlpha(alpha));
    g.setFont(juce::FontOptions(13.5f));
    g.drawText("Rhythm", text.removeFromTop(18), juce::Justification::centredLeft, true);

    // What the drums are, or would be: the same line either way, so the
    // groove chosen on the pedal is never hidden by the switch being off.
    // The studio is a click away, and the line says so.
    const juce::String meaning = values_.on
        ? summary() + juce::String::fromUTF8(" \xe2\x80\x94 click to change.")
        : "No drums. " + summary() + " is set; click to change.";
    g.setColour(cardlook::kCaption.withMultipliedAlpha(alpha));
    g.setFont(juce::FontOptions(11.5f));
    g.drawFittedText(meaning, text.removeFromTop(patternLost_ ? 16 : 30),
                     juce::Justification::topLeft, 2);

    if (patternLost_) {
        g.setColour(felitronics::appkit::brand::orange.withMultipliedAlpha(alpha * 0.9f));
        g.setFont(juce::FontOptions(11.5f));
        g.drawFittedText("Switching it off keeps the count-in and forgets "
                             + name(rhythm::patternName(*patternLost_)) + ".",
                         text, juce::Justification::topLeft, 2);
    }

    cardlook::drawSwitch(g, switchArea.withHeight(20).withY(12).toFloat(), values_.on, alpha);
}

} // namespace loopercat
