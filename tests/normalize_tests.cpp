// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The byte-level normalize pair (issue #53) against the THEORY of what it
// must guarantee, never the implementation:
//
//   1. measureLoudness reads the same figures off a file's bytes that the
//      streaming meter reads off the stream (Tech 3341's tone is the anchor);
//   2. withGainDb multiplies every sample by exactly one scale — and nothing
//      else: frames, format and layout survive, DAW-added chunks are
//      stripped to the canonical shape, silence stays bit-exact silence
//      (sign included), because a gain cannot invent signal;
//   3. gain is linear through the meter: measure(withGain(x, g)) lands at
//      measure(x) + g — the property the whole feature stands on;
//   4. anything that is not the pedal's own stereo float32 is refused with
//      an honest verb, not decoded, not guessed at.

#include "support.hpp"

#include <loopercat/Normalize.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

using namespace loopercat;

namespace {

// A float32 stereo 44.1 kHz WAV of a 997 Hz sine at `dbfs` peak, identical
// in both channels. `spike` plants one 0-frame sample on channel 0 (the
// quiet-but-peaky shape); frame 1 of channel 0 is forced to -0.0 so every
// rewrite proves it keeps signed silence bit-exact. `extraChunk` inserts a
// LIST chunk before the audio — the DAW-export shape canonicalize strips.
std::vector<unsigned char> sineWav(int frames, double dbfs, float spike = 0.0f,
                                   bool extraChunk = false)
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
    const int extra = extraChunk ? 8 + 26 : 0;
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(4 + 8 + 16 + extra + 8 + dataSize); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(44100); p32(44100LL * 8); p16(8); p16(32);
    if (extraChunk) {
        ascii("LIST"); p32(26);
        b.insert(b.end(), 26, 0);
    }
    ascii("data"); p32(dataSize);
    const double amp = std::pow(10.0, dbfs / 20.0);
    const double w = 2.0 * std::numbers::pi * 997.0 / 44100.0;
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(amp * std::sin(w * i));
        if (i == 0 && spike > 0.0f)
            pf(spike);
        else if (i == 1)
            pf(-0.0f);
        else
            pf(v);
        pf(v);
    }
    return b;
}

wav::BytesView view(const std::vector<unsigned char>& b) { return { b.data(), b.size() }; }

float sampleAt(const wav::Bytes& bytes, std::int64_t frame, std::size_t channel)
{
    const std::size_t o =
        wav::kCanonicalFloatDataStart + static_cast<std::size_t>(frame) * 8 + channel * 4;
    const std::uint32_t bits = std::uint32_t{ bytes[o] } | std::uint32_t{ bytes[o + 1] } << 8
                             | std::uint32_t{ bytes[o + 2] } << 16
                             | std::uint32_t{ bytes[o + 3] } << 24;
    return std::bit_cast<float>(bits);
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
    ::checkNear((actual), (expected), (tol), #actual " ~= " #expected, __FILE__, __LINE__)

} // namespace

int main()
{
    // --- measureLoudness: the bytes read like the stream ---

    {
        // Tech 3341's anchor tone, one second of it: -23 dBFS reads -23 LUFS.
        const auto bytes = sineWav(44100, -23.0);
        const wav::LoudnessReading reading = wav::measureLoudness(view(bytes));
        CHECK(reading.integratedLufs.has_value());
        CHECK_NEAR(*reading.integratedLufs, -23.0, 0.1);
    }
    {
        // The peak meter reads the raw spike bit-exactly — and the true peak
        // of a lone grid sample IS that sample: a band-limited impulse crests
        // on its own grid point, so the reconstruction adds nothing above
        // -6.02 dBTP (the -40 dBFS body underneath is 34 dB down).
        const auto bytes = sineWav(44100, -40.0, 0.5f);
        const wav::LoudnessReading reading = wav::measureLoudness(view(bytes));
        CHECK_EQ(std::bit_cast<std::uint32_t>(reading.samplePeak),
                 std::bit_cast<std::uint32_t>(0.5f));
        CHECK_NEAR(reading.truePeakDb, 20.0 * std::log10(0.5), 0.2);
    }
    {
        // Digital silence: measurable is an answer, and the answer is no.
        const auto bytes = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 44100 });
        const wav::LoudnessReading reading = wav::measureLoudness(view(bytes));
        CHECK(!reading.integratedLufs.has_value());
        CHECK_EQ(std::bit_cast<std::uint32_t>(reading.samplePeak),
                 std::bit_cast<std::uint32_t>(0.0f));
        CHECK(std::isinf(reading.truePeakDb) && reading.truePeakDb < 0.0);
    }
    {
        // Shorter than one 400 ms gating block: no reading, not a guess.
        const auto bytes = sineWav(4410, -23.0); // 100 ms
        CHECK(!wav::measureLoudness(view(bytes)).integratedLufs.has_value());
    }
    {
        // Refusals name what they refuse.
        const auto pcm = testkit::syntheticWav({ .tag = 1, .bits = 16, .frames = 100 });
        CHECK_THROWS(wav::measureLoudness(view(pcm)), "32-bit float");
        CHECK_THROWS(wav::withGainDb(view(pcm), 1.0), "32-bit float");
        const auto mono =
            testkit::syntheticWav({ .tag = 3, .channels = 1, .bits = 32, .frames = 100 });
        CHECK_THROWS(wav::measureLoudness(view(mono)), "stereo");
    }

    // --- withGainDb: one scale, nothing else ---

    {
        const double gain = 20.0 * std::log10(2.0); // scale of exactly x2 in dB
        const auto original = sineWav(4410, -23.0, 0.0f, /*extraChunk=*/true);
        const wav::Bytes scaled = wav::withGainDb(view(original), gain);

        // Canonical shape: the LIST chunk is gone, samples at the known offset.
        const wav::Info info = wav::readWavInfo(scaled);
        CHECK_EQ(info.format(), std::string("float32"));
        CHECK_EQ(info.channels, 2);
        CHECK_EQ(info.frames, 4410);

        // Every sample times two, within float rounding of the dB round-trip…
        const wav::Bytes canon = wav::canonicalize(view(original));
        bool allScaled = true;
        for (std::int64_t f = 0; f < info.frames; ++f)
            for (std::size_t c = 0; c < 2; ++c) {
                const float was = sampleAt(canon, f, c);
                const float now = sampleAt(scaled, f, c);
                allScaled = allScaled && std::abs(now - was * 2.0f) <= 1.0e-6f;
            }
        CHECK(allScaled);

        // …and signed silence is STILL signed silence, bit for bit: a gain
        // cannot invent signal, so -0.0 must not become anything else.
        CHECK_EQ(std::bit_cast<std::uint32_t>(sampleAt(scaled, 1, 0)),
                 std::bit_cast<std::uint32_t>(-0.0f));
    }

    // --- gain is linear through the meter: the feature's load-bearing wall ---

    {
        const auto quiet = sineWav(44100, -30.0);
        const wav::Bytes louder = wav::withGainDb(view(quiet), 7.0);
        const auto before = wav::measureLoudness(view(quiet)).integratedLufs;
        const auto after =
            wav::measureLoudness(wav::BytesView(louder.data(), louder.size())).integratedLufs;
        CHECK(before.has_value() && after.has_value());
        CHECK_NEAR(*after, *before + 7.0, 0.05);
    }

    // --- bytes that are not audio are reported, not metered ---

    {
        // One impossible value in a music file is counted (the recovered
        // card's foreign RIFF headers read as hundreds of these); the same
        // file without it counts zero.
        const auto damaged = sineWav(44100, -23.0, 1.0e20f);
        CHECK_EQ(wav::measureLoudness(view(damaged)).wildSamples, 1);
        const auto clean = sineWav(44100, -23.0);
        CHECK_EQ(wav::measureLoudness(view(clean)).wildSamples, 0);
    }

    // --- progress reporting tells the truth (issue #61) ---

    {
        const auto bytes = sineWav(44100, -23.0);
        std::vector<double> ticks;
        const auto collect = [&ticks](double v) { ticks.push_back(v); };

        (void) wav::measureLoudness(view(bytes), collect);
        CHECK(!ticks.empty());
        bool monotonic = true;
        for (std::size_t i = 1; i < ticks.size(); ++i)
            monotonic = monotonic && ticks[i] >= ticks[i - 1];
        CHECK(monotonic);
        CHECK(ticks.front() > 0.0); // frames counted, not a courtesy 0
        CHECK(std::abs(ticks.back() - 1.0) <= 1.0e-12);

        ticks.clear();
        (void) wav::withGainDb(view(bytes), 3.0, collect);
        CHECK(!ticks.empty());
        monotonic = true;
        for (std::size_t i = 1; i < ticks.size(); ++i)
            monotonic = monotonic && ticks[i] >= ticks[i - 1];
        CHECK(monotonic);
        CHECK(std::abs(ticks.back() - 1.0) <= 1.0e-12);
    }

    return testkit::summary("normalize");
}
