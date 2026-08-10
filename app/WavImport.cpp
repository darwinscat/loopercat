// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "WavImport.h"

#include <loopercat/Error.hpp>
#include <loopercat/Wav.hpp>

namespace loopercat::wavimport
{

namespace
{
    // The pedal takes the file as-is when the core's own upload gate does —
    // one truth, not a parallel reimplementation of it.
    bool pedalAcceptsAsIs(const juce::File& source)
    {
        juce::MemoryBlock raw;
        if (!source.loadFileAsData(raw))
            return false;
        try {
            wav::assertUploadable(wav::readWavInfo(wav::BytesView(
                static_cast<const unsigned char*>(raw.getData()), raw.getSize())));
            return true;
        } catch (const Error&) {
            return false;
        }
    }
} // namespace

juce::Result prepare(const juce::File& source, const juce::File& tempDir, Prepared& out)
{
    if (pedalAcceptsAsIs(source)) {
        out = { source, false };
        return juce::Result::ok();
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(source));
    if (reader == nullptr)
        return juce::Result::fail(source.getFileName()
                                  + " is not an audio file LooperCat can read");
    const int channels = static_cast<int>(reader->numChannels);
    if (channels < 1 || channels > 2)
        return juce::Result::fail(source.getFileName() + " has "
                                  + juce::String(channels)
                                  + " channels — only mono and stereo can go to the pedal");

    const juce::Result dirOk = tempDir.createDirectory();
    if (dirOk.failed())
        return dirOk;
    const juce::File dest =
        tempDir.getChildFile(source.getFileNameWithoutExtension() + "-pedal.wav")
            .getNonexistentSibling();

    auto stream = dest.createOutputStream();
    if (stream == nullptr)
        return juce::Result::fail("cannot write " + dest.getFullPathName());
    // 32-bit WAV out of JUCE's writer IS IEEE float — the pedal's native
    // recording format; commands::push canonicalizes the header bytes after.
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(
        stream.get(), double(wav::kSampleRate), 2, 32, {}, 0));
    if (writer == nullptr)
        return juce::Result::fail("cannot open a float32 writer for " + dest.getFullPathName());
    stream.release(); // the writer owns it now

    juce::AudioFormatReaderSource readerSource(reader.get(), false);
    juce::ResamplingAudioSource resampler(&readerSource, false, channels);
    const double ratio = reader->sampleRate / double(wav::kSampleRate);
    resampler.setResamplingRatio(ratio);

    constexpr int kBlock = 32768;
    resampler.prepareToPlay(kBlock, double(wav::kSampleRate));
    juce::int64 remaining =
        juce::int64(std::llround(double(reader->lengthInSamples) / ratio));

    juce::AudioBuffer<float> block(channels, kBlock);
    while (remaining > 0) {
        const int n = int(std::min<juce::int64>(remaining, kBlock));
        block.setSize(channels, n, false, false, true);
        resampler.getNextAudioBlock(juce::AudioSourceChannelInfo(block));
        // Mono feeds both pedal channels; stereo passes straight through.
        const float* left = block.getReadPointer(0);
        const float* right = block.getReadPointer(channels == 2 ? 1 : 0);
        const float* data[2] = { left, right };
        if (!writer->writeFromFloatArrays(data, 2, n)) {
            writer.reset();
            dest.deleteFile();
            return juce::Result::fail("writing " + dest.getFullPathName() + " failed");
        }
        remaining -= n;
    }
    writer.reset(); // flush before anyone reads the file

    out = { dest, true };
    return juce::Result::ok();
}

} // namespace loopercat::wavimport
