// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Strings.h"

#include <loopercat/usecases/Beat.hpp>
#include <loopercat/usecases/CountIn.hpp>
#include <loopercat/usecases/Rhythm.hpp>

#include <juce_core/juce_core.h>

//==============================================================================
// loopercat::words — the player's one line for a refusal the core states as
// a fact. The core refuses a click that would write a PATTERN number at a
// beat whose list is not charted (Beat.hpp, #149) and says so in full for
// the banner and the log; the Rhythm tab, the Play Count-In card and the
// table say it in one line beside the lamp, before the click, with no date
// and no field name in it. One place for those lines, so the three never
// drift and there is one list to reword.
//==============================================================================
namespace loopercat::words
{

// The beat as the screen prints it ("6/4"), or the truth about a number the
// manual's list does not have — never a throw: these lines are painted on
// every snapshot (Beat.hpp, label).
inline juce::String beatLabel(long long beat) { return utf8(usecases::beat::label(beat)); }

// Beside the rhythm switch, where its click is refused.
inline juce::String rhythmSwitchRefused(usecases::rhythm::SwitchRefusal why, long long beat)
{
    switch (why) {
    case usecases::rhythm::SwitchRefusal::onNeedsGroove:
        return "Switching on at " + beatLabel(beat)
            + " needs a pattern: choose one on the pedal first.";
    case usecases::rhythm::SwitchRefusal::offNeedsBlank:
        return "Switching off at " + beatLabel(beat)
            + " would drop the pattern: switch the count-in off first.";
    }
    jassertfalse;
    return {};
}

// Beside the Play Count-In switch (the card, the table cell's hover), where
// its click is refused.
inline juce::String countInRefused(usecases::countin::Refusal why, long long beat)
{
    switch (why) {
    case usecases::countin::Refusal::onBorrowsSection:
        return "Switching on at " + beatLabel(beat)
            + " would replace the pattern: switch the rhythm on first.";
    case usecases::countin::Refusal::offReturnsSection:
        return "Switching off at " + beatLabel(beat)
            + " would reset the pattern: switch the count-in off on the pedal instead.";
    }
    jassertfalse;
    return {};
}

// The BEAT caption and the line beside the switch while the count-in
// borrows the rhythm section at 4/4 (Rhythm.hpp, beatHeldByCountIn): the
// caption in the shape the take's lock takes, the line saying the way out.
inline juce::String beatCaptionHeld() { return "BEAT (held by the count-in)"; }
inline juce::String beatHeldByCountIn()
{
    return "BEAT stays at 4/4 while the count-in is on: switch the count-in off first.";
}

// The PATTERN caption at a beat whose list is not charted, in the shape BEAT
// takes under a take ("BEAT (fixed by the take)").
inline juce::String patternCaptionLocked(long long beat)
{
    if (const auto named = usecases::beat::nameIfListed(beat))
        return "PATTERN (list at " + utf8(std::string(*named)) + " not charted)";
    return "PATTERN (BEAT " + juce::String(beat) + " is not in the manual's list)";
}

} // namespace loopercat::words
