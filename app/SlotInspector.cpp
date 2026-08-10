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
    tempoEditor_.setInputRestrictions(5, "0123456789."); // "300.0" is the widest legal value
    tempoEditor_.setJustification(juce::Justification::centredRight);
    tempoEditor_.onReturnKey = [this] { commitTempo(); };
    tempoEditor_.onFocusLost = [this] { commitTempo(); };
    tempoEditor_.onEscapeKey = [this] { refresh(); };
    tempoEditor_.onTextChange = [this] { updateBarsHint(); };

    barsHint_.setFont(juce::FontOptions(11.5f));
    barsHint_.setColour(juce::Label::textColourId, kDim);

    footer_.setFont(juce::FontOptions(11.0f));
    footer_.setColour(juce::Label::textColourId, kDim);
    footer_.setJustificationType(juce::Justification::topLeft);
    footer_.setText(juce::String::fromUTF8(
                        "Changes land in the pedal's memory file \xe2\x80\x94 eject the volume "
                        "and reboot the pedal to hear them."),
                    juce::dontSendNotification);

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
    auto area = getLocalBounds().toFloat();
    g.setColour(kPaneBackground);
    g.fillRoundedRectangle(area, 8.0f);
    g.setColour(juce::Colour(0xff1e1e26));
    g.drawRoundedRectangle(area.reduced(0.5f), 8.0f, 1.0f);

    if (!hasSlot_) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText("Select a slot to set it up", getLocalBounds(), juce::Justification::centred);
        return;
    }

    // The slot's number is the title: it is what the pedal's display shows.
    auto header = getLocalBounds().reduced(kPad, 0).withHeight(38).withTrimmedTop(12);
    g.setColour(felitronics::appkit::brand::lilac);
    g.setFont(juce::FontOptions(15.0f));
    g.drawText("SLOT " + juce::String(info_.slot).paddedLeft('0', 2), header,
               juce::Justification::topLeft);

    if (info_.hasAudio) {
        // Bars only when the pedal has counted them: a slot it has not
        // indexed yet says nothing rather than "0 bars".
        juce::String facts = SlotTable::formatDuration(info_.frames);
        if (info_.measures > 0)
            facts << juce::String::fromUTF8(" \xc2\xb7 ") << info_.measures << " bars";
        g.setColour(kDim);
        g.setFont(juce::FontOptions(11.5f));
        g.drawText(facts, header, juce::Justification::topRight);
    }

    g.setColour(juce::Colour(0xff1e1e26)); // the rule between identity and behaviour
    const int y = 132;
    g.fillRect(kPad, y, getWidth() - 2 * kPad, 1);
}

void SlotInspector::resized()
{
    auto area = getLocalBounds().reduced(kPad, 0);
    area.removeFromTop(50); // the painted header

    nameCaption_.setBounds(area.removeFromTop(14));
    nameEditor_.setBounds(area.removeFromTop(26));
    area.removeFromTop(10);

    tempoCaption_.setBounds(area.removeFromTop(14));
    auto tempoFieldRow = area.removeFromTop(26);
    tempoEditor_.setBounds(tempoFieldRow.removeFromLeft(78));
    tempoFieldRow.removeFromLeft(10);
    barsHint_.setBounds(tempoFieldRow); // the consequence sits beside the field

    area.removeFromTop(22); // clears the rule painted at y = 132

    countIn_.setBounds(area.removeFromTop(countIn_.preferredHeight()));
    area.removeFromTop(8);
    oneShot_.setBounds(area.removeFromTop(oneShot_.preferredHeight()));

    footer_.setBounds(getLocalBounds().reduced(kPad, 0).withTop(getHeight() - 46).withHeight(40));
}

} // namespace loopercat
