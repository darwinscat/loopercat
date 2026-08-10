// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

//==============================================================================
// loopercat::wavimport — make a user's audio file pedal-acceptable (issue
// #20). The pedal wants 44.1 kHz stereo WAV in 16/24-bit PCM or 32-bit
// float; DAW exports come as anything — WAVE_FORMAT_EXTENSIBLE headers,
// 48 kHz, mono, AIFF. A file the pedal already accepts passes through
// UNTOUCHED (the byte-surgery invariant extends to uploads); everything
// else JUCE can read is rewritten as 44.1 kHz stereo float32 — the pedal's
// own native recording format — into a temp file the caller owns.
//==============================================================================
namespace loopercat::wavimport
{

struct Prepared {
    juce::File file;        // hand this to commands::push
    bool converted = false; // true: `file` is a temp conversion — delete it after the push
};

// Fails when the source is not audio JUCE can read, or has more than two
// channels — a surround downmix is a creative decision, not a default.
juce::Result prepare(const juce::File& source, const juce::File& tempDir, Prepared& out);

} // namespace loopercat::wavimport
