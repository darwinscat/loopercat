// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "WavImport.h"

#include "Mp3AudioFormat.h"
#include "OperationsLog.h"

#include <loopercat/Error.hpp>
#include <loopercat/Loudness.hpp>
#include <loopercat/Wav.hpp>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace loopercat::wavimport
{

namespace
{
    constexpr int kBlock = 32768;

    // The pedal takes the file as-is when the core's own upload gate does —
    // one truth, not a parallel reimplementation of it.
    bool pedalAcceptsAsIs(wav::BytesView bytes)
    {
        try {
            wav::assertUploadable(wav::readWavInfo(bytes));
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

    // The conversion's file inside its job directory. The name says what the
    // file is; the name on the card is decided elsewhere.
    constexpr const char* kConvertedFileName = "converted.wav";

    // JUCE's readers that read a file's samples as they lie, so the width
    // they report is a fact about the file. Named by the READER
    // (AudioFormatReader::getFormatName — wavFormatName and its siblings in
    // juce_audio_formats/codecs), not by the format object that was asked:
    // WavAudioFormat hands an Ogg stream inside a WAV to the Ogg reader. A
    // JUCE upgrade that renamed one would make its files read as decoded by
    // the system, which errs toward the mark.
    constexpr const char* kWavReader = "WAV file";
    constexpr const char* kAiffReader = "AIFF file";
    constexpr const char* kFlacReader = "FLAC file";
    constexpr const char* kOggReader = "Ogg-Vorbis file";
    // The words for a system decoder — CoreAudio on macOS, Windows Media on
    // Windows — which takes what JUCE's own readers refuse (float64, mu-law,
    // AAC) and reports whatever width it chose: nothing about the file.
    constexpr const char* kSystemDecoded = "decoded by the system";

    SourceFormat factsOf(const juce::AudioFormatReader& reader)
    {
        SourceFormat facts { .sampleRate = int(std::llround(reader.sampleRate)),
                             .channels = static_cast<int>(reader.numChannels) };
        const juce::String name = reader.getFormatName();
        if (name == Mp3AudioFormat::kReaderName) {
            facts.encoding = "MP3";
        } else if (name == kOggReader) {
            facts.encoding = "Ogg Vorbis";
        } else if (name == kWavReader || name == kAiffReader || name == kFlacReader) {
            facts.encoding = juce::String(int(reader.bitsPerSample)) + "-bit"
                           + (reader.usesFloatingPointData ? " float" : "");
            facts.factual = true;
        } else {
            facts.encoding = kSystemDecoded;
        }
        return facts;
    }

    // The words the push report uses for a channel count prepare lets through.
    juce::String channelsWord(int channels) { return channels == 1 ? "mono" : "stereo"; }

    // A source the pedal takes as it is: no conversion, no job directory.
    Prepared untouched(const juce::File& source, std::optional<NormalizeOutcome> outcome)
    {
        Prepared p;
        p.file = source;
        p.converted = false;
        p.normalize = outcome;
        return p;
    }
} // namespace

juce::Result prepare(const juce::File& source, const juce::File& importTmp, Prepared& out,
                     const Options& options)
{
    juce::MemoryBlock raw;
    if (!source.loadFileAsData(raw))
        return juce::Result::fail("cannot read " + source.getFullPathName());
    const wav::BytesView bytes(static_cast<const unsigned char*>(raw.getData()), raw.getSize());
    // A WAV whose structure is at fault is refused with the fault named,
    // never handed to a decoder that would pad a cut data chunk with
    // silence or keep one of two data chunks and drop the other (review of
    // issue #139). What the gate then refuses is shape — a tag, a rate, a
    // width — and that is the converter's job.
    if (const auto fault = wav::riffFault(bytes))
        return juce::Result::fail(source.getFileName() + " " + *fault);
    const bool passesAsIs = pedalAcceptsAsIs(bytes);
    if (!options.normalizeTargetLufs.has_value() && passesAsIs) {
        out = untouched(source, std::nullopt);
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
    const SourceFormat sourceFormat = factsOf(*reader);

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
                out = untouched(source, outcome);
                return juce::Result::ok();
            }
        } else if (!measured.has_value()) {
            // Silence, or shorter than one gating block: nothing to level,
            // and inventing a gain would be a guess. Import as-is and say so.
            outcome = NormalizeOutcome {};
            if (passesAsIs) {
                out = untouched(source, outcome);
                return juce::Result::ok();
            }
        } else {
            const double wanted = target - *measured;
            if (passesAsIs && std::abs(wanted) < loudness::kAlreadyAtTargetLu) {
                outcome = NormalizeOutcome { .measurable = true, .untouched = true,
                                             .measuredLufs = *measured };
                out = untouched(source, outcome);
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

    // The job's own directory: nothing else writes there, so the file inside
    // carries a fixed name and nothing is ever renamed around a clash. The
    // name the audio lands under on the card is the caller's decision, made
    // at the push (issue #139) — this one is never seen outside this job.
    JobDir jobDir(importTmp.getChildFile(juce::Uuid().toString()));
    const juce::Result dirOk = jobDir.path().createDirectory();
    if (dirOk.failed())
        return dirOk;
    const juce::File dest = jobDir.path().getChildFile(kConvertedFileName);
    if (dest.existsAsFile())
        return juce::Result::fail(dest.getFullPathName() + " already exists in a fresh job directory");

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

    out.file = dest;
    out.converted = true;
    // Rebuilt unless there is positive evidence the samples are the source's:
    // a reader that reads them as they lie reported the pedal's own shape,
    // and no gain went in. A header-only rewrite of such a file is the one
    // case that is not a rebuild; anything a decoder made is.
    out.rebuilt = !sourceFormat.factual || differsFromTarget(sourceFormat)
               || (outcome.has_value() && outcome->measurable && !outcome->untouched);
    out.normalize = outcome;
    out.sourceFormat = sourceFormat;
    out.jobDir = std::move(jobDir);
    return juce::Result::ok();
}

bool differsFromTarget(const SourceFormat& source)
{
    return source.sampleRate != kTargetSampleRate || source.encoding != kTargetEncoding
        || source.channels != kTargetChannels;
}

void JobDir::release()
{
    if (dir_ == juce::File())
        return;
    // import-tmp lives in the app's data home, beside operations.log.
    if (!dir_.deleteRecursively())
        oplog::append(dir_.getParentDirectory().getParentDirectory(),
                      "import-tmp: could not remove " + dir_.getFullPathName()
                          + " after the job; remove it by hand");
    dir_ = juce::File();
}

juce::String describeConversion(const SourceFormat& source)
{
    juce::StringArray was, now;
    if (source.sampleRate != kTargetSampleRate) {
        was.add(juce::String(source.sampleRate) + " Hz");
        now.add(juce::String(kTargetSampleRate) + " Hz");
    }
    if (source.encoding != kTargetEncoding) {
        was.add(source.encoding);
        now.add(kTargetEncoding);
    }
    if (source.channels != kTargetChannels) {
        was.add(channelsWord(source.channels));
        now.add(channelsWord(kTargetChannels));
    }
    if (was.isEmpty())
        return {};
    // The same arrow describeNormalize draws, so a note carrying both
    // sentences reads with one.
    return was.joinIntoString(", ") + juce::String::fromUTF8(" \xe2\x86\x92 ")
         + now.joinIntoString(", ");
}

} // namespace loopercat::wavimport
