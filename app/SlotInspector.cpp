// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SlotInspector.h"

#include "SlotTable.h"
#include "Strings.h"

#include <felitronics/appkit/Brand.h>

#include <loopercat/Commands.hpp>

#include <cmath>

namespace loopercat
{

namespace
{
    const juce::Colour kPaneBackground { 0xff0e0e13 };
    const juce::Colour kText { 0xffd8d8d8 };
    const juce::Colour kDim { 0xff63636d };
    const juce::Colour kCaption { 0xff8a8a92 };

    constexpr int kPad = 14;
    constexpr int kTempoDigits = 5; // "300.0", the pedal's widest tempo

    void styleEditor(juce::TextEditor& editor)
    {
        editor.setFont(juce::FontOptions(13.0f));
        editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff17171d));
        editor.setColour(juce::TextEditor::textColourId, kText);
        editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff2a2a34));
        editor.setColour(juce::TextEditor::focusedOutlineColourId,
                         felitronics::appkit::brand::violet);
        editor.setColour(juce::TextEditor::highlightColourId,
                         felitronics::appkit::brand::violet.withAlpha(0.4f));
    }
}

SlotInspector::SlotInspector()
{
    makeCaption(nameCaption_, "NAME");
    makeCaption(tempoCaption_, "TEMPO");

    styleEditor(nameEditor_);
    // The pedal's name is twelve fixed cells on a small display. A monospaced
    // field exactly twelve glyphs wide says that without a word of help — you
    // can see the room you have left.
    nameEditor_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                          juce::Font::plain));
    // The pedal's own constraints, enforced at the field: 12 characters from
    // the printable ASCII its display can show.
    juce::String printableAscii;
    for (juce::juce_wchar c = 0x20; c <= 0x7e; ++c)
        printableAscii += juce::String::charToString(c);
    nameEditor_.setInputRestrictions(rc0::kNameLength, printableAscii);
    nameEditor_.onReturnKey = [this] { commitName(); };
    nameEditor_.onFocusLost = [this] { commitName(); };
    nameEditor_.onEscapeKey = [this] { refresh(); };

    styleEditor(tempoEditor_);
    tempoEditor_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                           juce::Font::plain));
    tempoEditor_.setInputRestrictions(kTempoDigits, "0123456789."); // "300.0" is the widest value
    tempoEditor_.setJustification(juce::Justification::centredRight);
    tempoEditor_.onReturnKey = [this] { commitTempo(); };
    tempoEditor_.onFocusLost = [this] { commitTempo(); };
    tempoEditor_.onEscapeKey = [this] { refresh(); };
    tempoEditor_.onTextChange = [this] { updateBarsHint(); };

    barsHint_.setFont(juce::FontOptions(11.5f));
    barsHint_.setColour(juce::Label::textColourId, kDim);

    footer_.setFont(juce::FontOptions(11.0f));
    footer_.setColour(juce::Label::textColourId, kDim);
    footer_.setJustificationType(juce::Justification::centredRight);
    // Disconnect is enough: leaving STORAGE makes the pedal re-read its
    // memory (hardware, 2026-08-11 — a rename and a tempo both landed on the
    // display with no power cycle). No reason to send anyone to the wall plug.
    footer_.setText("Disconnect to hear the changes.", juce::dontSendNotification);

    countIn_.onToggle = [this] {
        if (hasSlot_ && !busy_ && onCountInToggled)
            onCountInToggled(info_.slot);
    };
    oneShot_.onToggle = [this] {
        if (hasSlot_ && !busy_ && onOneShotToggled)
            onOneShotToggled(info_.slot);
    };

    for (auto* child : std::initializer_list<juce::Component*> {
             &nameCaption_, &tempoCaption_, &barsHint_, &footer_, &nameEditor_, &tempoEditor_,
             &countIn_, &oneShot_ })
        addAndMakeVisible(child);

    setSlot(nullptr);
}

void SlotInspector::makeCaption(juce::Label& label, const juce::String& text)
{
    label.setText(text, juce::dontSendNotification);
    label.setFont(juce::FontOptions(10.0f));
    label.setColour(juce::Label::textColourId, kCaption);
}

void SlotInspector::setSlot(const SlotRow* row)
{
    hasSlot_ = row != nullptr;
    info_ = hasSlot_ ? row->info : catalog::SlotInfo {};
    refresh();
}

void SlotInspector::setBusy(bool busy)
{
    if (busy_ == busy)
        return;
    busy_ = busy;
    refresh();
}

void SlotInspector::refresh()
{
    const bool live = hasSlot_ && !busy_;
    for (auto* child : std::initializer_list<juce::Component*> { &nameEditor_, &tempoEditor_,
                                                                &countIn_, &oneShot_ })
        child->setEnabled(live);

    for (auto* child : std::initializer_list<juce::Component*> { &nameCaption_, &tempoCaption_,
                                                                &barsHint_, &footer_,
                                                                &nameEditor_, &tempoEditor_,
                                                                &countIn_, &oneShot_ })
        child->setVisible(hasSlot_);

    if (!hasSlot_) {
        repaint();
        return;
    }

    // A field being typed in is the user's, not ours: a snapshot landing
    // mid-edit must not yank the text out from under them.
    if (!nameEditor_.hasKeyboardFocus(true))
        nameEditor_.setText(utf8(info_.name).trimEnd(), juce::dontSendNotification);
    if (!tempoEditor_.hasKeyboardFocus(true))
        tempoEditor_.setText(SlotTable::formatTempo(info_.tempoTenths), juce::dontSendNotification);
    updateBarsHint();

    countIn_.setState(
        info_.countIn,
        info_.countIn ? "One bar of count at " + SlotTable::formatTempo(info_.tempoTenths)
                            + " BPM, then the loop."
                      : "No count: the loop starts the moment you press play.",
        !info_.countIn && info_.countInTakesPattern
            ? juce::String::fromUTF8("Switching it on replaces a rhythm pattern chosen on the "
                                     "pedal.")
            : juce::String());

    oneShot_.setState(info_.oneShot,
                      info_.oneShot ? "Plays once and stops at the end of the loop."
                                    : "Loops until you stop it.");

    resized(); // the count-in card grows when it has something to warn about
    repaint();
}

// A field as wide as the value the pedal allows and no wider: twelve cells for
// a name, five for "300.0". Monospaced, so the room left is the room shown —
// the limit made visible instead of explained.
int SlotInspector::fieldWidth(const juce::TextEditor& field, int cells)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(field.getFont(), juce::String::repeatedString("0", cells), 0.0f, 0.0f);
    return juce::roundToInt(glyphs.getBoundingBox(0, -1, true).getWidth()) + 14;
}

void SlotInspector::updateBarsHint()
{
    // The tempo is not just a number on this pedal: the bar count follows
    // from it (commands::setTempo writes MeasLen/Measure to match), so the
    // consequence belongs next to the field, before the write.
    const double bpm = tempoEditor_.getText().getDoubleValue();
    const long long tenths = std::llround(bpm * 10.0);
    if (!info_.hasAudio || tenths < commands::kTempoTenthsMin
        || tenths > commands::kTempoTenthsMax) {
        barsHint_.setText(info_.hasAudio ? juce::String("40.0 - 300.0 BPM") : juce::String(),
                          juce::dontSendNotification);
        return;
    }
    const long long bars = commands::barsFromTempo(tenths, info_.frames);
    barsHint_.setText(juce::String::fromUTF8("\xe2\x86\x92 ") + juce::String(bars)
                          + (bars == 1 ? " bar" : " bars"),
                      juce::dontSendNotification);
}

void SlotInspector::commitName()
{
    if (!hasSlot_ || busy_ || !onRenameCommitted)
        return;
    const juce::String value = nameEditor_.getText().trim();
    if (value.isEmpty() || value == utf8(info_.name).trimEnd())
        return;
    onRenameCommitted(info_.slot, value);
}

void SlotInspector::commitTempo()
{
    if (!hasSlot_ || busy_ || !onTempoCommitted)
        return;
    const juce::String value = tempoEditor_.getText().trim();
    if (value.isEmpty() || value == SlotTable::formatTempo(info_.tempoTenths))
        return;
    // Range enforcement is the core's job — its typed error reaches the
    // banner with the pedal's own 40.0-300.0 wording.
    const long long tenths = std::llround(value.getDoubleValue() * 10.0);
    if (tenths > 0)
        onTempoCommitted(info_.slot, tenths);
}

void SlotInspector::paint(juce::Graphics& g)
{
    g.fillAll(kPaneBackground);

    if (!hasSlot_) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("Select a slot to set it up", getLocalBounds(), juce::Justification::centred);
        return;
    }

    // The slot's number leads the row, the way the pedal's display names it.
    auto header = getLocalBounds().reduced(kPad, 0).withHeight(28).withTrimmedTop(6);
    g.setColour(felitronics::appkit::brand::lilac);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText("SLOT " + juce::String(info_.slot).paddedLeft('0', 2),
               header.removeFromLeft(76), juce::Justification::centredLeft);
}

void SlotInspector::resized()
{
    // A strip, not a column: identity on one row, the cards side by side under
    // it, so switching tabs never moves the table.
    auto area = getLocalBounds().reduced(kPad, 6);

    auto identity = area.removeFromTop(28);
    identity.removeFromLeft(76); // the painted "SLOT nn"

    nameCaption_.setBounds(identity.removeFromLeft(44).withTrimmedTop(8));
    const int nameWidth = fieldWidth(nameEditor_, rc0::kNameLength);
    nameEditor_.setBounds(identity.removeFromLeft(nameWidth).withSizeKeepingCentre(nameWidth, 24));
    identity.removeFromLeft(18);
    tempoCaption_.setBounds(identity.removeFromLeft(48).withTrimmedTop(8));
    const int tempoWidth = fieldWidth(tempoEditor_, kTempoDigits);
    tempoEditor_.setBounds(identity.removeFromLeft(tempoWidth).withSizeKeepingCentre(tempoWidth, 24));
    identity.removeFromLeft(8);
    barsHint_.setBounds(identity.removeFromLeft(90));
    footer_.setBounds(identity); // the "disconnect to hear it" note rides the same row

    area.removeFromTop(8);
    const int cardHeight = juce::jmax(countIn_.preferredHeight(), oneShot_.preferredHeight());
    auto cards = area.removeFromTop(juce::jmin(cardHeight, area.getHeight()));
    const int cardWidth = juce::jmin(320, (cards.getWidth() - 10) / 2);
    countIn_.setBounds(cards.removeFromLeft(cardWidth));
    cards.removeFromLeft(10);
    oneShot_.setBounds(cards.removeFromLeft(cardWidth));
}

} // namespace loopercat
