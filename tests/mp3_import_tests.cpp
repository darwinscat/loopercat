// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Mp3AudioFormat against the THEORY of what a gapless MP3 decode owes a
// looper (issue #52): the LAME encoder delay and padding must be gone — the
// decoded length is EXACTLY the source length and the downbeat sits at frame
// 0 — the channels keep their identities, seeks land where they claim, and
// the import chain turns the file into a pedal-canonical float32 WAV.
//
// The fixture (fixtures/tone-440-660-1s.mp3) is defined mathematically:
// exactly 44100 frames of 44.1 kHz stereo, left = 0.5·sin(2π·440·k/44100),
// right = 0.5·sin(2π·660·k/44100), 16-bit PCM, encoded once with
//   lame --cbr -b 192 -q 2  (LAME 4.0)
// so it carries the Xing/LAME gapless tag. An UNTRIMMED decode would come
// out ~1100+ frames longer with leading junk — the checks below falsify that.

#include "support.hpp"

#include "../app/Mp3AudioFormat.h"
#include "../app/WavImport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <loopercat/Wav.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace loopercat;

namespace
{

constexpr int kFrames = 44100; // the fixture's exact source length

juce::File fixtureFile()
{
    return juce::File(LOOPERCAT_MP3_FIXTURE);
}

std::unique_ptr<juce::AudioFormatReader> openFixture()
{
    // The import chain's exact registration order (WavImport.cpp): the
    // bundled decoder first, the JUCE basics after.
    juce::AudioFormatManager formats;
    formats.registerFormat(new Mp3AudioFormat(), false);
    formats.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(fixtureFile()));
}

// Zero crossings of a sampled sine count its frequency: a tone at f Hz
// crosses zero 2·f times per second. Codec ripple jitters samples near zero,
// so crossings are counted with a small hysteresis dead-band.
int zeroCrossings(const float* x, int n)
{
    int crossings = 0;
    float prev = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float v = std::abs(x[i]) < 0.01f ? prev : x[i];
        if (prev != 0.0f && v != 0.0f && std::signbit(v) != std::signbit(prev))
            ++crossings;
        if (v != 0.0f)
            prev = v;
    }
    return crossings;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // --- the reader tells the mathematical truth about the fixture ---
    auto reader = openFixture();
    CHECK(reader != nullptr);
    if (reader == nullptr)
        return testkit::summary("mp3_import");
    CHECK_EQ(static_cast<int>(reader->sampleRate), 44100);
    CHECK_EQ(static_cast<int>(reader->numChannels), 2);
    CHECK(reader->usesFloatingPointData); // float — the pedal's own sample format
    // Gapless: EXACTLY the source length; delay/padding frames would inflate it.
    CHECK_EQ(static_cast<long long>(reader->lengthInSamples), static_cast<long long>(kFrames));

    // --- the decoded signal is the fixture's signal ---
    juce::AudioBuffer<float> all(2, kFrames);
    CHECK(reader->read(&all, 0, kFrames, 0, true, true));
    {
        // The downbeat is at frame 0: with the encoder delay untrimmed the
        // first ~1100 samples would be near-silence.
        float head = 0.0f;
        for (int i = 0; i < 32; ++i)
            head = std::max(head, std::abs(all.getSample(0, i)));
        CHECK(head > 0.1f);

        const int left = zeroCrossings(all.getReadPointer(0), kFrames);
        const int right = zeroCrossings(all.getReadPointer(1), kFrames);
        CHECK(std::abs(left - 880) <= 18);   // 440 Hz counts as 880 crossings/s
        CHECK(std::abs(right - 1320) <= 26); // 660 Hz counts as 1320 crossings/s
        CHECK(right > left + 300);           // channels kept their identities

        double body = 0.0; // the tone has body, not near-silence
        for (int i = 0; i < kFrames; ++i)
            body += std::abs(all.getSample(0, i));
        CHECK(body / kFrames > 0.2);
    }

    // --- a seek lands where it claims ---
    {
        auto fresh = openFixture();
        juce::AudioBuffer<float> window(2, 512);
        CHECK(fresh->read(&window, 0, 512, 10000, true, true));
        float worst = 0.0f; // tolerance, not bit-equality: the synthesis state
        for (int i = 0; i < 512; ++i) // after a cold seek may differ in ripple
            worst = std::max(worst, std::abs(window.getSample(0, i) - all.getSample(0, 10000 + i)));
        CHECK(worst < 0.02f);
    }

    // --- the import chain turns the mp3 into a pedal-canonical WAV ---
    {
        const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("loopercat-mp3-tests")
                                       .getNonexistentSibling();
        wavimport::Prepared out;
        const juce::Result r = wavimport::prepare(fixtureFile(), tempDir, out, {});
        CHECK(r.wasOk());
        if (r.wasOk()) {
            CHECK(out.converted); // an mp3 is always a conversion, never a pass-through
            juce::MemoryBlock raw;
            CHECK(out.file.loadFileAsData(raw));
            const auto info = wav::readWavInfo(wav::BytesView(
                static_cast<const unsigned char*>(raw.getData()), raw.getSize()));
            CHECK_EQ(static_cast<int>(info.sampleRate), 44100);
            CHECK_EQ(info.channels, 2);
            // Gapless end to end: the pedal file carries the source length exactly.
            CHECK_EQ(static_cast<long long>(info.frames), static_cast<long long>(kFrames));
            bool uploadable = true;
            try {
                wav::assertUploadable(info);
            } catch (...) {
                uploadable = false;
            }
            CHECK(uploadable);
            out.file.deleteFile();
        }
        tempDir.deleteRecursively();
    }

    // --- garbage wearing .mp3 fails plainly ---
    {
        const juce::File tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getChildFile("loopercat-mp3-tests-garbage")
                                       .getNonexistentSibling();
        tempDir.createDirectory();
        const juce::File fake = tempDir.getChildFile("not-audio.mp3");
        std::vector<unsigned char> noise(4096);
        for (size_t i = 0; i < noise.size(); ++i)
            noise[i] = static_cast<unsigned char>((i * 37 + 11) & 0xff);
        fake.replaceWithData(noise.data(), noise.size());

        wavimport::Prepared out;
        const juce::Result r = wavimport::prepare(fake, tempDir, out, {});
        CHECK(r.failed());
        CHECK(r.getErrorMessage().contains("not an audio file"));
        tempDir.deleteRecursively();
    }

    return testkit::summary("mp3_import");
}
