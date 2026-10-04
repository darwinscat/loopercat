// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// wavimport::prepare against the THEORY of what the pedal accepts (issue
// #20): a file the pedal takes as-is must pass through untouched; everything
// else JUCE reads becomes 44.1 kHz stereo float32; what cannot become that
// fails plainly. The field case that started this: DAW 24-bit exports wear
// WAVE_FORMAT_EXTENSIBLE headers the strict core parser refuses.
//
// Issue #139 adds what a conversion reports about itself — the source's
// rate, sample format and channels, said only where they differ from the
// pedal's — and where it lives: a directory per job under import-tmp, gone
// with the job whichever way the job ends.

#include "support.hpp"

#include "../app/WavImport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <loopercat/Error.hpp>
#include <loopercat/Loudness.hpp>
#include <loopercat/Wav.hpp>

#include <bit>
#include <cmath>
#include <numbers>
#include <optional>

using namespace loopercat;

namespace
{

juce::File writeTemp(const juce::File& dir, const juce::String& name,
                     const std::vector<unsigned char>& bytes)
{
    const juce::File f = dir.getChildFile(name);
    f.replaceWithData(bytes.data(), bytes.size());
    return f;
}

// A WAVE_FORMAT_EXTENSIBLE 24-bit 44.1 kHz stereo file — the shape DAWs
// export and the core parser (rightly) refuses: fmt is 40 bytes, tag 0xFFFE,
// the real format hides in the SubFormat GUID.
std::vector<unsigned char> extensiblePcm24(int frames)
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
    const int blockAlign = 2 * 3;
    const int dataSize = frames * blockAlign;
    ascii("RIFF"); p32(4 + 8 + 40 + 8 + dataSize); ascii("WAVE");
    ascii("fmt "); p32(40);
    p16(0xfffe);                 // WAVE_FORMAT_EXTENSIBLE
    p16(2); p32(44100); p32(44100 * blockAlign); p16(blockAlign); p16(24);
    p16(22);                     // cbSize
    p16(24);                     // valid bits
    p32(3);                      // channel mask: L | R
    // SubFormat GUID: KSDATAFORMAT_SUBTYPE_PCM
    p32(1); p16(0); p16(0x10);
    for (const unsigned char c : { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 })
        b.push_back(c);
    ascii("data"); p32(dataSize);
    for (int i = 0; i < dataSize; ++i)
        b.push_back(0);
    return b;
}

// Mono pcm16 with a frame-index ramp — testkit's rampFill is stereo-only,
// and the mono test needs a signal, not silence, to prove the duplication.
std::vector<unsigned char> monoRamp(int frames)
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
    const int dataSize = frames * 2;
    ascii("RIFF"); p32(4 + 8 + 16 + 8 + dataSize); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(1); p16(1); p32(44100); p32(44100 * 2); p16(2); p16(16);
    ascii("data"); p32(dataSize);
    for (int frame = 0; frame < frames; ++frame)
        p16((frame - frames / 2) & 0xffff);
    return b;
}

wav::Info infoOf(const juce::File& f)
{
    juce::MemoryBlock raw;
    testkit::check(f.loadFileAsData(raw), "loadFileAsData", __FILE__, __LINE__);
    return wav::readWavInfo(
        wav::BytesView(static_cast<const unsigned char*>(raw.getData()), raw.getSize()));
}

// --- fixtures and meters for the normalize-at-import cases (issue #53) ---

float dbAmp(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// float32 WAV bytes holding a 997 Hz sine (Tech 3341's reference tone) at
// `amp` peak in every channel. `spike` plants one loud frame-0 sample on
// channel 0 — the quiet-but-peaky shape whose boost must stop at the ceiling.
std::vector<unsigned char> sineWav(int frames, int channels, float amp, float spike = 0.0f)
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
    const auto pf = [&p32](float v) {
        p32(static_cast<long long>(std::bit_cast<std::uint32_t>(v)));
    };
    const int blockAlign = channels * 4;
    const int dataSize = frames * blockAlign;
    ascii("RIFF"); p32(4 + 8 + 16 + 8 + dataSize); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(channels); p32(44100); p32(44100LL * blockAlign); p16(blockAlign); p16(32);
    ascii("data"); p32(dataSize);
    const double w = 2.0 * std::numbers::pi * 997.0 / 44100.0;
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amp * std::sin(w * i));
        pf(i == 0 && spike > 0.0f ? spike : v);
        for (int c = 1; c < channels; ++c)
            pf(v);
    }
    return b;
}

// Integrated loudness (and optionally the true peak, dBTP) of a written file,
// through the same core meter the import used — the check is that the OUTPUT
// lands on target, not that the code multiplied by what it said.
std::optional<double> measureLufs(const juce::File& f, double* truePeakDbOut = nullptr)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(f));
    if (reader == nullptr)
        return std::nullopt;
    const int frames = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buf(2, frames);
    reader->read(&buf, 0, frames, 0, true, true);
    std::vector<float> interleaved(2 * static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        interleaved[2 * static_cast<std::size_t>(i)] = buf.getSample(0, i);
        interleaved[2 * static_cast<std::size_t>(i) + 1] = buf.getSample(1, i);
    }
    loudness::Meter meter(44100);
    meter.process(interleaved.data(), static_cast<std::size_t>(frames));
    if (truePeakDbOut != nullptr)
        *truePeakDbOut = meter.truePeakDb();
    return meter.integratedLufs();
}

void checkNear(double actual, double expected, double tol, const char* what, const char* file,
               int line)
{
    ++testkit::checksRun;
    if (!(std::abs(actual - expected) <= tol)) {
        std::ostringstream os;
        os << what << "  (actual: " << actual << ", expected: " << expected << " +/- " << tol
           << ")";
        testkit::fail(os.str(), file, line);
    }
}

#define CHECK_NEAR(actual, expected, tol)                                                          \
    checkNear((actual), (expected), (tol), #actual " ~= " #expected, __FILE__, __LINE__)

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::File work =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("wav_import_tests")
            .getNonexistentSibling();
    work.createDirectory();
    const juce::File tmp = work.getChildFile("import-tmp");

    // --- a pedal-ready file passes through untouched ---
    //
    // "Pedal-ready" means float32, the format the pedal records in. Nothing
    // else earns the shortcut (issue #44).
    {
        const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 4410 });
        const juce::File src = writeTemp(work, "ready.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
    }

    // --- the reported case: plain 16-bit stereo 44.1 kHz IS converted ---
    //
    // The exact shape a beta tester's file had. It used to sail through the
    // gate untouched and land on the pedal as PCM, which the pedal would not
    // play; the fix is that it now goes through the converter like everything
    // that is not already float32.
    {
        const auto bytes = testkit::syntheticWav({ .tag = 1, .bits = 16, .frames = 4410 });
        const juce::File src = writeTemp(work, "sixteen-bit.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.converted);          // the whole point: it must NOT pass through
        CHECK(p.file != src);
        const wav::Info out = infoOf(p.file);
        CHECK_EQ(out.format(), "float32");
        CHECK_EQ(out.channels, 2);
        CHECK_EQ(out.sampleRate, 44100);
        CHECK_EQ(out.frames, 4410);  // same length, resampling is a no-op at 44.1
    }

    // --- the field case: extensible 24-bit stereo 44.1 kHz converts ---
    {
        const juce::File src = writeTemp(work, "daw-export.wav", extensiblePcm24(4410));
        CHECK_THROWS(wav::assertUploadable(infoOf(src)), "format"); // the old refusal
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.converted);
        const wav::Info out = infoOf(p.file);
        CHECK_EQ(out.format(), std::string("float32"));
        CHECK_EQ(out.sampleRate, wav::kSampleRate);
        CHECK_EQ(out.channels, 2);
        CHECK_EQ(out.frames, 4410);
    }

    // --- 48 kHz shrinks to 44.1 with the frame count scaled honestly ---
    {
        const auto bytes = testkit::syntheticWav({ .sampleRate = 48000, .frames = 48000 });
        const juce::File src = writeTemp(work, "48k.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.converted);
        const wav::Info out = infoOf(p.file);
        CHECK_EQ(out.sampleRate, wav::kSampleRate);
        CHECK_EQ(out.format(), std::string("float32"));
        const auto expected = static_cast<std::int64_t>(std::llround(48000.0 * 44100.0 / 48000.0));
        CHECK(std::llabs(out.frames - expected) <= 1);
    }

    // --- mono duplicates into both pedal channels, samples intact ---
    {
        const juce::File src = writeTemp(work, "mono.wav", monoRamp(1000));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.converted);
        const wav::Info out = infoOf(p.file);
        CHECK_EQ(out.channels, 2);
        CHECK_EQ(out.frames, 1000);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(p.file));
        CHECK(reader != nullptr);
        if (reader != nullptr) {
            juce::AudioBuffer<float> buf(2, 1000);
            reader->read(&buf, 0, 1000, 0, true, true);
            bool equal = true, nonZero = false;
            for (int i = 0; i < 1000; ++i) {
                equal = equal && std::abs(buf.getSample(0, i) - buf.getSample(1, i)) < 1.0e-6f;
                nonZero = nonZero || std::abs(buf.getSample(0, i)) > 1.0e-6f;
            }
            CHECK(equal);   // both channels carry the same signal...
            CHECK(nonZero); // ...and it is the ramp, not silence
        }
    }

    // --- more than two channels is a refusal, not a guessed downmix ---
    {
        const auto bytes = testkit::syntheticWav({ .channels = 4, .frames = 100 });
        const juce::File src = writeTemp(work, "quad.wav", bytes);
        wavimport::Prepared p;
        const juce::Result r = wavimport::prepare(src, tmp, p, {});
        CHECK(r.failed());
        CHECK(r.getErrorMessage().contains("channels"));
    }

    // --- not audio at all is a refusal with the filename in it ---
    {
        const juce::File src = work.getChildFile("not-audio.wav");
        src.replaceWithText("this is a text file wearing a wav extension");
        wavimport::Prepared p;
        const juce::Result r = wavimport::prepare(src, tmp, p, {});
        CHECK(r.failed());
        CHECK(r.getErrorMessage().contains("not-audio.wav"));
    }

    // --- normalize at import (issue #53): opt-in, measured, capped, honest ---

    // A pedal-ready quiet sine, normalize ON: rewritten to land on target —
    // the byte-exact promise deliberately traded away by the user's choice —
    // and the OUTPUT measures -18, frames intact.
    {
        const juce::File src = writeTemp(work, "quiet.wav", sineWav(3 * 44100, 2, dbAmp(-28.0)));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(p.converted);
        CHECK(p.file != src);
        CHECK(p.normalize.has_value());
        CHECK(p.normalize->measurable);
        CHECK(!p.normalize->cappedByPeak);
        CHECK_NEAR(p.normalize->measuredLufs, -28.0, 0.2);
        const auto out = measureLufs(p.file);
        CHECK(out.has_value());
        CHECK_NEAR(*out, -18.0, 0.2);
        CHECK_EQ(infoOf(p.file).frames, 3 * 44100); // gain moves no frames
    }

    // The same quiet pedal-ready file with normalize OFF: byte-exact
    // pass-through, exactly the pre-#53 behaviour.
    {
        const juce::File src = writeTemp(work, "quiet-off.wav", sineWav(44100, 2, dbAmp(-28.0)));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
        CHECK(!p.normalize.has_value());
    }

    // Already at target: pass through untouched, and say so — re-importing an
    // already-normalized file must stay a no-op.
    {
        const juce::File src = writeTemp(work, "attarget.wav", sineWav(44100, 2, dbAmp(-18.0)));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
        CHECK(p.normalize.has_value());
        CHECK(p.normalize->measurable);
        CHECK(p.normalize->untouched);
    }

    // The cap: a quiet body with one loud peak wants +22 dB but may only have
    // what the -1 dBTP ceiling leaves above the 0.5 spike — and the written
    // file's true peak proves it.
    {
        const juce::File src =
            writeTemp(work, "peaky.wav", sineWav(44100, 2, dbAmp(-40.0), 0.5f));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(p.converted);
        CHECK(p.normalize.has_value());
        CHECK(p.normalize->cappedByPeak);
        CHECK_NEAR(p.normalize->gainDb, -1.0 - 20.0 * std::log10(0.5), 0.1);
        double truePeakDb = 0.0;
        measureLufs(p.file, &truePeakDb);
        CHECK(truePeakDb <= -1.0 + 1.0e-3);
    }

    // Mono is measured as the stereo sum it will actually play as (issue
    // #53's open point, pinned): a -23 dBFS mono sine, duplicated, reads
    // -23 LUFS — the Tech 3341 stereo figure, 3 dB hotter than the -26 the
    // lone channel would meter at alone — and the gain is computed for THAT,
    // so the output, not the input, lands on target.
    {
        const juce::File src =
            writeTemp(work, "mono-sine.wav", sineWav(3 * 44100, 1, dbAmp(-23.0)));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(p.converted); // mono is never pedal-ready
        CHECK(p.normalize.has_value());
        CHECK_NEAR(p.normalize->measuredLufs, -23.0, 0.2); // post-duplication, not -26
        const auto out = measureLufs(p.file);
        CHECK(out.has_value());
        CHECK_NEAR(*out, -18.0, 0.2);
    }

    // Bytes that are not audio: a pedal-ready file with an impossible sample
    // imports untouched and the outcome names the damage — no gain is ever
    // computed from a "loudness" of garbage.
    {
        const juce::File src =
            writeTemp(work, "damaged.wav", sineWav(44100, 2, dbAmp(-28.0), 1.0e20f));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
        CHECK(p.normalize.has_value());
        CHECK(p.normalize->damaged);
        CHECK(!p.normalize->measurable);
        CHECK_EQ(p.normalize->wildSamples, 1);
    }

    // Digital silence is unmeasurable: a pedal-ready file imports untouched,
    // with the outcome saying why nothing was levelled.
    {
        const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 44100 });
        const juce::File src = writeTemp(work, "silence.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
        CHECK(p.normalize.has_value());
        CHECK(!p.normalize->measurable);
    }

    // Unmeasurable but wrong-shaped still converts — a silent 48 kHz file
    // becomes pedal-ready with no gain invented for it.
    {
        const auto bytes = testkit::syntheticWav({ .sampleRate = 48000, .frames = 48000 });
        const juce::File src = writeTemp(work, "silence48.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(p.converted);
        CHECK(p.normalize.has_value());
        CHECK(!p.normalize->measurable);
        CHECK(!measureLufs(p.file).has_value()); // still silence on the way out
    }

    // --- what the rebuild changed, as facts (issue #139) ---

    // The arrow the report draws — the one describeNormalize already uses,
    // so a note carrying both sentences reads with one.
    const std::string arrow = " \xe2\x86\x92 ";

    // 48 kHz, 24-bit, mono: every fact differs from the pedal's, and the
    // sentence says all three — source on the left, the pedal on the right.
    {
        const auto bytes = testkit::syntheticWav(
            { .tag = 1, .channels = 1, .sampleRate = 48000, .bits = 24, .frames = 4800 });
        const juce::File src = writeTemp(work, "daw-mono-48k.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.converted);
        CHECK(p.sourceFormat.has_value());
        if (p.sourceFormat.has_value()) {
            CHECK_EQ(p.sourceFormat->sampleRate, 48000);
            CHECK_EQ(p.sourceFormat->encoding.toStdString(), std::string("24-bit"));
            CHECK_EQ(p.sourceFormat->channels, 1);
            CHECK_EQ(wavimport::describeConversion(*p.sourceFormat).toStdString(),
                     "48000 Hz, 24-bit, mono" + arrow + "44100 Hz, 32-bit float, stereo");
        }
    }

    // A pedal-ready file passes through with no facts: there was no rebuild
    // to report, and no directory to own.
    {
        const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 4410 });
        const juce::File src = writeTemp(work, "ready-facts.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(!p.converted);
        CHECK(!p.sourceFormat.has_value());
        CHECK(p.jobDir.path() == juce::File());
    }

    // A pedal-ready file rewritten for its loudness alone IS a conversion,
    // and its facts match the pedal on every point — so the sentence is
    // empty: "32-bit float → 32-bit float" is nothing anyone needs told.
    {
        const juce::File src =
            writeTemp(work, "quiet-facts.wav", sineWav(3 * 44100, 2, dbAmp(-28.0)));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, { .normalizeTargetLufs = -18.0 }).wasOk());
        CHECK(p.converted);
        CHECK(p.sourceFormat.has_value());
        if (p.sourceFormat.has_value()) {
            CHECK_EQ(p.sourceFormat->sampleRate, 44100);
            CHECK_EQ(p.sourceFormat->encoding.toStdString(), std::string("32-bit float"));
            CHECK_EQ(p.sourceFormat->channels, 2);
            CHECK(wavimport::describeConversion(*p.sourceFormat).isEmpty());
        }
        CHECK(p.normalize.has_value());
        CHECK(p.normalize->measurable); // the gain is the only story here
    }

    // The field case keeps its bit depth as the fact, not the header's shape.
    {
        const juce::File src = writeTemp(work, "daw-export-facts.wav", extensiblePcm24(4410));
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
        CHECK(p.sourceFormat.has_value());
        if (p.sourceFormat.has_value())
            CHECK_EQ(wavimport::describeConversion(*p.sourceFormat).toStdString(),
                     "24-bit" + arrow + "32-bit float");
    }

    // --- the sentence says only what changed ---
    {
        using wavimport::SourceFormat;
        const auto say = [](const SourceFormat& f) {
            return wavimport::describeConversion(f).toStdString();
        };
        CHECK_EQ(say({ 48000, "24-bit", 1 }),
                 "48000 Hz, 24-bit, mono" + arrow + "44100 Hz, 32-bit float, stereo");
        CHECK_EQ(say({ 44100, "24-bit", 2 }), "24-bit" + arrow + "32-bit float");
        CHECK_EQ(say({ 44100, "32-bit float", 2 }), std::string(""));
        CHECK_EQ(say({ 44100, "16-bit", 2 }), "16-bit" + arrow + "32-bit float");
        CHECK_EQ(say({ 44100, "MP3", 2 }), "MP3" + arrow + "32-bit float");
        CHECK_EQ(say({ 48000, "32-bit float", 2 }), "48000 Hz" + arrow + "44100 Hz");
        CHECK_EQ(say({ 44100, "32-bit float", 1 }), "mono" + arrow + "stereo");
        CHECK_EQ(say({ 96000, "16-bit", 1 }),
                 "96000 Hz, 16-bit, mono" + arrow + "44100 Hz, 32-bit float, stereo");
        CHECK_EQ(say({ 22050, "8-bit", 2 }), "22050 Hz, 8-bit" + arrow + "44100 Hz, 32-bit float");
    }

    // --- the conversion's directory: one per job, gone with the job ---

    // A conversion lives in a directory of its own directly under import-tmp,
    // under a working name that is nobody's product: the name on the card is
    // decided at the push, not here.
    {
        const auto bytes = testkit::syntheticWav({ .sampleRate = 48000, .frames = 48000 });
        const juce::File src = writeTemp(work, "song.wav", bytes);
        juce::File dir;
        {
            wavimport::Prepared p;
            CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
            CHECK(p.converted);
            dir = p.jobDir.path();
            CHECK(dir.isDirectory());
            CHECK(p.file.getParentDirectory() == dir);
            CHECK(dir.getParentDirectory() == tmp);
            CHECK(!p.file.getFileName().contains("-pedal"));
            CHECK(!p.file.getFileName().startsWith("song"));
        }
        // the push succeeded and the job returned: the directory is gone
        CHECK(!dir.exists());
    }

    // The push threw: the directory is gone just the same — the job's scope
    // unwinds through the Prepared, whichever way it ends.
    {
        const auto bytes = testkit::syntheticWav({ .sampleRate = 48000, .frames = 48000 });
        const juce::File src = writeTemp(work, "song-throws.wav", bytes);
        juce::File dir;
        try {
            wavimport::Prepared p;
            CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
            dir = p.jobDir.path();
            CHECK(dir.isDirectory());
            throw Error("slot 9 already has audio");
        } catch (const Error&) {
        }
        CHECK(!dir.exists());
    }

    // Two files with the same stem in one queue: each conversion in its own
    // directory, neither renamed around the other, both readable at once.
    {
        const juce::File dirA = work.getChildFile("a");
        const juce::File dirB = work.getChildFile("b");
        dirA.createDirectory();
        dirB.createDirectory();
        const juce::File srcA = writeTemp(dirA, "song.wav",
                                          testkit::syntheticWav({ .sampleRate = 48000, .frames = 48000 }));
        const juce::File srcB = writeTemp(dirB, "song.wav", monoRamp(1000));
        {
            wavimport::Prepared a, b;
            CHECK(wavimport::prepare(srcA, tmp, a, {}).wasOk());
            CHECK(wavimport::prepare(srcB, tmp, b, {}).wasOk());
            CHECK(a.converted && b.converted);
            CHECK(a.file != b.file);
            CHECK(a.jobDir.path() != b.jobDir.path());
            CHECK(a.file.existsAsFile() && b.file.existsAsFile());
            CHECK(std::llabs(infoOf(a.file).frames - 44100) <= 1); // 48000 frames at 48 kHz
            CHECK_EQ(infoOf(b.file).frames, 1000);                 // the mono ramp, untouched
        }
        // both jobs done: import-tmp holds nothing of either
        CHECK_EQ(tmp.getNumberOfChildFiles(juce::File::findFilesAndDirectories), 0);
    }

    // The directory follows the Prepared that owns it through a move, and
    // dies with the last owner, not the first.
    {
        const auto bytes = testkit::syntheticWav({ .sampleRate = 48000, .frames = 4800 });
        const juce::File src = writeTemp(work, "song-moved.wav", bytes);
        juce::File dir;
        {
            wavimport::Prepared moved;
            {
                wavimport::Prepared p;
                CHECK(wavimport::prepare(src, tmp, p, {}).wasOk());
                dir = p.jobDir.path();
                moved = std::move(p);
            }
            CHECK(dir.isDirectory()); // the first owner is gone, the directory is not
            CHECK(moved.file.existsAsFile());
        }
        CHECK(!dir.exists());
    }

    // Every Prepared above is gone; a cleanup that fails is a failure.
    CHECK(work.deleteRecursively());
    return testkit::summary("wav_import");
}
