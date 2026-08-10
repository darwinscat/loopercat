// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// wavimport::prepare against the THEORY of what the pedal accepts (issue
// #20): a file the pedal takes as-is must pass through untouched; everything
// else JUCE reads becomes 44.1 kHz stereo float32; what cannot become that
// fails plainly. The field case that started this: DAW 24-bit exports wear
// WAVE_FORMAT_EXTENSIBLE headers the strict core parser refuses.

#include "support.hpp"

#include "../app/WavImport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <loopercat/Wav.hpp>

#include <cmath>

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
    {
        const auto bytes = testkit::syntheticWav({ .frames = 4410 });
        const juce::File src = writeTemp(work, "ready.wav", bytes);
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p).wasOk());
        CHECK(!p.converted);
        CHECK(p.file == src);
    }

    // --- the field case: extensible 24-bit stereo 44.1 kHz converts ---
    {
        const juce::File src = writeTemp(work, "daw-export.wav", extensiblePcm24(4410));
        CHECK_THROWS(wav::assertUploadable(infoOf(src)), "format"); // the old refusal
        wavimport::Prepared p;
        CHECK(wavimport::prepare(src, tmp, p).wasOk());
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
        CHECK(wavimport::prepare(src, tmp, p).wasOk());
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
        CHECK(wavimport::prepare(src, tmp, p).wasOk());
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
        const juce::Result r = wavimport::prepare(src, tmp, p);
        CHECK(r.failed());
        CHECK(r.getErrorMessage().contains("channels"));
    }

    // --- not audio at all is a refusal with the filename in it ---
    {
        const juce::File src = work.getChildFile("not-audio.wav");
        src.replaceWithText("this is a text file wearing a wav extension");
        wavimport::Prepared p;
        const juce::Result r = wavimport::prepare(src, tmp, p);
        CHECK(r.failed());
        CHECK(r.getErrorMessage().contains("not-audio.wav"));
    }

    work.deleteRecursively();
    return testkit::summary("wav_import");
}
