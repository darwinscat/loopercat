// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "RhythmPane.h"

#include "RefusalWords.h"
#include "Strings.h"

#include <felitronics/appkit/Brand.h>

namespace loopercat
{

namespace rhythm = loopercat::usecases::rhythm;

namespace
{
    const juce::Colour kPaneBackground { 0xff0e0e13 };
    const juce::Colour kDim { 0xff63636d };

    constexpr int kPad = 14;
    constexpr int kHeaderHeight = 28;
    constexpr int kCellGap = 10;
    constexpr int kNumberWidth = 52; // "200" or "-10" in a monospaced field, with room

    juce::String name(std::string_view text)
    {
        return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
    }

    // A number as the pedal's screen prints it: TONE carries its sign.
    juce::String signedNumber(long long value)
    {
        return (value > 0 ? "+" : "") + juce::String(value);
    }

}

//==============================================================================
void RhythmSwitch::setState(bool on, juce::String meaning)
{
    on_ = on;
    meaning_ = std::move(meaning);
    repaint();
}

void RhythmSwitch::mouseDown(const juce::MouseEvent&)
{
    if (isEnabled() && onToggle)
        onToggle();
}

void RhythmSwitch::paint(juce::Graphics& g)
{
    const float alpha = isEnabled() ? 1.0f : 0.45f;
    auto area = getLocalBounds();
    if (hovered_ && isEnabled()) {
        g.setColour(juce::Colour(0xff1b1b23));
        g.fillRoundedRectangle(area.toFloat(), 6.0f);
    }
    const auto switchArea = area.removeFromLeft(cardlook::kSwitchWidth);
    cardlook::drawSwitch(g, switchArea.withSizeKeepingCentre(38, 20).toFloat(), on_, alpha);
    g.setColour(cardlook::kText.withMultipliedAlpha(alpha));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(meaning_, area.reduced(4, 0), juce::Justification::centredLeft, true);
}

//==============================================================================
RhythmControls::RhythmControls(std::function<void(rhythm::Edits)> onEdit)
    : onEdit_(std::move(onEdit))
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
    makeChoice(pattern_, "pattern",
               std::span(rhythm::kPatterns).first(rhythm::kPatterns.size() - 1),
               [](long long n) { return rhythm::Edits { .pattern = n }; });
    makeChoice(kit_, "kit", rhythm::kKits, [](long long n) { return rhythm::Edits { .kit = n }; });
    makeChoice(beat_, "beat", rhythm::kBeats,
               [](long long n) { return rhythm::Edits { .beat = n }; });
    makeChoice(variation_, "variation", rhythm::kVariations,
               [](long long n) { return rhythm::Edits { .variation = n }; });

    makeNumber(level_, "level", "0123456789", [this] { return values_.level; },
               [](long long n) { return rhythm::Edits { .level = n }; });
    makeNumber(reverb_, "reverb", "0123456789", [this] { return values_.reverb; },
               [](long long n) { return rhythm::Edits { .reverb = n }; });
    makeNumber(toneLow_, "toneLow", "-0123456789", [this] { return values_.toneLow; },
               [](long long n) { return rhythm::Edits { .toneLow = n }; });
    makeNumber(toneHigh_, "toneHigh", "-0123456789", [this] { return values_.toneHigh; },
               [](long long n) { return rhythm::Edits { .toneHigh = n }; });

    refresh();
}

void RhythmControls::makeCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(10.0f));
    label.setColour(juce::Label::textColourId, cardlook::kCaption);
    addAndMakeVisible(label);
}

void RhythmControls::makeChoice(juce::ComboBox& box, const char* id,
                                std::span<const rhythm::Choice> list,
                                std::function<rhythm::Edits(long long)> makeEdit)
{
    box.setComponentID(id);
    // Item ids are the manual's numbers plus one: JUCE keeps 0 for "nothing".
    for (const rhythm::Choice& choice : list)
        box.addItem(name(choice.name), static_cast<int>(choice.number) + 1);
    box.setColour(juce::ComboBox::backgroundColourId, cardlook::kField);
    box.setColour(juce::ComboBox::textColourId, cardlook::kText);
    box.setColour(juce::ComboBox::outlineColourId, cardlook::kOutline);
    box.setColour(juce::ComboBox::arrowColourId, cardlook::kCaption);
    box.setColour(juce::ComboBox::focusedOutlineColourId, felitronics::appkit::brand::violet);
    // A lamp reports nothing, whoever set it: a locked BEAT stays locked
    // even for a caller that reaches the box past its greyed face.
    // A control nobody can act on reports nothing AND keeps nothing: a
    // dropdown left open when a job starts still commits its pick, so a
    // greyed box that was moved snaps back to the memory's own value
    // instead of naming a groove the memory does not have.
    box.onChange = [this, &box, makeEdit] {
        if (!box.isEnabled()) {
            refresh();
            return;
        }
        const int selected = box.getSelectedId();
        if (selected > 0 && onEdit_)
            onEdit_(makeEdit(selected - 1));
    };
    addAndMakeVisible(box);
}

void RhythmControls::makeNumber(juce::TextEditor& editor, const char* id,
                                const juce::String& allowed, std::function<long long()> current,
                                std::function<rhythm::Edits(long long)> makeEdit)
{
    editor.setComponentID(id);
    cardlook::styleFieldEditor(editor);
    editor.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                     juce::Font::plain));
    editor.setInputRestrictions(3, allowed); // "200", "-10": the widest values
    editor.setJustification(juce::Justification::centredRight);
    editor.onReturnKey = [this, &editor, current, makeEdit] {
        commitNumber(editor, current, makeEdit);
    };
    editor.onFocusLost = [this, &editor, current, makeEdit] {
        commitNumber(editor, current, makeEdit);
    };
    editor.onEscapeKey = [this] { refresh(); };
    addAndMakeVisible(editor);
}

// A typed number becomes an edit only when it differs from what the slot
// has now; an empty or unchanged field is nothing. Range enforcement is the
// core's job — its typed error reaches the banner in the manual's words.
void RhythmControls::commitNumber(juce::TextEditor& editor,
                                  const std::function<long long()>& current,
                                  const std::function<rhythm::Edits(long long)>& makeEdit)
{
    if (!isEnabled()) { // the same rule as the choices: refuse, and show the truth
        refresh();
        return;
    }
    const juce::String text = editor.getText().trim();
    if (text.isEmpty() || text == "-" || !onEdit_)
        return;
    const long long value = text.getLargeIntValue();
    if (value == current())
        return;
    onEdit_(makeEdit(value));
}

void RhythmControls::setValues(const rhythm::Values& values)
{
    values_ = values;
    refresh();
}

void RhythmControls::refresh()
{
    // PATTERN is an index into the current BEAT's list, and only the 4/4
    // list is charted (Rhythm.hpp, #149): elsewhere the box shows the number
    // the card holds — as text, with no item behind it — and is a lamp, in
    // the shape BEAT takes under a take. A 4/4 name for a 6/4 number is the
    // one thing this tab must never show. The caption never throws: a memory
    // can hold a beat the manual's list lacks, and this runs on every snapshot.
    if (values_.patternCharted)
        pattern_.setSelectedId(static_cast<int>(values_.pattern) + 1, juce::dontSendNotification);
    else
        pattern_.setText(juce::String(values_.pattern), juce::dontSendNotification);
    pattern_.setEnabled(isEnabled() && values_.patternCharted);
    patternCaption_.setText(values_.patternCharted ? juce::String("PATTERN")
                                                   : words::patternCaptionLocked(values_.beat),
                            juce::dontSendNotification);
    kit_.setSelectedId(static_cast<int>(values_.kit) + 1, juce::dontSendNotification);
    beat_.setSelectedId(static_cast<int>(values_.beat) + 1, juce::dontSendNotification);
    variation_.setSelectedId(static_cast<int>(values_.variation) + 1, juce::dontSendNotification);
    // The manual's rule, said where it applies: a recorded take fixes the
    // beat. And ours (Rhythm.hpp, #149): while the count-in borrows the
    // section at 4/4, BEAT stays — the way out is on the pane's line.
    beat_.setEnabled(isEnabled() && !values_.beatLocked && !values_.beatHeldByCountIn);
    beatCaption_.setText(values_.beatLocked         ? juce::String("BEAT (fixed by the take)")
                         : values_.beatHeldByCountIn ? words::beatCaptionHeld()
                                                     : juce::String("BEAT"),
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

// Controls nobody can act on still show what the slot holds: going grey (or
// coming back) puts the memory's own values on them. A dropdown left open
// when a job starts still commits its pick — the edit is refused, as it must
// be, but without this the tab would go on naming a groove the memory does
// not have until the job ends.
void RhythmControls::enablementChanged()
{
    for (auto* control : std::initializer_list<juce::Component*> { &kit_, &variation_, &level_,
                                                                   &reverb_, &toneLow_,
                                                                   &toneHigh_ })
        control->setEnabled(isEnabled());
    refresh(); // also re-applies BEAT's own lock, and PATTERN's at an uncharted beat
}

void RhythmControls::resized()
{
    // Two rows of four, the RHYTHM screen's order read left to right: the
    // choices above, the numbers below.
    auto area = getLocalBounds();
    const int cellWidth = (area.getWidth() - 3 * kCellGap) / 4;

    auto row = area.removeFromTop(kRowHeight);
    const auto cell = [&row, cellWidth](juce::Label& caption, juce::Component& control,
                                        int width) {
        auto c = row.removeFromLeft(cellWidth);
        row.removeFromLeft(kCellGap);
        caption.setBounds(c.removeFromTop(14));
        control.setBounds(c.withHeight(24).withWidth(width));
    };
    cell(patternCaption_, pattern_, cellWidth);
    cell(kitCaption_, kit_, cellWidth);
    cell(beatCaption_, beat_, cellWidth);
    cell(variationCaption_, variation_, cellWidth);

    area.removeFromTop(kRowGap);
    row = area.removeFromTop(kRowHeight);
    // Numbers are short: the field is as wide as its widest value.
    cell(levelCaption_, level_, kNumberWidth);
    cell(reverbCaption_, reverb_, kNumberWidth);
    cell(toneLowCaption_, toneLow_, kNumberWidth);
    cell(toneHighCaption_, toneHigh_, kNumberWidth);
}

//==============================================================================
RhythmPane::RhythmPane()
    : controls_([this](rhythm::Edits edits) {
          if (hasSlot_ && !busy_ && onEdit && allowed().rhythm)
              onEdit(info_.slot, std::move(edits));
      })
{
    switch_.onToggle = [this] {
        if (hasSlot_ && !busy_ && onEdit && allowed().rhythm)
            onEdit(info_.slot, rhythm::Edits { .on = !info_.rhythm.on });
    };

    cost_.setFont(juce::FontOptions(11.5f));
    cost_.setColour(juce::Label::textColourId, felitronics::appkit::brand::orange);
    cost_.setJustificationType(juce::Justification::centredLeft);

    footer_.setFont(juce::FontOptions(11.0f));
    footer_.setColour(juce::Label::textColourId, kDim);
    footer_.setJustificationType(juce::Justification::centredRight);

    for (auto* child : std::initializer_list<juce::Component*> { &switch_, &cost_, &footer_,
                                                                &controls_ })
        addAndMakeVisible(child);

    setSlot(nullptr);
}

void RhythmPane::setSlot(const SlotRow* row)
{
    hasSlot_ = row != nullptr;
    info_ = hasSlot_ ? row->info : catalog::SlotInfo {};
    refresh();
}

void RhythmPane::setBusy(bool busy)
{
    if (busy_ == busy)
        return;
    busy_ = busy;
    refresh();
}

void RhythmPane::refresh()
{
    // What the card may be asked decides what the tab offers: on a card this
    // app only reads, the switch and every field are lamps, and the footer
    // says so instead of promising that a change will be heard.
    const bool live = hasSlot_ && !busy_ && allowed().rhythm;
    switch_.setEnabled(live);
    controls_.setEnabled(live);
    footer_.setText(allowed().anyWrite() ? juce::String("Disconnect to hear the changes.")
                                         : juce::String(CardPermissions::writesOnlyTo()),
                    juce::dontSendNotification);

    for (auto* child : std::initializer_list<juce::Component*> { &switch_, &cost_, &footer_,
                                                                &controls_ })
        child->setVisible(hasSlot_);

    if (!hasSlot_) {
        repaint();
        return;
    }

    // The sentence is the switch's alone: the groove itself is on the
    // controls right under it, and a line that repeated them would only be
    // cut short at the width the header has.
    const rhythm::Values& v = info_.rhythm;
    switch_.setState(v.on, v.on ? "Drums play with this memory."
                                : "No drums with this memory.");
    // A click the core would refuse is not offered: at an uncharted beat the
    // two clicks that write a PATTERN number make the switch a lamp, and the
    // line beside it says why in the player's words (RefusalWords.h) — the
    // same typed refusal the core would throw, so the tab and a banner can
    // never disagree about when (Rhythm.hpp, switchRefusal).
    const auto refused = rhythm::switchRefusal(v, info_.countIn, !v.on);
    if (refused)
        switch_.setEnabled(false);
    // Otherwise the BEAT that stays while the count-in borrows the section,
    // or the groove that "off" would forget: only with a count-in in front
    // of playing drums does off mean Blank rather than State off
    // (Rhythm.hpp). That click is refused at an uncharted beat, so the name
    // is looked up only where the list is charted.
    cost_.setText(refused ? words::rhythmSwitchRefused(*refused, v.beat)
                  : v.beatHeldByCountIn ? words::beatHeldByCountIn()
                  : v.on && info_.countIn
                      ? "Switching it off keeps the count-in and forgets "
                            + name(rhythm::patternName(v.pattern)) + "."
                      : juce::String(),
                  juce::dontSendNotification);
    controls_.setValues(v);
    repaint();
}

void RhythmPane::paint(juce::Graphics& g)
{
    g.fillAll(kPaneBackground);

    if (!hasSlot_) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("Select a slot to set up its rhythm", getLocalBounds(),
                   juce::Justification::centred);
        return;
    }

    // The slot's number leads the row, the way the pedal's display names it.
    auto header = getLocalBounds().reduced(kPad, 0).withHeight(kHeaderHeight).withTrimmedTop(6);
    g.setColour(felitronics::appkit::brand::lilac);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText("SLOT " + juce::String(info_.slot).paddedLeft('0', 2),
               header.removeFromLeft(76), juce::Justification::centredLeft);
}

void RhythmPane::resized()
{
    // The switch and its sentence on the header row beside the slot number,
    // the cost in orange after it, the footer at the right; under them the
    // eight fields in two rows. One tab, so the table never moves.
    auto area = getLocalBounds().reduced(kPad, 6);

    auto header = area.removeFromTop(kHeaderHeight);
    header.removeFromLeft(76); // the painted "SLOT nn"
    footer_.setBounds(header.removeFromRight(juce::jmin(260, header.getWidth() / 3)));
    switch_.setBounds(header.removeFromLeft(juce::jmin(360, header.getWidth() / 2)));
    header.removeFromLeft(8);
    cost_.setBounds(header);

    area.removeFromTop(8);
    controls_.setBounds(area.removeFromTop(juce::jmin(RhythmControls::kHeight, area.getHeight())));
}

} // namespace loopercat
