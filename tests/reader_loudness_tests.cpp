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

// Little-endian RIFF building blocks, by hand: the suite assembles files the
// way the format spells them, not the way any reader expects them.
void ascii(std::vector<unsigned char>& b, std::string_view s)
{
    for (const char c : s)
        b.push_back(static_cast<unsigned char>(c));
}
void p16(std::vector<unsigned char>& b, int v)
{
    b.push_back(static_cast<unsigned char>(v & 0xff));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}
void p32(std::vector<unsigned char>& b, long long v)
{
    p16(b, static_cast<int>(v & 0xffff));
    p16(b, static_cast<int>((v >> 16) & 0xffff));
}
void pf(std::vector<unsigned char>& b, float v)
{
    p32(b, static_cast<long long>(std::bit_cast<std::uint32_t>(v)));
}
// The pedal's float32 stereo 44.1 kHz fmt body, 16 bytes.
void pedalFmt(std::vector<unsigned char>& b)
{
    p16(b, 3); p16(b, 2); p32(b, 44100); p32(b, 44100LL * 8); p16(b, 8); p16(b, 32);
}

// A float32 stereo 44.1 kHz WAV of a 997 Hz sine at `dbfs` peak in both
// channels — the tone BS.1770 calibrates on; `spike` plants one frame-0
// sample on channel 0, 1e20 of it making the take "damaged"; `extraChunk`
// puts a 26-byte LIST chunk between fmt and data, the DAW-export shape.
std::vector<unsigned char> sineWav(int frames, double dbfs, float spike = 0.0f,
                                   bool extraChunk = false)
{
    std::vector<unsigned char> b;
    const int extra = extraChunk ? 8 + 26 : 0;
    const int dataSize = frames * 8;
    ascii(b, "RIFF"); p32(b, 4 + 8 + 16 + extra + 8 + dataSize); ascii(b, "WAVE");
    ascii(b, "fmt "); p32(b, 16);
    pedalFmt(b);
    if (extraChunk) {
        ascii(b, "LIST"); p32(b, 26);
        b.insert(b.end(), 26, 0);
    }
    ascii(b, "data"); p32(b, dataSize);
    const double amp = std::pow(10.0, dbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * 997.0 / 44100.0;
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amp * std::sin(w * i));
        pf(b, i == 0 && spike > 0.0f ? spike : v);
        pf(b, v);
    }
    return b;
}

// The same file with its RIFF size field — bytes 4..7 — saying `size`.
std::vector<unsigned char> withRiffSize(std::vector<unsigned char> bytes, std::uint32_t size)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes[4 + static_cast<std::size_t>(shift / 8)] = static_cast<unsigned char>((size >> shift) & 0xffu);
    return bytes;
}

// A reader over the file, or null when JUCE will not open it — the pass
// then meters nothing and files nothing, which the suite counts as safe.
std::unique_ptr<juce::AudioFormatReader> tryOpen(const juce::File& file)
{
    juce::WavAudioFormat format;
    return std::unique_ptr<juce::AudioFormatReader>(
        format.createReaderFor(new juce::FileInputStream(file), true));
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
            const auto reader = open(file);
            const auto decoded = readerloudness::measure(*reader);
            CHECK(core.integratedLufs.has_value());
            checkSameReading(core, decoded);
            // the gate's two halves: the core would measure it, and JUCE
            // decoded the frames the core would — as many, same rate, stereo
            const auto info = readerloudness::measurableInfo(file);
            CHECK(info.has_value());
            CHECK(info.has_value() && info->frames == 3 * 44100);
            CHECK(info.has_value() && readerloudness::decodesAsCore(*reader, *info));
        }
        // the DAW-export shape, a LIST chunk ahead of data: read whole by both
        {
            const auto listed = sineWav(44100, -23.0, 0.0f, /*extraChunk=*/true);
            const juce::File file = scratch.put("listed.wav", listed);
            const auto reader = open(file);
            checkSameReading(wav::measureLoudness(view(listed)), readerloudness::measure(*reader));
            const auto info = readerloudness::measurableInfo(file);
            CHECK(info.has_value() && readerloudness::decodesAsCore(*reader, *info));
        }
        // digital silence: both say "nothing to measure", -inf dBTP, and still agree
        const auto quiet = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 44100 });
        const juce::File quietFile = scratch.put("silence.wav", quiet);
        const auto quietCore = wav::measureLoudness(view(quiet));
        CHECK(!quietCore.integratedLufs.has_value());
        checkSameReading(quietCore, readerloudness::measure(*open(quietFile)));
        CHECK(readerloudness::measurableInfo(quietFile));
        // a damaged take: the wild count is the same count
        const auto damaged = sineWav(44100, -23.0, 1.0e20f);
        const juce::File damagedFile = scratch.put("damaged.wav", damaged);
        const auto damagedCore = wav::measureLoudness(view(damaged));
        CHECK_EQ(damagedCore.wildSamples, 1);
        checkSameReading(damagedCore, readerloudness::measure(*open(damagedFile)));
        CHECK(readerloudness::measurableInfo(damagedFile));
        // under one gating block: unmeasurable on both sides
        const auto brief = sineWav(4410, -23.0);
        const juce::File briefFile = scratch.put("brief.wav", brief);
        checkSameReading(wav::measureLoudness(view(brief)), readerloudness::measure(*open(briefFile)));
        CHECK(readerloudness::measurableInfo(briefFile));
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
        CHECK(!readerloudness::measurableInfo(cutFile));
        // 2 s present, 3 s claimed: the padded number is not the audio's
        auto padded = sineWav(3 * 44100, -20.0);
        padded.resize(44 + 2 * 44100 * 8);
        const juce::File paddedFile = scratch.put("cut-third.wav", padded);
        const auto audio = wav::measureLoudness(view(sineWav(2 * 44100, -20.0)));
        const auto juceSays = readerloudness::measure(*open(paddedFile));
        CHECK(audio.integratedLufs && juceSays.integratedLufs
              && std::abs(*audio.integratedLufs - *juceSays.integratedLufs) > 0.1);
        CHECK(!readerloudness::measurableInfo(paddedFile));
        // pcm16 stereo: JUCE decodes it, the pedal gate refuses it
        const auto pcm = testkit::syntheticWav({ .tag = 1, .bits = 16, .frames = 44100 });
        const juce::File pcmFile = scratch.put("pcm16.wav", pcm);
        CHECK_THROWS(wav::measureLoudness(view(pcm)), "32-bit float");
        (void) readerloudness::measure(*open(pcmFile)); // reads fine — which is the problem
        CHECK(!readerloudness::measurableInfo(pcmFile));
        // mono float32: the same
        const auto mono = testkit::syntheticWav({ .tag = 3, .channels = 1, .bits = 32, .frames = 44100 });
        const juce::File monoFile = scratch.put("mono.wav", mono);
        CHECK_THROWS(wav::measureLoudness(view(mono)), "stereo");
        (void) readerloudness::measure(*open(monoFile));
        CHECK(!readerloudness::measurableInfo(monoFile));
        // 48 kHz float32: the pedal does not play it, the gate does not file it
        const auto fast = testkit::syntheticWav({ .tag = 3, .sampleRate = 48000, .bits = 32, .frames = 48000 });
        CHECK(!readerloudness::measurableInfo(scratch.put("48k.wav", fast)));
        // not a WAV at all, and a file that is not there
        CHECK(!readerloudness::measurableInfo(scratch.put("text.wav", std::vector<unsigned char>(100, 'x'))));
        CHECK(!readerloudness::measurableInfo(scratch.dir.getChildFile("missing.wav")));
        // the gate reads the header and the size, nothing more: the same
        // answers off the first 44 bytes as off the file
        const auto whole = sineWav(44100, -23.0);
        CHECK(readerloudness::measurableInfo(wav::BytesView(whole.data(), 44),
                                               static_cast<std::int64_t>(whole.size())));
        CHECK(!readerloudness::measurableInfo(wav::BytesView(whole.data(), 44), 44 + 13230 * 8));
        CHECK(!readerloudness::measurableInfo(wav::BytesView(whole.data(), 44), 44)); // no frames
        CHECK(!readerloudness::measurableInfo(wav::BytesView(whole.data(), 40),
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
                // and a reader whose frame count is not the core's is not the core's reader
                CHECK(!readerloudness::decodesAsCore(*reader, wav::readWavInfo(view(whole))));
            }
        }
    }

    // --- 2b. the core reads the file whole; JUCE reads what the RIFF size field lets it ---
    //
    // Theory: JUCE's chunk walk stops at the RIFF size field and sizes the
    // data chunk with the frame size it knows when it meets it; the core walks
    // the file to its end and takes fmt and data in any order. On a file the
    // two read differently the gate's first half (the core would measure it)
    // holds and its second half (JUCE decoded the core's frames) must fail —
    // or the pass would file JUCE's {absent, 0, -inf} under the hash the
    // check job files the core's number under (review of #140, P0 narrowed).
    {
        struct Shape {
            const char* name;
            std::vector<unsigned char> bytes;
        };
        // (a) a RIFF size that ends right after fmt: WAVE + the fmt chunk
        const auto afterFmt = withRiffSize(sineWav(44100, -20.0), 4 + 8 + 16);
        // (b) a RIFF size that ends inside a LIST chunk ahead of data: ten of its 26 bytes
        const auto insideList = withRiffSize(sineWav(44100, -20.0, 0.0f, /*extraChunk=*/true), 4 + 24 + 8 + 10);
        // (c) a tiny file with data ahead of fmt: eight frames at half scale
        std::vector<unsigned char> dataFirst;
        ascii(dataFirst, "RIFF"); p32(dataFirst, 4 + 8 + 64 + 8 + 16); ascii(dataFirst, "WAVE");
        ascii(dataFirst, "data"); p32(dataFirst, 64);
        for (int i = 0; i < 16; ++i)
            pf(dataFirst, 0.5f);
        ascii(dataFirst, "fmt "); p32(dataFirst, 16);
        pedalFmt(dataFirst);
        for (const Shape& shape : { Shape { "riff-after-fmt.wav", afterFmt },
                                    Shape { "riff-inside-list.wav", insideList },
                                    Shape { "data-before-fmt.wav", dataFirst } }) {
            const juce::File file = scratch.put(shape.name, shape.bytes);
            // the core reads the whole file: a reading with a real peak
            const auto core = wav::measureLoudness(view(shape.bytes));
            CHECK(core.samplePeak > 0.0f);
            const auto info = readerloudness::measurableInfo(file);
            CHECK(info.has_value()); // the first half of the gate holds...
            const auto reader = tryOpen(file);
            CHECK(reader != nullptr); // ...and JUCE opens the file...
            if (reader == nullptr || !info)
                continue;
            CHECK_EQ(reader->lengthInSamples, 0); // ...as one of no frames
            CHECK(!readerloudness::decodesAsCore(*reader, *info)); // so the second half fails
            // which is what JUCE's number would have been filed as
            const auto juceSays = readerloudness::measure(*reader);
            CHECK(!juceSays.integratedLufs.has_value());
            CHECK_EQ(std::bit_cast<std::uint32_t>(juceSays.samplePeak), std::bit_cast<std::uint32_t>(0.0f));
        }
        // the two tone-length shapes read -20 LUFS whole: the number the check job files
        for (const auto* bytes : { &afterFmt, &insideList }) {
            const auto core = wav::measureLoudness(view(*bytes));
            CHECK(core.integratedLufs.has_value() && std::abs(*core.integratedLufs - (-20.0)) <= 0.1);
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
