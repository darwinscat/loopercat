// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TargetLufs.h"

#include <loopercat/Loudness.hpp>
#include <loopercat/Normalize.hpp>

#include <juce_core/juce_core.h>

#include <cmath>
#include <optional>

//==============================================================================
// loopercat::loudnessreport — one reading, two audiences: the inspector wants
// the sentence, the column wants the number — and both want to be told when
// the number is not one.
//
// The verdict ("Normalize would change this") is made against the target in
// Settings with the command's own gain rule. With no usable target — the
// stored one is not a target (#142 review) — there is no verdict: the
// number, the peak, and nothing coloured. A damaged take is reported as
// damaged either way; that is a fact about the bytes, not about a target.
//
// JUCE-core only, so it is tested without a window
// (tests/normalize_step_tests.cpp).
//==============================================================================
namespace loopercat::loudnessreport
{

struct Report {
    juce::String cellText;    // the column: "-22.8", "damaged", "n/a"
    juce::String rowText;     // the player row: "-22.8 LUFS · 4.8 dB below target -18"
    juce::String noteText;    // the toast: the row text plus the peak
    juce::String tooltipText; // the hint: what the tight row cannot say
    bool attention = false;   // Normalize would change this — drawn to be noticed
    bool damaged = false;
};

inline Report describe(const wav::LoudnessReading& reading, std::optional<double> targetLufs)
{
    if (reading.wildSamples > 0) {
        // The number the meter would print here is real — and meaningless:
        // 2.4e38 is not a loudness, it is a foreign header read as float
        // (the 2026-09-02 recovered card). The row gets a sign and a word;
        // the hint gets the story.
        const juce::String what = "damaged audio: " + juce::String(reading.wildSamples)
                                + " impossible sample value(s)";
        return { "damaged", "damaged audio", what,
                 "This file contains bytes that are not sound. Re-push the loop from its "
                 "original; Normalize will not touch it.",
                 true, true };
    }
    if (!reading.integratedLufs.has_value())
        return { "n/a", "silent or too short to measure", "silent or too short to measure",
                 "Nothing to measure: silence, or under 400 ms of audio.", false, false };
    const double lufs = *reading.integratedLufs;
    const juce::String peak = juce::String(reading.truePeakDb, 1) + " dBTP";
    if (!targetLufs.has_value()) {
        // Nothing to measure it against: the number, and no verdict at all.
        const juce::String row = juce::String(lufs, 1) + " LUFS";
        return { juce::String(lufs, 1), row, row + ", peak " + peak,
                 "Peak " + peak
                     + juce::String::fromUTF8(" \xc2\xb7 no usable target: set one in Settings "
                                              "\xe2\x86\x92 Import."),
                 false, false };
    }
    const juce::String target = targetlufs::format(*targetLufs);
    const double wanted = *targetLufs - lufs;
    const bool offTarget = std::abs(wanted) >= loudness::kAlreadyAtTargetLu;
    // Attention means "Normalize would change this" — so the readout runs
    // the command's own gain rule. A quiet loop whose peaks already touch the
    // ceiling is off target and yet has nothing to gain (field report: a slot
    // painted orange that the command then rightly refused); it reads grey
    // with the reason, and a partial boost says how much is actually there.
    const double gain = !offTarget ? 0.0
                      : std::isfinite(reading.truePeakDb)
                          ? loudness::normalizeGainDb(lufs, *targetLufs, reading.truePeakDb,
                                                      loudness::kPeakCeilingDb)
                          : wanted;
    // The command's own lines (#142 review): a boost the ceiling leaves
    // under kSmallestGainDb is nothing to gain, and colours nothing.
    const bool wouldChange = offTarget && std::abs(gain) >= loudness::kSmallestGainDb;
    const bool capped = wanted > 0.0 && gain < wanted;
    juce::String row = juce::String(lufs, 1) + juce::String::fromUTF8(" LUFS \xc2\xb7 ");
    if (!offTarget)
        row << "at target " << target;
    else {
        row << juce::String(std::abs(wanted), 1) << " dB " << (wanted > 0.0 ? "below" : "above")
            << " target " << target;
        if (!wouldChange)
            row << juce::String::fromUTF8(" \xc2\xb7 peak-limited, nothing to gain");
        else if (capped)
            row << juce::String::fromUTF8(" \xc2\xb7 only +") << juce::String(gain, 1)
                << " dB possible";
    }
    juce::String tip = "Peak " + peak + juce::String::fromUTF8(" \xc2\xb7 target ") + target + " LUFS.";
    if (offTarget && !wouldChange)
        tip << " Cannot be raised without clipping.";
    else if (capped)
        tip << " Only +" << juce::String(gain, 1) << " dB fits without clipping.";
    return { juce::String(lufs, 1), row, row + ", peak " + peak, tip, wouldChange, false };
}

} // namespace loopercat::loudnessreport
