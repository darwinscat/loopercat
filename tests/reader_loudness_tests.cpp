// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The player's meter over JUCE's decoder (app/ReaderLoudness.h) against the
// core's own (wav::measureLoudness), and the gate between them (#140):
//
//   1. for a take the pedal gate accepts — stereo float32, whole — what the
//      pass reads off JUCE's decoder IS what the core reads off the bytes,
//      bit for bit: LUFS, sample peak, true peak, wild count. A decoder that
//      starts to disagree fails here, before it files a wrong number;
//   2. the gate refuses what JUCE would still happily decode and the core
//      would not: a file shorter than its header claims (JUCE pads it with
//      silence), pcm16, mono — and it refuses off the first bytes and the
//      size, never the whole file;
//   3. the streamed file hash is the in-memory hash of the same bytes, and a
//      read cut short is no hash at all.

#include "support.hpp"

#include "../app/ReaderLoudness.h"
#include "../app/history/ContentHash.h"
#include "../app/history/FileHash.h"

#include <loopercat/Normalize.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

using namespace loopercat;

namespace {

// A float32 stereo 44.1 kHz WAV of a 997 Hz sine at `dbfs` peak in both
// channels — the tone BS.1770 calibrates on; `spike` plants one frame-0
// sample on channel 0, 1e20 of it making the take "damaged".
std::vector<unsigned char> sineWav(int frames, double dbfs, float spike = 0.0f)
{
    std::vector<unsigned char> b;
    const auto ascii = [&b](std::string_view s) {
        for (const char c : s)
            b.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](long long v) {
        p16(static_cast<int>(v & 0xffff));
        p16(static_cast<int>((v >> 16) & 0xffff));
    };
    const auto pf = [&p32](float v) { p32(static_cast<long long>(std::bit_cast<std::uint32_t>(v))); };
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(4 + 8 + 16 + 8 + dataSize); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(44100); p32(44100LL * 8); p16(8); p16(32);
    ascii("data"); p32(dataSize);
    const double amp = std::pow(10.0, dbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * 997.0 / 44100.0;
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amp * std::sin(w * i));
        pf(i == 0 && spike > 0.0f ? spike : v);
        pf(v);
    }
    return b;
}

wav::BytesView view(const std::vector<unsigned char>& b) { return { b.data(), b.size() }; }

std::string_view chars(const std::vector<unsigned char>& b)
{
    return { reinterpret_cast<const char*>(b.data()), b.size() };
}

struct Scratch {
    juce::File dir;
    Scratch()
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("loopercat-reader-loudness-"
                                + juce::String(juce::Time::getHighResolutionTicks())))
    {
        if (!dir.createDirectory().wasOk())
            throw Error("cannot create the scratch directory");
    }
    ~Scratch()
    {
        // Every reader over these files must be gone by now: a failed sweep is
        // a failed test, not a quiet leftover (the Windows lane holds open files).
        if (!dir.deleteRecursively())
            testkit::fail("scratch directory not removed: " + dir.getFullPathName().toStdString(),
                          __FILE__, __LINE__);
    }
    juce::File put(const juce::String& name, const std::vector<unsigned char>& bytes) const
    {
        const juce::File file = dir.getChildFile(name);
        if (!file.replaceWithData(bytes.data(), bytes.size()))
            throw Error("cannot write " + file.getFullPathName().toStdString());
        return file;
    }
};

std::unique_ptr<juce::AudioFormatReader> open(const juce::File& file)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(
        format.createReaderFor(new juce::FileInputStream(file), true));
    if (reader == nullptr)
        throw Error("JUCE could not open " + file.getFullPathName().toStdString());
    return reader;
}

// Bit for bit: a reading is four numbers, and "close enough" is how a wrong
// decoder slips a wrong number into the history.
void checkSameReading(const wav::LoudnessReading& core, const wav::LoudnessReading& decoded)
{
    CHECK_EQ(core.integratedLufs.has_value(), decoded.integratedLufs.has_value());
    if (core.integratedLufs && decoded.integratedLufs)
        CHECK_EQ(std::bit_cast<std::uint64_t>(*core.integratedLufs),
                 std::bit_cast<std::uint64_t>(*decoded.integratedLufs));
    CHECK_EQ(std::bit_cast<std::uint32_t>(core.samplePeak), std::bit_cast<std::uint32_t>(decoded.samplePeak));
    CHECK_EQ(std::bit_cast<std::uint64_t>(core.truePeakDb), std::bit_cast<std::uint64_t>(decoded.truePeakDb));
    CHECK_EQ(core.wildSamples, decoded.wildSamples);
}

} // namespace

int main()
{
    Scratch scratch;

    // --- 1. the pass reads what the core reads, bit for bit, on pedal float32 ---
    {
        // a loud take, a quiet one, three seconds long (block boundaries fall
        // inside it: the core chunks at 4096 frames, the pass at 32768)
        for (const double dbfs : { -14.0, -23.0, -40.0 }) {
            const auto bytes = sineWav(3 * 44100, dbfs);
            const juce::File file = scratch.put("tone" + juce::String(dbfs) + ".wav", bytes);
            const auto core = wav::measureLoudness(view(bytes));
            const auto decoded = readerloudness::measure(*open(file));
            CHECK(core.integratedLufs.has_value());
            checkSameReading(core, decoded);
            CHECK(readerloudness::coreWouldMeasure(file));
        }
        // digital silence: both say "nothing to measure", -inf dBTP, and still agree
        const auto quiet = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 44100 });
        const juce::File quietFile = scratch.put("silence.wav", quiet);
        const auto quietCore = wav::measureLoudness(view(quiet));
        CHECK(!quietCore.integratedLufs.has_value());
        checkSameReading(quietCore, readerloudness::measure(*open(quietFile)));
        CHECK(readerloudness::coreWouldMeasure(quietFile));
        // a damaged take: the wild count is the same count
        const auto damaged = sineWav(44100, -23.0, 1.0e20f);
        const juce::File damagedFile = scratch.put("damaged.wav", damaged);
        const auto damagedCore = wav::measureLoudness(view(damaged));
        CHECK_EQ(damagedCore.wildSamples, 1);
        checkSameReading(damagedCore, readerloudness::measure(*open(damagedFile)));
        CHECK(readerloudness::coreWouldMeasure(damagedFile));
        // under one gating block: unmeasurable on both sides
        const auto brief = sineWav(4410, -23.0);
        const juce::File briefFile = scratch.put("brief.wav", brief);
        checkSameReading(wav::measureLoudness(view(brief)), readerloudness::measure(*open(briefFile)));
        CHECK(readerloudness::coreWouldMeasure(briefFile));
    }

    // --- 2. the gate refuses what JUCE decodes and the core would not ---
    {
        // 0.3 s present, 1 s claimed: JUCE pads to the header's length and
        // still reads a number; the core refuses the file; the gate says no
        auto cut = sineWav(44100, -23.0);
        cut.resize(44 + 13230 * 8);
        const juce::File cutFile = scratch.put("cut-short.wav", cut);
        CHECK_THROWS(wav::measureLoudness(view(cut)), "truncated");
        CHECK(readerloudness::measure(*open(cutFile)).integratedLufs.has_value()); // the reviewer's -23.0
        CHECK(!readerloudness::coreWouldMeasure(cutFile));
        // 2 s present, 3 s claimed: the padded number is not the audio's
        auto padded = sineWav(3 * 44100, -20.0);
        padded.resize(44 + 2 * 44100 * 8);
        const juce::File paddedFile = scratch.put("cut-third.wav", padded);
        const auto audio = wav::measureLoudness(view(sineWav(2 * 44100, -20.0)));
        const auto juceSays = readerloudness::measure(*open(paddedFile));
        CHECK(audio.integratedLufs && juceSays.integratedLufs
              && std::abs(*audio.integratedLufs - *juceSays.integratedLufs) > 0.1);
        CHECK(!readerloudness::coreWouldMeasure(paddedFile));
        // pcm16 stereo: JUCE decodes it, the pedal gate refuses it
        const auto pcm = testkit::syntheticWav({ .tag = 1, .bits = 16, .frames = 44100 });
        const juce::File pcmFile = scratch.put("pcm16.wav", pcm);
        CHECK_THROWS(wav::measureLoudness(view(pcm)), "32-bit float");
        (void) readerloudness::measure(*open(pcmFile)); // reads fine — which is the problem
        CHECK(!readerloudness::coreWouldMeasure(pcmFile));
        // mono float32: the same
        const auto mono = testkit::syntheticWav({ .tag = 3, .channels = 1, .bits = 32, .frames = 44100 });
        const juce::File monoFile = scratch.put("mono.wav", mono);
        CHECK_THROWS(wav::measureLoudness(view(mono)), "stereo");
        (void) readerloudness::measure(*open(monoFile));
        CHECK(!readerloudness::coreWouldMeasure(monoFile));
        // 48 kHz float32: the pedal does not play it, the gate does not file it
        const auto fast = testkit::syntheticWav({ .tag = 3, .sampleRate = 48000, .bits = 32, .frames = 48000 });
        CHECK(!readerloudness::coreWouldMeasure(scratch.put("48k.wav", fast)));
        // not a WAV at all, and a file that is not there
        CHECK(!readerloudness::coreWouldMeasure(scratch.put("text.wav", std::vector<unsigned char>(100, 'x'))));
        CHECK(!readerloudness::coreWouldMeasure(scratch.dir.getChildFile("missing.wav")));
        // the gate reads the header and the size, nothing more: the same
        // answers off the first 44 bytes as off the file
        const auto whole = sineWav(44100, -23.0);
        CHECK(readerloudness::coreWouldMeasure(wav::BytesView(whole.data(), 44),
                                               static_cast<std::int64_t>(whole.size())));
        CHECK(!readerloudness::coreWouldMeasure(wav::BytesView(whole.data(), 44), 44 + 13230 * 8));
        CHECK(!readerloudness::coreWouldMeasure(wav::BytesView(whole.data(), 44), 44)); // no frames
        CHECK(!readerloudness::coreWouldMeasure(wav::BytesView(whole.data(), 40),
                                                static_cast<std::int64_t>(whole.size())));
        // and JUCE never says it stopped short: a reader told its stream is a
        // frame longer than it is reads that frame as silence and reports
        // success — which is the whole reason the gate, not the decoder,
        // decides what gets filed
        {
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader(
                format.createReaderFor(new juce::MemoryInputStream(whole.data(), whole.size(), false), true));
            CHECK(reader != nullptr);
            if (reader) {
                reader->lengthInSamples += 1;
                CHECK(readerloudness::measure(*reader).integratedLufs.has_value());
            }
        }
    }

    // --- 3. the streamed hash is the in-memory hash, or nothing ---
    {
        const auto bytes = sineWav(3 * 44100, -18.0); // 1 MB and a bit: past one hash buffer
        const juce::File file = scratch.put("hashed.wav", bytes);
        CHECK(bytes.size() > static_cast<std::size_t>(history::kHashBufferBytes));
        const auto streamed = history::fileContentHash(file);
        CHECK(streamed.has_value());
        CHECK(streamed.has_value() && *streamed == history::contentHash(chars(bytes)));
        CHECK(streamed.has_value() && streamed->size() == 32u);
        // the empty file hashes like empty bytes; a missing file hashes to nothing
        // (juce::File::replaceWithData with no bytes deletes rather than empties: create() instead)
        const juce::File empty = scratch.dir.getChildFile("empty.bin");
        CHECK(empty.create().wasOk());
        CHECK(empty.existsAsFile() && empty.getSize() == 0);
        CHECK(history::fileContentHash(empty) == history::contentHash(""));
        CHECK(!history::fileContentHash(scratch.dir.getChildFile("nowhere.bin")).has_value());
        // told to stop before the end: no hash of a prefix
        int reads = 0;
        const auto stopAfterFirst = [&reads] { return reads++ > 0; };
        CHECK(!history::fileContentHash(file, stopAfterFirst).has_value());
        CHECK(!history::fileContentHash(file, [] { return true; }).has_value());
        // a tiny file, under one digest block
        const juce::File tiny = scratch.put("tiny.bin", std::vector<unsigned char>(7, 'q'));
        CHECK(history::fileContentHash(tiny) == history::contentHash("qqqqqqq"));
    }

    return testkit::summary("reader_loudness_tests");
}
