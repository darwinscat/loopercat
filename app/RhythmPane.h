// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "CardPermissions.h"
#include "PedalWorker.h"
#include "UseCaseCard.h"

#include <loopercat/usecases/Rhythm.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

//==============================================================================
// loopercat::RhythmPane — the Rhythm tab: will drums play with this memory,
// and which. The pedal's RHYTHM screen laid out flat under the table — one
// switch, the groove as a line, and the eight fields as eight controls in
// two rows — because a groove has more than one word in it and a card in the
// Properties strip could only point at a popover (Alisa, 2026-09-26).
//
// Same contract as the inspector: the row comes in, callbacks go out. It
// owns no memory bytes; each change is one Edits (Rhythm.hpp) the owner
// turns into one worker job. A card the table keeps read-only turns every
// control into a lamp and the footer into the sentence about what this app
// writes to (CardPermissions.h).
//==============================================================================
namespace loopercat
{

// The switch with its sentence: one component, so a click lands on it the
// way JUCE lands clicks, and the harness can press it the same way.
class RhythmSwitch final : public juce::Component
{
public:
    RhythmSwitch() { setComponentID("switch"); }

    std::function<void()> onToggle;

    void setState(bool on, juce::String meaning);
    void enablementChanged() override { repaint(); }
    void mouseDown(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = false; repaint(); }
    void paint(juce::Graphics&) override;

    bool isOn() const { return on_; }

private:
    bool on_ = false;
    bool hovered_ = false;
    juce::String meaning_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RhythmSwitch)
};

// The RHYTHM screen's eight fields, one control each, in the screen's order:
// the choices above, the numbers below. Every control carries its field's
// name as its component id (pattern, kit, beat, variation, level, reverb,
// toneLow, toneHigh). Two of them lock themselves from the memory's own
// facts, caption saying why: BEAT under a recorded take, PATTERN at a beat
// whose list is not charted (Rhythm.hpp) — there it shows the number.
class RhythmControls final : public juce::Component
{
public:
    explicit RhythmControls(std::function<void(usecases::rhythm::Edits)> onEdit);

    // A snapshot landed: refresh what is not being typed in.
    void setValues(const usecases::rhythm::Values& values);
    void enablementChanged() override;
    void resized() override;

    static constexpr int kRowHeight = 14 + 24; // a caption over a control
    static constexpr int kRowGap = 10;
    static constexpr int kHeight = 2 * kRowHeight + kRowGap;

private:
    void refresh();
    void commitNumber(juce::TextEditor& editor, const std::function<long long()>& current,
                      const std::function<usecases::rhythm::Edits(long long)>& makeEdit);
    void makeCaption(juce::Label& label, const juce::String& text);
    void makeChoice(juce::ComboBox& box, const char* id, std::span<const usecases::rhythm::Choice> list,
                    std::function<usecases::rhythm::Edits(long long)> makeEdit);
    void makeNumber(juce::TextEditor& editor, const char* id, const juce::String& allowed,
                    std::function<long long()> current,
                    std::function<usecases::rhythm::Edits(long long)> makeEdit);

    usecases::rhythm::Values values_ {};
    std::function<void(usecases::rhythm::Edits)> onEdit_;

    juce::Label patternCaption_, kitCaption_, beatCaption_, variationCaption_, levelCaption_,
        reverbCaption_, toneLowCaption_, toneHighCaption_;
    juce::ComboBox pattern_, kit_, beat_, variation_;
    juce::TextEditor level_, reverb_, toneLow_, toneHigh_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RhythmControls)
};

class RhythmPane final : public juce::Component
{
public:
    RhythmPane();

    // The current selection, or nullptr when nothing is selected. Safe to
    // call on every snapshot: a field the user is typing in is left alone.
    void setSlot(const SlotRow* row);
    void setBusy(bool busy); // a worker job is running — the tab waits

    std::function<void(int, usecases::rhythm::Edits)> onEdit;

    // What the card may be asked (CardPermissions.h), from the owner. Unset,
    // nothing is writable: a tab nobody told what it may do offers nothing.
    std::function<CardPermissions()> permissions;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void refresh();
    CardPermissions allowed() const { return permissions ? permissions() : CardPermissions {}; }

    bool hasSlot_ = false;
    catalog::SlotInfo info_ {};
    bool busy_ = false;

    RhythmSwitch switch_;
    juce::Label cost_, footer_;
    RhythmControls controls_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RhythmPane)
};

} // namespace loopercat
