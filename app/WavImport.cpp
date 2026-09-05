// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "WavImport.h"

#include "Mp3AudioFormat.h"

#include <loopercat/Error.hpp>
#include <loopercat/Loudness.hpp>
#include <loopercat/Wav.hpp>

#include <cmath>
#include <vector>

namespace loopercat::wavimport
{

namespace
{
    constexpr int kBlock = 32768;

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

    // How many 44.1 kHz frames the conversion of `reader` will produce.
    juce::int64 outputFrames(const juce::AudioFormatReader& reader)
    {
        return juce::int64(std::llround(double(reader.lengthInSamples)
                                        * double(wav::kSampleRate) / reader.sampleRate));
    }

    // The measurement pass feeds the meter the SAME stream the write pass
    // produces — post-resample, mono already duplicated into both channels —
    // so the gain is computed for exactly the samples that will land on the
    // card. That pins issue #53's open point: a duplicated mono source is
    // measured as the stereo sum it will actually play as.
    void measureStream(juce::AudioFormatReader& reader, loudness::Meter& meter)
    {
        const int channels = static_cast<int>(reader.numChannels);
        juce::AudioFormatReaderSource readerSource(&reader, false);
        juce::ResamplingAudioSource resampler(&readerSource, false, channels);
        const double ratio = reader.sampleRate / double(wav::kSampleRate);
        resampler.setResamplingRatio(ratio);
        resampler.prepareToPlay(kBlock, double(wav::kSampleRate));

        juce::int64 remaining = outputFrames(reader);
        juce::AudioBuffer<float> block(channels, kBlock);
        std::vector<float> interleaved(2 * std::size_t(kBlock));
        while (remaining > 0) {
            const int n = int(std::min<juce::int64>(remaining, kBlock));
            block.setSize(channels, n, false, false, true);
            resampler.getNextAudioBlock(juce::AudioSourceChannelInfo(block));
            const float* left = block.getReadPointer(0);
            const float* right = block.getReadPointer(channels == 2 ? 1 : 0);
            for (int i = 0; i < n; ++i) {
                interleaved[2 * std::size_t(i)] = left[i];
                interleaved[2 * std::size_t(i) + 1] = right[i];
            }
            meter.process(interleaved.data(), std::size_t(n));
            remaining -= n;
        }
    }
} // namespace

juce::Result prepare(const juce::File& source, const juce::File& tempDir, Prepared& out,
                     const Options& options)
{
    const bool passesAsIs = pedalAcceptsAsIs(source);
    if (!options.normalizeTargetLufs.has_value() && passesAsIs) {
        out = { source, false, std::nullopt };
        return juce::Result::ok();
    }

    juce::AudioFormatManager formats;
    // The bundled gapless decoder goes FIRST so .mp3 takes it even where an
    // OS codec would also claim the extension — identical PCM everywhere.
    formats.registerFormat(new Mp3AudioFormat(), false);
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

    // Pass 1 of the opt-in normalization (issue #53): measure, decide, and —
    // when a pedal-ready file needs nothing — keep the byte-exact promise.
    double gainDb = 0.0;
    std::optional<NormalizeOutcome> outcome;
    if (options.normalizeTargetLufs.has_value()) {
        const double target = *options.normalizeTargetLufs;
        loudness::Meter meter(wav::kSampleRate);
        measureStream(*reader, meter);
        const std::optional<double> measured = meter.integratedLufs();
        if (meter.wildSamples() > 0) {
            // Bytes that are not audio: whatever "loudness" they add up to is
            // not a thing to aim at. Import as-is, report the damage.
            outcome = NormalizeOutcome { .damaged = true, .wildSamples = meter.wildSamples() };
            if (passesAsIs) {
                out = { source, false, outcome };
                return juce::Result::ok();
            }
        } else if (!measured.has_value()) {
            // Silence, or shorter than one gating block: nothing to level,
            // and inventing a gain would be a guess. Import as-is and say so.
            outcome = NormalizeOutcome {};
            if (passesAsIs) {
                out = { source, false, outcome };
                return juce::Result::ok();
            }
        } else {
            const double wanted = target - *measured;
            if (passesAsIs && std::abs(wanted) < loudness::kAlreadyAtTargetLu) {
                outcome = NormalizeOutcome { .measurable = true, .untouched = true,
                                             .measuredLufs = *measured };
                out = { source, false, outcome };
                return juce::Result::ok();
            }
            gainDb = loudness::normalizeGainDb(*measured, target, meter.truePeakDb(),
                                               loudness::kPeakCeilingDb);
            outcome = NormalizeOutcome { .measurable = true,
                                         .cappedByPeak = wanted > 0.0 && gainDb + 1.0e-9 < wanted,
                                         .measuredLufs = *measured,
                                         .gainDb = gainDb };
        }
    }

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

    // A fresh reader source starts at frame zero — pass 1 left the shared
    // reader's position behind it, and this rewinds without reopening.
    juce::AudioFormatReaderSource readerSource(reader.get(), false);
    juce::ResamplingAudioSource resampler(&readerSource, false, channels);
    const double ratio = reader->sampleRate / double(wav::kSampleRate);
    resampler.setResamplingRatio(ratio);

    resampler.prepareToPlay(kBlock, double(wav::kSampleRate));
    juce::int64 remaining = outputFrames(*reader);

    const float scale = static_cast<float>(std::pow(10.0, gainDb / 20.0));
    juce::AudioBuffer<float> block(channels, kBlock);
    while (remaining > 0) {
        const int n = int(std::min<juce::int64>(remaining, kBlock));
        block.setSize(channels, n, false, false, true);
        resampler.getNextAudioBlock(juce::AudioSourceChannelInfo(block));
        if (outcome.has_value() && outcome->measurable)
            block.applyGain(scale);
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

    out = { dest, true, outcome };
    return juce::Result::ok();
}

} // namespace loopercat::wavimport
