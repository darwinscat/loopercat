// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the playback engine: the REAL AudioEngine pumped
// through its device-callback seam with a fake device — no audio hardware,
// no test-only API. The fixture wav encodes its frame index into every
// sample (the ramp), so the output stream proves WHERE the engine is
// playing from, not just that something is audible:
//
//   - misuse first: load a missing file, play with nothing loaded
//   - loaded: length is right, samples match the ramp from frame 0
//   - stop silences and freezes; seek resumes at the sought frame
//   - looping wraps past the end back to frame 0
//   - non-looping runs dry at the end (silence, transport halts)
//   - a two-track memory plays as its mix: solo is one take exactly, the mix
//     is their sum at their levels, the shorter track wraps on its own
//     length as gaplessly as the section loop, one clock moves both

#include "support.hpp"

#include "../app/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <fstream>

using namespace loopercat;

namespace {

constexpr int kFrames = 44100; // 1.0 s
constexpr int kBlock = 512;

// The least AudioIODevice that lets AudioSourcePlayer prepare: 44.1k, 512.
struct FakeDevice final : juce::AudioIODevice {
    FakeDevice() : juce::AudioIODevice("fake", "fake-type") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return false; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return 44100.0; }
    int getCurrentBitDepth() override { return 16; }
    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger b;
        b.setRange(0, 2, true);
        return b;
    }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

// Pump one block through the engine's device callback.
void pumpBlock(AudioEngine& engine, std::vector<float>& left, std::vector<float>& right)
{
    left.assign(kBlock, -2.0f); // poison: proves the callback overwrites
    right.assign(kBlock, -2.0f);
    float* outs[] = { left.data(), right.data() };
    engine.audioDeviceIOCallbackWithContext(nullptr, 0, outs, 2, kBlock, {});
}

bool isSilent(const std::vector<float>& samples)
{
    for (const float s : samples)
        if (!juce::exactlyEqual(s, 0.0f))
            return false;
    return true;
}

// Frame index encoded in a ramp sample (left channel), rounded.
int frameOfSample(float sample)
{
    return static_cast<int>(std::lround(static_cast<double>(sample) * 32768.0)) + kFrames / 2;
}

// Pump until a non-silent block arrives (the read-ahead thread needs a
// moment to prime); returns the index of the first non-silent sample, or -1.
int pumpUntilAudible(AudioEngine& engine, std::vector<float>& left, std::vector<float>& right,
                     const int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (juce::Time::getMillisecondCounterHiRes() < deadline) {
        pumpBlock(engine, left, right);
        for (int i = 0; i < kBlock; ++i)
            if (!juce::exactlyEqual(left[static_cast<std::size_t>(i)], 0.0f))
                return i;
        juce::Thread::sleep(20);
    }
    return -1;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const juce::File wavFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("loopercat-engine-" + juce::String(stamp) + ".wav");
    {
        const auto bytes = testkit::syntheticWav({ .frames = kFrames, .rampFill = true });
        std::ofstream out(wavFile.getFullPathName().toStdString(), std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    AudioEngine engine; // NB: no initialiseDevice() — fully headless
    std::vector<float> left, right;

    // --- misuse before anything is loaded ---

    {
        const auto result = engine.load(juce::File("/nonexistent/loopercat/nope.wav"));
        CHECK(result.failed());
        CHECK(result.getErrorMessage().contains("nope.wav"));
        CHECK(!engine.hasSource());

        engine.play(); // nothing loaded: must be a no-op, not a crash
        CHECK(!engine.isPlaying());

        FakeDevice device;
        engine.audioDeviceAboutToStart(&device);
        pumpBlock(engine, left, right); // callback with no source: silence
        CHECK(isSilent(left) && isSilent(right));
        engine.audioDeviceStopped();
    }

    // --- load + play from frame 0 ---

    FakeDevice device;
    engine.audioDeviceAboutToStart(&device);

    CHECK(engine.load(wavFile).wasOk());
    CHECK(engine.hasSource());
    CHECK(std::abs(engine.lengthSeconds() - 1.0) < 0.001);
    CHECK(!engine.isPlaying());

    engine.setLooping(false);
    engine.play();
    CHECK(engine.isPlaying());
    {
        const int at = pumpUntilAudible(engine, left, right, 5000);
        CHECK(at >= 0);
        if (at >= 0) {
            const int frame = frameOfSample(left[static_cast<std::size_t>(at)]);
            // Buffer priming may cost whole silent blocks, but never data from
            // anywhere other than the start of the file.
            CHECK(frame >= 0 && frame < kFrames / 4);
            // Continuity + stereo mirror: the next samples are the next frames.
            int broken = 0;
            for (int i = at; i < kBlock; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                if (std::abs(left[idx] - testkit::rampSample(frame + (i - at), kFrames)) > 1.0e-6f)
                    ++broken;
                if (std::abs(left[idx] + right[idx]) > 1.0e-6f)
                    ++broken;
            }
            CHECK_EQ(broken, 0);
        }
    }

    // --- stop: silence and a frozen position, after the anti-click fade ---

    engine.stop();
    CHECK(!engine.isPlaying());
    pumpBlock(engine, left, right); // the transport fades this block out
    pumpBlock(engine, left, right);
    const double frozen = engine.positionSeconds();
    pumpBlock(engine, left, right);
    CHECK(isSilent(left) && isSilent(right));
    CHECK(juce::exactlyEqual(engine.positionSeconds(), frozen));

    // --- seek: playback resumes at (never before) the sought frame ---

    engine.setPosition(0.5);
    engine.play();
    {
        const int at = pumpUntilAudible(engine, left, right, 5000);
        CHECK(at >= 0);
        if (at >= 0) {
            const int frame = frameOfSample(left[static_cast<std::size_t>(at)]);
            CHECK(frame >= kFrames / 2);
            CHECK(frame < kFrames / 2 + kFrames / 4);
        }
    }
    engine.stop();

    // --- looping wraps: after the last frame comes frame 0 ---

    engine.setLooping(true);
    engine.setPosition(0.99); // 441 frames from the end
    engine.play();
    {
        bool sawWrap = false;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        int previousFrame = -1;
        while (!sawWrap && juce::Time::getMillisecondCounterHiRes() < deadline) {
            pumpBlock(engine, left, right);
            for (int i = 0; i < kBlock; ++i) {
                const float s = left[static_cast<std::size_t>(i)];
                if (juce::exactlyEqual(s, 0.0f))
                    continue; // priming silence
                const int frame = frameOfSample(s);
                if (previousFrame > kFrames * 3 / 4 && frame < kFrames / 4)
                    sawWrap = true;
                previousFrame = frame;
            }
            juce::Thread::sleep(10);
        }
        CHECK(sawWrap);
    }
    engine.stop();

    // --- non-looping runs dry at the end ---

    engine.setLooping(false);
    engine.setPosition(0.99);
    engine.play();
    {
        // Drain: shortly after the end there must be nothing but silence.
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        int silentStreak = 0;
        while (silentStreak < 20 && juce::Time::getMillisecondCounterHiRes() < deadline) {
            pumpBlock(engine, left, right);
            silentStreak = isSilent(left) ? silentStreak + 1 : 0;
            juce::Thread::sleep(5);
        }
        CHECK_EQ(silentStreak, 20);
        CHECK(engine.positionSeconds() >= engine.lengthSeconds() - 0.05);
    }

    // --- section preview: the marker loop wraps sample-exactly ---

    {
        engine.setLooping(false); // the section, not the file loop, must wrap
        engine.setSection(0.25, 0.5); // frames [11025, 22050)
        engine.setPosition(0.3);
        engine.play();

        const int sectionStart = kFrames / 4, sectionEnd = kFrames / 2;
        int wraps = 0, outOfSection = 0, discontinuities = 0, previousFrame = -1;
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
        while (wraps < 2 && juce::Time::getMillisecondCounterHiRes() < deadline) {
            pumpBlock(engine, left, right);
            for (int i = 0; i < kBlock; ++i) {
                const float s = left[static_cast<std::size_t>(i)];
                if (juce::exactlyEqual(s, 0.0f))
                    continue; // priming silence only — counted via discontinuity below
                const int frame = frameOfSample(s);
                if (frame < sectionStart || frame >= sectionEnd)
                    ++outOfSection;
                if (previousFrame >= 0) {
                    if (previousFrame == sectionEnd - 1 && frame == sectionStart)
                        ++wraps; // the seam, sample-adjacent
                    else if (frame != previousFrame + 1)
                        ++discontinuities;
                }
                previousFrame = frame;
            }
            juce::Thread::sleep(5);
        }
        CHECK(wraps >= 2);
        CHECK_EQ(outOfSection, 0);
        // The wrap seam must be gapless: the only allowed jumps are the wraps
        // themselves; one initial jump from priming silence is tolerated.
        CHECK(discontinuities <= 1);

        engine.stop();
        engine.clearSection();
    }

    // --- play at the end starts over, not a dead click ---

    engine.setPosition(engine.lengthSeconds()); // the precondition, explicit
    engine.play();
    {
        const int at = pumpUntilAudible(engine, left, right, 5000);
        CHECK(at >= 0);
        if (at >= 0)
            CHECK(frameOfSample(left[static_cast<std::size_t>(at)]) < kFrames / 4);
    }
    engine.stop();

    // --- a two-track memory: the mix, solo, and each track on its own loop ---
    //
    // Track 1 is the one-second ramp above; track 2 a half-second ramp at
    // half level — the pedal's 4 bars under 8. Solo hands over one take
    // exactly; the mix is the sum of both at their levels; track 2 wraps at
    // its own end, twice under one pass of track 1, held to the section
    // loop's gaplessness; and one clock moves both tracks.
    {
        const juce::File wav2 =
            juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("loopercat-engine-t2-" + juce::String(stamp) + ".wav");
        constexpr int kFrames2 = kFrames / 2;
        constexpr float kGain2 = 0.5f;
        {
            const auto bytes = testkit::syntheticWav({ .frames = kFrames2, .rampFill = true });
            std::ofstream out(wav2.getFullPathName().toStdString(), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        // Track 2's ramp is centred on its own half-length, and heard at the
        // track's level: the decode divides the level back out.
        const auto frameOfSample2 = [](float sample, float gain) {
            return static_cast<int>(std::lround(static_cast<double>(sample / gain) * 32768.0))
                 + kFrames2 / 2;
        };

        CHECK(engine.loadMix({ { wavFile, 1.0f }, { wav2, kGain2 } }).wasOk());
        CHECK(engine.hasSource());
        CHECK_EQ(engine.mixTrackCount(), 2);
        CHECK(std::abs(engine.lengthSeconds() - 1.0) < 0.001); // the longest track
        engine.setLooping(true);

        // Solo T1: track 1's ramp, exactly and continuously, from the start.
        engine.setSolo(1);
        engine.setPosition(0);
        engine.play();
        {
            const int at = pumpUntilAudible(engine, left, right, 5000);
            CHECK(at >= 0);
            if (at >= 0) {
                int broken = 0;
                int previous = frameOfSample(left[static_cast<std::size_t>(at)]);
                CHECK(previous >= 0 && previous < kFrames / 4);
                for (int i = at + 1; i < kBlock; ++i) {
                    const int frame = frameOfSample(left[static_cast<std::size_t>(i)]);
                    if (frame != previous + 1)
                        ++broken;
                    previous = frame;
                }
                CHECK_EQ(broken, 0);
            }
        }

        // The transport's resampler keeps three samples of look-ahead from the
        // block before, so the first block after a solo change carries three
        // samples of the old state: it is pumped and set aside.
        constexpr int kLookAhead = 3;
        // A mix whose read-ahead has not caught up pauses: silence, the clock
        // standing — on a loaded machine that shows as a run of exact zeros
        // with no frame lost on either side. Two zeros in a row is that
        // pause; the ramp's own single zero at its centre is a frame.
        const auto paused = [](const std::vector<float>& block) {
            for (std::size_t i = 1; i < block.size(); ++i)
                if (juce::exactlyEqual(block[i], 0.0f) && juce::exactlyEqual(block[i - 1], 0.0f))
                    return true;
            return false;
        };

        // Solo T2: track 2's ramp, exactly, wrapping at ITS end — the only
        // jumps allowed are its wraps, as the section loop is held to.
        {
            engine.setSolo(2);
            pumpBlock(engine, left, right); // the switch block
            int wraps = 0, discontinuities = 0, outside = 0, previous = -1;
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
            while (wraps < 2 && juce::Time::getMillisecondCounterHiRes() < deadline) {
                pumpBlock(engine, left, right);
                for (int i = 0; i < kBlock; ++i) {
                    const float s = left[static_cast<std::size_t>(i)];
                    if (juce::exactlyEqual(s, 0.0f) && previous + 1 != kFrames2 / 2)
                        continue; // the mix paused for its read-ahead; nothing is lost
                    const int frame = frameOfSample2(s, kGain2);
                    if (frame < 0 || frame >= kFrames2)
                        ++outside;
                    if (previous >= 0) {
                        if (previous == kFrames2 - 1 && frame == 0)
                            ++wraps;
                        else if (frame != previous + 1)
                            ++discontinuities;
                    }
                    previous = frame;
                }
                juce::Thread::sleep(5);
            }
            CHECK(wraps >= 2);
            CHECK_EQ(outside, 0);
            CHECK(discontinuities <= 1);
        }

        // The mix: both ramps at their levels on ONE clock. The clock is read
        // off a solo block, and the very next block is the sum — a solo or a
        // level change is heard at the next callback, not a read-ahead later.
        {
            int mismatches = -1;
            for (int attempt = 0; attempt < 20 && mismatches != 0; ++attempt) {
                engine.setSolo(1);
                pumpBlock(engine, left, right); // the switch block
                pumpBlock(engine, left, right);
                if (paused(left))
                    continue; // the clock stood somewhere in this block: read it again
                const int f0 = frameOfSample(left[0]);
                engine.setSolo(0);
                pumpBlock(engine, left, right);
                if (paused(left))
                    continue;
                mismatches = 0;
                for (int i = kLookAhead; i < kBlock; ++i) {
                    const int f = f0 + kBlock + i;
                    const float expected = testkit::rampSample(f % kFrames, kFrames)
                                         + kGain2 * testkit::rampSample(f % kFrames2, kFrames2);
                    if (std::abs(left[static_cast<std::size_t>(i)] - expected) > 1.0e-4f)
                        ++mismatches;
                }
            }
            CHECK_EQ(mismatches, 0);
        }

        // One clock: after a seek, track 2 sits where track 1 sits, folded
        // into its own length.
        {
            engine.setSolo(1);
            engine.setPosition(0.6);
            const int at = pumpUntilAudible(engine, left, right, 5000);
            CHECK(at >= 0);
            if (at >= 0) {
                // The block that follows a solo-1 block starts kBlock frames on;
                // past the resampler's look-ahead the next block is track 2's.
                int f1 = -1, f2 = -2;
                for (int attempt = 0; attempt < 20 && f2 != (f1 + kBlock + kLookAhead) % kFrames2;
                     ++attempt) {
                    engine.setSolo(1);
                    pumpBlock(engine, left, right); // the switch block
                    pumpBlock(engine, left, right);
                    if (paused(left))
                        continue;
                    f1 = frameOfSample(left[0]);
                    engine.setSolo(2);
                    pumpBlock(engine, left, right);
                    if (paused(left))
                        continue;
                    f2 = frameOfSample2(left[kLookAhead], kGain2);
                }
                CHECK(f1 >= static_cast<int>(0.6 * kFrames));
                CHECK_EQ(f2, (f1 + kBlock + kLookAhead) % kFrames2);
            }
        }

        // The section loop on the mix: track 2 follows the section in the
        // mix's frames, wrapping back with the same seam the one-track path
        // is held to. The section avoids the ramp's zero crossing (its
        // centre), which the priming-silence skip below could not tell apart.
        {
            engine.setSolo(2);
            constexpr int sectionStart = 13230, sectionEnd = 19845; // 0.3 s .. 0.45 s
            engine.setSection(0.3, 0.45);
            engine.setPosition(0.35);
            pumpBlock(engine, left, right); // the switch block
            int wraps = 0, discontinuities = 0, outside = 0, previous = -1;
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 5000;
            while (wraps < 2 && juce::Time::getMillisecondCounterHiRes() < deadline) {
                pumpBlock(engine, left, right);
                for (int i = 0; i < kBlock; ++i) {
                    const float s = left[static_cast<std::size_t>(i)];
                    if (juce::exactlyEqual(s, 0.0f))
                        continue; // priming silence after the seek
                    const int frame = frameOfSample2(s, kGain2);
                    if (frame < sectionStart || frame >= sectionEnd)
                        ++outside;
                    if (previous >= 0) {
                        if (previous == sectionEnd - 1 && frame == sectionStart)
                            ++wraps;
                        else if (frame != previous + 1)
                            ++discontinuities;
                    }
                    previous = frame;
                }
                juce::Thread::sleep(5);
            }
            CHECK(wraps >= 2);
            CHECK_EQ(outside, 0);
            CHECK(discontinuities <= 1);
            engine.clearSection();
        }

        // A pause never costs a frame. A seek back behind the read-ahead
        // leaves every track's buffer empty at the new position; blocks
        // pumped before it has caught up are silence with the clock standing,
        // so the clock moves by exactly the samples that were heard — and
        // what is heard next is where the seek went, not somewhere past it.
        {
            engine.setSolo(1);
            engine.clearSection();
            engine.setPosition(0.8);
            (void) pumpUntilAudible(engine, left, right, 5000);
            engine.setPosition(0.0); // behind the buffers: a miss on both tracks
            int heard = 0;
            for (int block = 0; block < 8; ++block) { // no pacing: faster than any refill
                pumpBlock(engine, left, right);
                for (int i = 0; i < kBlock; ++i)
                    heard += juce::exactlyEqual(left[static_cast<std::size_t>(i)], 0.0f) ? 0 : 1;
            }
            const auto clock = static_cast<int>(std::lround(engine.positionSeconds() * 44100.0));
            std::printf("PAUSE heard %d samples of %d, clock at %d\n", heard, 8 * kBlock, clock);
            // The resampler's look-ahead makes the clock run a few samples
            // ahead of what has been output; a whole block is a pause.
            CHECK(std::abs(clock - heard) <= 2 * kLookAhead + 1);
            juce::Thread::sleep(80);
            const int at = pumpUntilAudible(engine, left, right, 5000);
            CHECK(at >= 0);
            if (at >= 0)
                CHECK(frameOfSample(left[static_cast<std::size_t>(at)]) <= heard + kBlock);
        }

        engine.stop();
        engine.setSolo(0);
        // A memory whose take sits on track 2 alone: track 1 is a silent
        // lane, track 2 plays — the mix is track 2 at its level.
        {
            CHECK(engine.loadMix({ { juce::File(), 1.0f }, { wav2, 1.0f } }).wasOk());
            CHECK_EQ(engine.mixTrackCount(), 2);
            CHECK(std::abs(engine.lengthSeconds() - 0.5) < 0.001);
            engine.setPosition(0);
            engine.play();
            const int at = pumpUntilAudible(engine, left, right, 5000);
            CHECK(at >= 0);
            if (at >= 0)
                CHECK(frameOfSample2(left[static_cast<std::size_t>(at)], 1.0f) < kFrames2 / 4);
            engine.stop();
        }
        // Nothing to play at all is refused, not silence.
        CHECK(engine.loadMix({ { juce::File(), 1.0f }, { juce::File(), 1.0f } }).failed());
        CHECK(!engine.hasSource());
        wav2.deleteFile();
    }

    engine.audioDeviceStopped();
    wavFile.deleteFile();

    return testkit::summary("audio_engine_harness");
}
