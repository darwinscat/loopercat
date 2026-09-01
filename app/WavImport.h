// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <optional>

//==============================================================================
// loopercat::wavimport — make a user's audio file pedal-acceptable (issue
// #20). The pedal wants 44.1 kHz stereo WAV in 16/24-bit PCM or 32-bit
// float; DAW exports come as anything — WAVE_FORMAT_EXTENSIBLE headers,
// 48 kHz, mono, AIFF. A file the pedal already accepts passes through
// UNTOUCHED (the byte-surgery invariant extends to uploads); everything
// else JUCE can read is rewritten as 44.1 kHz stereo float32 — the pedal's
// own native recording format — into a temp file the caller owns.
//
// The one exception is opt-in (issue #53): with a normalization target set,
// loudness is measured per ITU-R BS.1770-4 over the exact post-resample
// stereo stream headed for the card, and one constant gain is baked into the
// samples so every upload lands at the same perceived loudness. ON means
// even a pedal-ready file is rewritten when its loudness is off target —
// deliberately trading the byte-exact pass-through for a level setlist, as
// the user's explicit choice.
//==============================================================================
namespace loopercat::wavimport
{

struct Options {
    // Integrated-loudness target in LUFS (the app defaults to -18, ReplayGain
    // 2.0's reference — the modern spelling of mp3gain's "89 dB"). Empty =
    // normalization off, today's byte-exact behavior.
    std::optional<double> normalizeTargetLufs;
};

// A pedal-ready source already within loudness::kAlreadyAtTargetLu of the
// target passes through byte-exact instead of being rewritten for a fraction
// nobody can hear — re-importing an already-normalized file stays a no-op.

// What normalization did to this import — reported so the toast and the
// operations log can say it, not so callers can second-guess it.
struct NormalizeOutcome {
    bool measurable = false;   // false: silence, or shorter than one 400 ms
                               // gating block — imported with no gain applied
    bool untouched = false;    // pedal-ready and already at target: byte-exact
    bool cappedByPeak = false; // the boost stopped at the -1 dB sample-peak
                               // ceiling, short of target — loud peaks, quiet body
    double measuredLufs = 0;   // valid when measurable
    double gainDb = 0;         // gain actually baked in (0 when untouched/unmeasurable)
    bool damaged = false;      // bytes that are not audio were found: imported
                               // with no gain — no "loudness" of garbage is a target
    std::int64_t wildSamples = 0; // how many, for the report
};

struct Prepared {
    juce::File file;        // hand this to commands::push
    bool converted = false; // true: `file` is a temp conversion — delete it after the push
    std::optional<NormalizeOutcome> normalize; // engaged iff Options asked for it
};

// Fails when the source is not audio JUCE can read, or has more than two
// channels — a surround downmix is a creative decision, not a default.
juce::Result prepare(const juce::File& source, const juce::File& tempDir, Prepared& out,
                     const Options& options);

} // namespace loopercat::wavimport
