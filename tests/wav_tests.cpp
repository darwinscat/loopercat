// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The RIFF/WAVE reader, the pedal-canonical rewriter (pinned byte-for-byte
// to a header a real pedal normalized), upload validation, and the pull-side
// filename rules (pinned to golden "technicalNames").

#include "support.hpp"

#include <loopercat/Wav.hpp>

#include <algorithm>
#include <set>

using namespace loopercat;
using testkit::WavSpec;
using testkit::syntheticWav;

namespace {

std::vector<unsigned char> fromBase64(const std::string& b64)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> out;
    int acc = 0, bits = 0;
    for (const char c : b64) {
        if (c == '=')
            break;
        const auto v = alphabet.find(c);
        if (v == std::string_view::npos)
            throw loopercat::Error("test fixture: bad base64");
        acc = (acc << 6) | static_cast<int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

} // namespace

int main()
{
    CHECK_EQ(wav::kSampleRate, 44100); // the pedal's one and only rate

    // --- reader ---

    {
        const auto info = wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 16, .frames = 4321 }));
        CHECK_EQ(info.format(), "pcm16");
        CHECK_EQ(info.formatTag, 1);
        CHECK_EQ(info.channels, 2);
        CHECK_EQ(info.sampleRate, 44100);
        CHECK_EQ(info.bitsPerSample, 16);
        CHECK_EQ(info.blockAlign, 4);
        CHECK_EQ(info.frames, 4321);
        CHECK_EQ(info.dataBytes, 4321 * 4);
    }

    {
        const auto info = wav::readWavInfo(syntheticWav({ .tag = 3, .bits = 32, .frames = 100 }));
        CHECK_EQ(info.format(), "float32");
        CHECK_EQ(info.frames, 100);
    }

    // Extra metadata chunks before data do not confuse the frame count.
    CHECK_EQ(wav::readWavInfo(syntheticWav({ .frames = 777, .extraChunk = true })).frames, 777);

    // A truncated data chunk is an error, not a short read.
    CHECK_THROWS(wav::readWavInfo(syntheticWav({ .frames = 1000, .truncateBy = 100 })), "truncated");

    // Non-WAVE input.
    {
        const std::string junk = "MP3 or whatever, 64 bytes of it padding padding pad";
        wav::Bytes bytes(junk.begin(), junk.end());
        CHECK_THROWS(wav::readWavInfo(bytes), "RIFF");
        CHECK_THROWS(wav::readWavInfo(wav::Bytes(10, 0)), "RIFF");
    }

    CHECK_THROWS(wav::readWavInfo(syntheticWav({ .tag = 2 })), "unsupported");

    // --- upload validation ---

    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .channels = 1 }))), "stereo");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .sampleRate = 48000 }))), "44100");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 8 }))), "format");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 3, .bits = 64 }))), "format");

    CHECK_EQ(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 16 }))).format(), "pcm16");
    CHECK_EQ(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 24 }))).format(), "pcm24");
    CHECK_EQ(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 3, .bits = 32 }))).format(), "float32");

    // --- canonicalize ---

    // Strips metadata chunks and keeps audio bytes intact.
    {
        const auto messy = syntheticWav({ .tag = 1, .bits = 16, .frames = 500, .extraChunk = true });
        const auto clean = wav::canonicalize(messy);
        CHECK_EQ(clean.size(), 44u + 500 * 4); // pcm gets the classic 44-byte header
        const auto before = wav::readWavInfo(messy);
        const auto after = wav::readWavInfo(clean);
        CHECK_EQ(after.frames, before.frames);
        CHECK_EQ(after.format(), before.format());
        CHECK(std::equal(messy.end() - 500 * 4, messy.end(), clean.begin() + 44));
    }

    // Float32 gets the pedal-style 28-byte fmt body.
    {
        const auto clean = wav::canonicalize(syntheticWav({ .tag = 3, .bits = 32, .frames = 100 }));
        CHECK_EQ(clean.size(), 56u + 100 * 8);
        CHECK_EQ(clean[16] | (clean[17] << 8), 28); // fmt chunk size
        CHECK_EQ(clean[36] | (clean[37] << 8), 10); // cbSize = 10
        CHECK(clean[48] == 'd' && clean[49] == 'a' && clean[50] == 't' && clean[51] == 'a');
    }

    // The float32 header byte-identical to a real pedal-normalized file
    // (golden), outside the two file-specific size fields.
    {
        const auto& fixture = testkit::golden().at("canonicalFloat32Header");
        const auto pedal = fromBase64(fixture.at("base64").get<std::string>());
        const auto ours = wav::canonicalize(syntheticWav({ .tag = 3, .bits = 32, .frames = 100 }));
        std::set<int> skip;
        for (const auto& off : fixture.at("sizeFieldOffsets"))
            skip.insert(off.get<int>());
        int mismatches = 0;
        for (int i = 0; i < 56; ++i)
            if (!skip.contains(i) && ours[static_cast<std::size_t>(i)] != pedal[static_cast<std::size_t>(i)])
                ++mismatches;
        CHECK_EQ(mismatches, 0);
    }

    // Idempotent: canonicalizing a canonical file changes nothing.
    {
        const auto once = wav::canonicalize(syntheticWav({ .tag = 3, .bits = 32, .frames = 100, .extraChunk = true }));
        CHECK(wav::canonicalize(once) == once);
    }

    // --- trimmed: the canonical slice, byte-exact ---

    {
        // The ramp encodes each frame's index — the slice must carry exactly
        // frames [100, 4200), regardless of any metadata chunk in front.
        const auto source = syntheticWav({ .frames = 8000, .extraChunk = true, .rampFill = true });
        const auto slice = wav::trimmed(source, 100, 4200);
        const auto info = wav::readWavInfo(slice);
        CHECK_EQ(info.frames, 4100);
        CHECK_EQ(slice.size(), 44u + 4100 * 4);
        int wrongFrames = 0;
        for (int i = 0; i < 4100; ++i) {
            const std::size_t at = 44 + static_cast<std::size_t>(i) * 4;
            const int left = static_cast<std::int16_t>(slice[at] | (slice[at + 1] << 8));
            if (left != (100 + i) - 8000 / 2)
                ++wrongFrames;
        }
        CHECK_EQ(wrongFrames, 0);

        // Full range == canonicalize; the slice math holds for float32's
        // 8-byte frames too.
        CHECK(wav::trimmed(source, 0, 8000) == wav::canonicalize(source));
        const auto floatWav = syntheticWav({ .tag = 3, .bits = 32, .frames = 500 });
        CHECK_EQ(wav::readWavInfo(wav::trimmed(floatWav, 200, 300)).frames, 100);
        CHECK_EQ(wav::trimmed(floatWav, 200, 300).size(), 56u + 100 * 8);

        // Rejection edges: inverted, empty, and out-of-file ranges.
        CHECK_THROWS(wav::trimmed(source, 4200, 100), "bad frame range");
        CHECK_THROWS(wav::trimmed(source, 100, 100), "bad frame range");
        CHECK_THROWS(wav::trimmed(source, -1, 100), "bad frame range");
        CHECK_THROWS(wav::trimmed(source, 0, 8001), "bad frame range");
    }

    // --- pull naming, pinned to golden "technicalNames" ---

    for (const auto& entry : testkit::golden().at("technicalNames"))
        CHECK_EQ(wav::isTechnicalWavName(entry.at("name").get<std::string>()),
                 entry.at("technical").get<bool>());

    CHECK_EQ(wav::wavFileName(3, "Nice Song"), "03 - Nice Song.wav");
    CHECK_EQ(wav::wavFileName(42, "  spaced   out  "), "42 - spaced out.wav");
    CHECK_EQ(wav::wavFileName(7, "a/b:c*d"), "07 - a_b_c_d.wav");
    CHECK_EQ(wav::wavFileName(9, "   "), "09 - Memory09.wav");

    CHECK_EQ(wav::pullFileName(2, "My Loop", "TRACK.WAV"), "02 - My Loop.wav");
    CHECK_EQ(wav::pullFileName(2, "My Loop", "dropped.wav"), "dropped.wav");

    return testkit::summary("wav");
}
