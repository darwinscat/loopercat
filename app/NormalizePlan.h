// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Commands.hpp>
#include <loopercat/Loudness.hpp>
#include <loopercat/Normalize.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

//==============================================================================
// loopercat::normalizeplan — what commands::normalize would do with a take
// that measures like this, decided from the reading and the target alone
// (#142). The single-slot action asks here before it opens its window: a
// take the command would leave alone is a toast and no window; one it would
// refuse is refused in the command's own words; one it would rewrite opens
// the window with the numbers in hand.
//
// The rule is the command's, piece by piece — its target window, its two
// refusals, loudness::kAlreadyAtTargetLu, loudness::normalizeGainDb under
// loudness::kPeakCeilingDb, loudness::kNoGainDb — so a case the command
// treats as "write nothing" can never open the window, and the sentence a
// refusal shows is the one the job would have failed with. It decides
// nothing FOR the command: that one measures the card's bytes again before
// it writes, whatever this said of them (a take replaced in between is
// caught there).
//
// Pure and JUCE-free, so it is tested without a window
// (tests/normalize_plan_tests.cpp).
//==============================================================================
namespace loopercat::normalizeplan
{

struct Plan {
    enum class Outcome {
        apply,       // the command would bake gainDb in: ask first
        nothingToDo, // the command would write nothing and say why: tell, do not ask
        refuse       // the command would throw: `words` is its sentence
    };
    Outcome outcome;
    double measuredLufs; // the reading's integrated loudness; 0 when refused
    double gainDb;       // apply: the gain the command would bake in; 0 otherwise
    bool cappedByPeak;   // the ceiling takes part of the boost (apply) or all of it (nothingToDo)
    // apply: the window's first line — "Measured -22.8 LUFS, this adds +4.7 dB";
    // refuse: the command's refusal, word for word; nothingToDo: empty — the
    // reading's own words (MainComponent::describeReading) say it.
    std::string words;
};

namespace detail {

    // The one decimal every loudness figure in the app is shown with.
    inline std::string oneDecimal(double value)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f", value);
        return text;
    }

    // The ceiling as the command's own refusal names it: "-1 dBTP".
    inline std::string shortest(double value)
    {
        char text[32];
        std::snprintf(text, sizeof text, "%g", value);
        return text;
    }

} // namespace detail

inline Plan decide(int slot, const wav::LoudnessReading& reading, double targetLufs)
{
    commands::requireNormalizeTarget(targetLufs);
    // Garbage first, as the command has it: no number is computed from bytes
    // that are not audio, whatever else the reading says.
    if (reading.wildSamples > 0)
        return { Plan::Outcome::refuse, 0.0, 0.0, false,
                 commands::normalizeDamagedRefusal(slot, reading.wildSamples) };
    if (!reading.integratedLufs.has_value())
        return { Plan::Outcome::refuse, 0.0, 0.0, false,
                 commands::normalizeUnmeasurableRefusal(slot) };

    const double measured = *reading.integratedLufs;
    const double wanted = targetLufs - measured;
    if (std::abs(wanted) < loudness::kAlreadyAtTargetLu)
        return { Plan::Outcome::nothingToDo, measured, 0.0, false, {} };
    const double gainDb = loudness::normalizeGainDb(measured, targetLufs, reading.truePeakDb,
                                                    loudness::kPeakCeilingDb);
    const bool capped = wanted > 0.0 && gainDb + loudness::kNoGainDb < wanted;
    if (std::abs(gainDb) < loudness::kNoGainDb)
        return { Plan::Outcome::nothingToDo, measured, 0.0, capped, {} };

    std::string words = "Measured " + detail::oneDecimal(measured) + " LUFS, ";
    if (capped)
        words += "+" + detail::oneDecimal(gainDb) + " dB possible: the "
               + detail::shortest(loudness::kPeakCeilingDb) + " dBTP ceiling stops the rest";
    else if (gainDb > 0.0)
        words += "this adds +" + detail::oneDecimal(gainDb) + " dB";
    else
        words += "this cuts " + detail::oneDecimal(-gainDb) + " dB";
    return { Plan::Outcome::apply, measured, gainDb, capped, std::move(words) };
}

} // namespace loopercat::normalizeplan
