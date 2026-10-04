// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <optional>
#include <utility>

//==============================================================================
// loopercat::wavimport — make a user's audio file pedal-acceptable (issue
// #20). The pedal wants 44.1 kHz stereo WAV in 16/24-bit PCM or 32-bit
// float; DAW exports come as anything — WAVE_FORMAT_EXTENSIBLE headers,
// 48 kHz, mono, AIFF. A file the pedal already accepts passes through
// UNTOUCHED (the byte-surgery invariant extends to uploads); everything
// else JUCE can read is rewritten as 44.1 kHz stereo float32 — the pedal's
// own native recording format — into a temp directory of its own under the
// caller's import-tmp, which dies with the Prepared that owns it.
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
    bool cappedByPeak = false; // the boost stopped at the -1 dBTP true-peak
                               // ceiling, short of target — loud peaks, quiet body
    double measuredLufs = 0;   // valid when measurable
    double gainDb = 0;         // gain actually baked in (0 when untouched/unmeasurable)
    bool damaged = false;      // bytes that are not audio were found: imported
                               // with no gain — no "loudness" of garbage is a target
    std::int64_t wildSamples = 0; // how many, for the report
};

// What the audio was before the rebuild (issue #139) — the facts the push
// report states, against the fixed target of 44100 Hz, 32-bit float, stereo.
struct SourceFormat {
    int sampleRate = 0;    // Hz
    // "24-bit", "32-bit float" — the file's own sample format; for a
    // compressed source its codec ("MP3"): the bit depth a decoder hands
    // out is the decoder's choice, not a fact about the file.
    juce::String encoding;
    int channels = 0;      // 1 or 2; prepare refuses the rest
};

// The pedal's side of the sentence; a source that matches it on every point
// gets no sentence at all.
inline constexpr int kTargetSampleRate = 44100;
inline constexpr const char* kTargetEncoding = "32-bit float";
inline constexpr int kTargetChannels = 2;

// "48000 Hz, 24-bit, mono → 44100 Hz, 32-bit float, stereo", saying only
// what changed: a 44.1 kHz 24-bit stereo file reads "24-bit → 32-bit float",
// and a pedal-ready file that was rewritten for its loudness alone reads as
// nothing — "32-bit float → 32-bit float" is not a change anyone needs told.
juce::String describeConversion(const SourceFormat& source);

// The conversion's own directory under import-tmp: one per job, so two
// uploads of files with the same stem in one queue never meet, and gone with
// the Prepared that owns it — after the push succeeded and after it threw
// alike. Empty until a conversion needs one.
class JobDir
{
public:
    JobDir() = default;
    explicit JobDir(juce::File dir) : dir_(std::move(dir)) {}
    ~JobDir() { release(); }
    JobDir(JobDir&& other) noexcept : dir_(std::exchange(other.dir_, juce::File())) {}
    JobDir& operator=(JobDir&& other) noexcept
    {
        if (this != &other) {
            release();
            dir_ = std::exchange(other.dir_, juce::File());
        }
        return *this;
    }
    JobDir(const JobDir&) = delete;
    JobDir& operator=(const JobDir&) = delete;

    const juce::File& path() const { return dir_; }

private:
    void release()
    {
        if (dir_ != juce::File())
            dir_.deleteRecursively();
        dir_ = juce::File();
    }
    juce::File dir_;
};

struct Prepared {
    juce::File file;        // hand this to commands::push
    bool converted = false; // true: `file` is a conversion in `jobDir`, not the source
    std::optional<NormalizeOutcome> normalize; // engaged iff Options asked for it
    std::optional<SourceFormat> sourceFormat;  // what was rebuilt; present iff converted
    JobDir jobDir;          // the conversion's directory; its lifetime is the file's
};

// Fails when the source is not audio JUCE can read, or has more than two
// channels — a surround downmix is a creative decision, not a default.
// `importTmp` is the app's import-tmp root; a conversion gets a directory of
// its own under it.
juce::Result prepare(const juce::File& source, const juce::File& importTmp, Prepared& out,
                     const Options& options);

} // namespace loopercat::wavimport
