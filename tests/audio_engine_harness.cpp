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

    // --- play at the end starts over, not a dead click ---

    engine.play();
    {
        const int at = pumpUntilAudible(engine, left, right, 5000);
        CHECK(at >= 0);
        if (at >= 0)
            CHECK(frameOfSample(left[static_cast<std::size_t>(at)]) < kFrames / 4);
    }
    engine.stop();

    engine.audioDeviceStopped();
    wavFile.deleteFile();

    return testkit::summary("audio_engine_harness");
}
