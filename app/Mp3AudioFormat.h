// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

//==============================================================================
// loopercat::Mp3AudioFormat — MP3 decode via minimp3 (fetched + pinned, CC0),
// issue #52. ONE bundled decoder on all three platforms instead of the OS
// codecs: Linux has no MP3 codec in a stock JUCE build at all, and the
// platform decoders that do exist (CoreAudio's ExtAudioFile aside) don't
// strip the LAME encoder delay/padding — ~25 ms of leading junk that lands
// the downbeat late and trailing padding that gaps the loop seam, which a
// looper cannot accept. minimp3_ex parses the Xing/LAME tag and trims both
// automatically; a file WITHOUT the tag decodes as-is — no guessed trims,
// the in-app trim tool covers those.
//
// Register this BEFORE registerBasicFormats() so .mp3 always takes this path
// (identical PCM everywhere); AudioFormatManager routes by file extension,
// so no other extension ever reaches it. Decode only — createWriterFor says
// no, the pedal speaks float32 WAV.
//==============================================================================
namespace loopercat
{

class Mp3AudioFormat final : public juce::AudioFormat
{
public:
    Mp3AudioFormat();

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor(juce::InputStream* sourceStream,
                                             bool deleteStreamIfOpeningFails) override;
    std::unique_ptr<juce::AudioFormatWriter>
    createWriterFor(std::unique_ptr<juce::OutputStream>&,
                    const juce::AudioFormatWriterOptions&) override
    {
        return nullptr; // decode only — uploads leave here as float32 WAV
    }
};

} // namespace loopercat
