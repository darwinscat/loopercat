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

// Hand-assembled RIFF containers for attacking the parser: each chunk is an
// id, a DECLARED size, and the actual body bytes — declared and actual may
// deliberately disagree, modeling truncated and hostile files.
struct RawChunk {
    std::string id;
    std::uint32_t declared;
    std::vector<unsigned char> body;
};

std::vector<unsigned char> rawRiff(const std::vector<RawChunk>& chunks)
{
    std::vector<unsigned char> buf;
    const auto ascii = [&buf](std::string_view s) {
        for (const char c : s)
            buf.push_back(static_cast<unsigned char>(c));
    };
    const auto p32 = [&buf](std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8)
            buf.push_back(static_cast<unsigned char>((v >> shift) & 0xffu));
    };
    ascii("RIFF");
    p32(0); // patched below once the total is known
    ascii("WAVE");
    for (const auto& chunk : chunks) {
        ascii(chunk.id);
        p32(chunk.declared);
        buf.insert(buf.end(), chunk.body.begin(), chunk.body.end());
    }
    const auto riffSize = static_cast<std::uint32_t>(buf.size() - 8);
    for (int i = 0; i < 4; ++i)
        buf[4 + static_cast<std::size_t>(i)]
            = static_cast<unsigned char>((riffSize >> (8 * i)) & 0xffu);
    return buf;
}

// A well-formed 16-byte fmt body — pcm16 stereo 44.1k unless overridden.
std::vector<unsigned char> fmtBody(int tag = 1, int channels = 2, int blockAlign = 4,
                                   int bits = 16)
{
    std::vector<unsigned char> b;
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<unsigned char>(v & 0xff));
        b.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) {
        p16(v & 0xffff);
        p16((v >> 16) & 0xffff);
    };
    p16(tag);
    p16(channels);
    p32(44100);
    p32(44100 * blockAlign);
    p16(blockAlign);
    p16(bits);
    return b;
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

    // --- crafted buffers: refuse loudly, never read out of bounds ---
    // (crew review 2026-08-01, #2 and #3 — the main import path parses
    // untrusted files)

    {
        const std::vector<unsigned char> pad(24, 0);
        const std::vector<unsigned char> fmt = fmtBody();

        // fmt at EOF: a declared 16-byte body with zero bytes behind it must
        // be an explicit error, not an out-of-bounds read of the fmt fields.
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "JUNK", 24, pad }, { "fmt ", 16, {} } })),
                     "truncated");

        // fmt body cut mid-field.
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "JUNK", 24, pad },
                                                { "fmt ", 16, std::vector<unsigned char>(10, 0) } })),
                     "truncated");

        // ANY chunk claiming more than the file holds — junk chunks included
        // (a silent stop-scanning would hide real truncation).
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "fmt ", 16, fmt },
                                                { "data", 8, std::vector<unsigned char>(8, 0) },
                                                { "LIST", 100, {} } })),
                     "truncated");

        // Exactly one fmt and one data chunk: duplicates are ambiguous —
        // sizing frames by one data chunk while slicing audio from another
        // would be silent wrong audio — so both cases are refused outright.
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "fmt ", 16, fmt },
                                                { "fmt ", 16, fmt },
                                                { "data", 8, std::vector<unsigned char>(8, 0) } })),
                     "more than one fmt");
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "fmt ", 16, fmt },
                                                { "data", 400, std::vector<unsigned char>(400, 0) },
                                                { "data", 8, std::vector<unsigned char>(8, 0) } })),
                     "more than one data");

        // blockAlign must agree with channels * bytes-per-sample: every frame
        // count divides by it.
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "fmt ", 16, fmtBody(1, 2, 3, 16) },
                                                { "data", 12, std::vector<unsigned char>(12, 0) } })),
                     "does not match");

        // fmt shorter than its 16 mandatory bytes.
        CHECK_THROWS(wav::readWavInfo(rawRiff({ { "JUNK", 24, pad },
                                                { "fmt ", 8, std::vector<unsigned char>(8, 0) } })),
                     "malformed fmt");

        // The control: the same hand-built shape, well-formed, parses.
        const auto ok = wav::readWavInfo(rawRiff({ { "fmt ", 16, fmt },
                                                   { "data", 400, std::vector<unsigned char>(400, 0) } }));
        CHECK_EQ(ok.frames, 100);
        CHECK_EQ(ok.format(), "pcm16");
    }

    // --- upload validation ---

    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .channels = 1 }))), "stereo");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .sampleRate = 48000 }))), "44100");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 8 }))), "format");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 3, .bits = 64 }))), "format");

    // Only the pedal's own format goes in untouched. PCM used to be accepted
    // here on the strength of Roland's documentation, and hardware said no:
    // a 16-bit 44.1 kHz stereo file uploaded as-is produced a memory the
    // pedal would not play (issue #44). Everything else now goes through the
    // converter, which is what made the same tester's 24-bit files work.
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 16 }))),
                 "32-bit float");
    CHECK_THROWS(wav::assertUploadable(wav::readWavInfo(syntheticWav({ .tag = 1, .bits = 24 }))),
                 "32-bit float");
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
