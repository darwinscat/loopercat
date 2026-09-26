// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "UseCaseCard.h"

#include <loopercat/usecases/Rhythm.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

//==============================================================================
// loopercat::RhythmCard — will drums play with this memory, and which.
//
// The card is the pedal's RHYTHM screen as one line: a switch, and what the
// drums would be (Rock1 · Studio · 4/4 · A). The switch is the switch; the
// rest of the card opens the studio for it — a callout with the eight fields
// of the RHYTHM screen, one control each, every change one job — because the
// strip under the table is one line tall and a groove has more than one
// word in it.
//
// Like every card, it owns no memory bytes: it reports an Edits (Rhythm.hpp)
// and the owner turns it into a job. A snapshot landing while the studio is
// open refreshes it, except a field being typed in.
//==============================================================================
namespace loopercat
{

// The studio: the RHYTHM screen's fields, editable. Lives inside a callout,
// owned by it; the card keeps a SafePointer for refreshes.
class RhythmOptions final : public juce::Component
{
public:
    RhythmOptions(const usecases::rhythm::Values& values,
                  std::function<void(usecases::rhythm::Edits)> onEdit);

    void setValues(const usecases::rhythm::Values& values);
    void enablementChanged() override;
    void resized() override;
    void paint(juce::Graphics&) override;

    static constexpr int kWidth = 500;
    static constexpr int kHeight = 118;

private:
    void refresh();
    void commitNumber(juce::TextEditor& editor, long long current,
                      std::function<usecases::rhythm::Edits(long long)> makeEdit);
    void makeCaption(juce::Label& label, const juce::String& text);
    void makeChoice(juce::ComboBox& box, std::span<const usecases::rhythm::Choice> list,
                    std::function<usecases::rhythm::Edits(long long)> makeEdit);
    void makeNumber(juce::TextEditor& editor, int cells, const juce::String& allowed,
                    std::function<usecases::rhythm::Edits(long long)> makeEdit);

    usecases::rhythm::Values values_;
    std::function<void(usecases::rhythm::Edits)> onEdit_;

    juce::Label patternCaption_, kitCaption_, beatCaption_, variationCaption_, levelCaption_,
        reverbCaption_, toneLowCaption_, toneHighCaption_;
    juce::ComboBox pattern_, kit_, beat_, variation_;
    juce::TextEditor level_, reverb_, toneLow_, toneHigh_;
    // The editors' commit closures read the current value at commit time, so
    // a snapshot that lands mid-edit compares against what the slot has NOW.
    std::function<long long()> levelNow_, reverbNow_, toneLowNow_, toneHighNow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RhythmOptions)
};

class RhythmCard final : public juce::Component
{
public:
    RhythmCard();

    std::function<void(usecases::rhythm::Edits)> onEdit;

    // `patternLost`: the groove that switching OFF would forget (with a
    // count-in in front of it, off means Blank) — the card's one cost line.
    void setValues(const usecases::rhythm::Values& values, std::optional<long long> patternLost);

    // Height follows content: the cost line only exists when there is a cost.
    int preferredHeight() const { return patternLost_ ? 78 : 58; }

    void enablementChanged() override;
    void visibilityChanged() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseEnter(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void paint(juce::Graphics&) override;

private:
    void openStudio();
    juce::String summary() const;

    usecases::rhythm::Values values_ {};
    std::optional<long long> patternLost_;
    bool hovered_ = false;
    juce::Component::SafePointer<RhythmOptions> studio_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RhythmCard)
};

} // namespace loopercat
