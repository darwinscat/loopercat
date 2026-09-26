// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared test support: a tiny check harness, the golden fixture (values
// captured from real BOSS RC-5 hardware — fixtures/golden.json, one truth
// shared with the rc5cat JS and Swift suites), and synthetic fixtures built
// from the THEORY of the format (mirroring rc5cat test/helpers.js), never
// from the implementation under test.

#pragma once

#include <loopercat/Error.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace testkit {

inline int checksRun = 0;
inline int checksFailed = 0;

inline void fail(const std::string& what, const char* file, int line)
{
    ++checksFailed;
    std::printf("FAIL %s:%d  %s\n", file, line, what.c_str());
    std::fflush(stdout); // a crash further down must not take this line with it
}

inline void check(bool ok, const char* expr, const char* file, int line)
{
    ++checksRun;
    if (!ok)
        fail(expr, file, line);
}

template <typename A, typename B>
inline void checkEq(const A& a, const B& b, const char* expr, const char* file, int line)
{
    ++checksRun;
    if (!(a == b)) {
        std::ostringstream os;
        os << expr << "  (left: " << a << ", right: " << b << ")";
        fail(os.str(), file, line);
    }
}

inline int summary(const char* suite)
{
    std::printf("%s: %d checks, %d failed\n", suite, checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}

// The golden fixture — parsed once. LOOPERCAT_GOLDEN_JSON is the absolute
// path baked in by CMake; a missing or unparsable fixture aborts the suite.
inline const nlohmann::json& golden()
{
    static const nlohmann::json g = [] {
        std::ifstream in(LOOPERCAT_GOLDEN_JSON);
        if (!in)
            throw loopercat::Error("cannot open golden fixture: " LOOPERCAT_GOLDEN_JSON);
        return nlohmann::json::parse(in);
    }();
    return g;
}

// --- synthetic fixtures (theory of the format, per rc5cat test/helpers.js) ---

// A slot body shaped like a real dump but with values deliberately different
// from the factory ones (Measure=0, Reverb=0, Fill=0, Part4=1, Stop=0), so
// factory-state tests can never pass by accident against synthetic data.
inline std::string syntheticSlotBody(const std::string& name = "Memory 00")
{
    std::string padded = (name + std::string(12, ' ')).substr(0, 12);
    std::string s = "\n<NAME>\n";
    for (int i = 0; i < 12; ++i) {
        const std::string tag = "C" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        s += "\t<" + tag + ">" + std::to_string(static_cast<unsigned char>(padded[static_cast<std::size_t>(i)]))
           + "</" + tag + ">\n";
    }
    s += "</NAME>\n<TRACK1>\n";
    const std::pair<const char*, int> track[] = { { "Rev", 0 }, { "PlyLvl", 100 }, { "Pan", 50 },
        { "One", 0 }, { "StrtMod", 0 }, { "StpMod", 0 }, { "Measure", 0 }, { "MeasMod", 1 },
        { "MeasLen", 0 }, { "MeasBtLp", 0 }, { "RecTmp", 1200 }, { "WavStat", 0 }, { "WavLen", 0 } };
    for (const auto& [t, v] : track)
        s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
    s += "</TRACK1>\n<MASTER>\n";
    const std::pair<const char*, int> master[] = { { "Tempo", 1200 }, { "DubMode", 0 },
        { "RecAction", 1 }, { "AutoRec", 0 }, { "FadeTime", 5 }, { "Level", 100 }, { "LpMod", 0 },
        { "LpLen", 0 }, { "TrkMod", 1 }, { "Sync", 0 } };
    for (const auto& [t, v] : master)
        s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
    s += "</MASTER>\n<RHYTHM>\n";
    const std::pair<const char*, int> rhythm[] = { { "Level", 100 }, { "Reverb", 0 },
        { "Pattern", 0 }, { "Variation", 0 }, { "VariationChange", 0 }, { "Kit", 0 }, { "Beat", 2 },
        { "Fill", 0 }, { "Part1", 1 }, { "Part2", 1 }, { "Part3", 1 }, { "Part4", 1 },
        { "RecCount", 0 }, { "PlayCount", 0 }, { "Start", 0 }, { "Stop", 0 }, { "ToneLow", 10 },
        { "ToneHigh", 10 }, { "State", 0 } };
    for (const auto& [t, v] : rhythm)
        s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
    s += "</RHYTHM>\n";
    return s;
}

inline std::string syntheticMemoryText(std::uint32_t tailMarker = 0x38, int slots = 99)
{
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<database name=\"RC-5\" revision=\"0\">\n";
    for (int i = 0; i < slots; ++i) {
        const std::string n = std::to_string(i + 1);
        xml += "<mem id=\"" + std::to_string(i) + "\">"
             + syntheticSlotBody("Memory " + std::string(n.size() < 2 ? "0" : "") + n) + "</mem>\n";
    }
    xml += "</database>";
    xml += "\n";
    for (int i = 0; i < 4; ++i) // little-endian uint32 write counter (theory: rc0.js)
        xml.push_back(static_cast<char>((tailMarker >> (8 * i)) & 0xff));
    return xml;
}

// --- the two-track model, from the numbers measured on its card ---

// One track of a two-track memory, as the pedal writes it: `bars` and
// `frames` of a recorded take, or nothing (bars = 0) for an empty track.
struct TrackSpec {
    int bars = 0;
    long long frames = 0;
};

// A two-track memory body: NAME, TRACK1, TRACK2 (the same thirteen fields,
// each track its own take), MASTER, RHYTHM. RecTmp equals the memory's Tempo
// on every recorded track, and an empty track is factory-shaped (Measure=1,
// MeasLen=0, WavStat=0, WavLen=0) — both as measured on the card, 2026-09.
inline std::string syntheticTwoTrackSlotBody(const std::string& name, TrackSpec t1, TrackSpec t2,
                                             int tempoTenths, int lpLen, int level1 = 100,
                                             int level2 = 100)
{
    std::string padded = (name + std::string(12, ' ')).substr(0, 12);
    std::string s = "\n<NAME>\n";
    for (int i = 0; i < 12; ++i) {
        const std::string tag = "C" + std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        const auto code = static_cast<unsigned char>(padded[static_cast<std::size_t>(i)]);
        s += "\t<" + tag + ">" + std::to_string(code) + "</" + tag + ">\n";
    }
    s += "</NAME>\n";
    const auto track = [&s, tempoTenths, level1, level2](int number, const TrackSpec& spec) {
        const bool recorded = spec.bars > 0;
        s += "<TRACK" + std::to_string(number) + ">\n";
        const std::pair<const char*, long long> fields[] = { { "Rev", 0 },
            { "PlyLvl", number == 1 ? level1 : level2 },
            { "Pan", 50 }, { "One", 0 }, { "StrtMod", 0 }, { "StpMod", 0 },
            { "Measure", recorded ? spec.bars + 7 : 1 }, { "MeasMod", 1 },
            { "MeasLen", recorded ? spec.bars : 0 }, { "MeasBtLp", 0 }, { "RecTmp", tempoTenths },
            { "WavStat", recorded ? 1 : 0 }, { "WavLen", recorded ? spec.frames : 0 } };
        for (const auto& [t, v] : fields)
            s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
        s += "</TRACK" + std::to_string(number) + ">\n";
    };
    track(1, t1);
    track(2, t2);
    s += "<MASTER>\n";
    const std::pair<const char*, int> master[] = { { "Tempo", tempoTenths }, { "DubMode", 0 },
        { "RecAction", 1 }, { "AutoRec", 0 }, { "FadeTime", 5 }, { "Level", 100 }, { "LpMod", 1 },
        { "LpLen", lpLen }, { "TrkMod", 1 }, { "Sync", 0 } };
    for (const auto& [t, v] : master)
        s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
    s += "</MASTER>\n<RHYTHM>\n";
    const std::pair<const char*, int> rhythm[] = { { "Level", 100 }, { "Reverb", 30 },
        { "Pattern", 0 }, { "Variation", 0 }, { "VariationChange", 0 }, { "Kit", 0 }, { "Beat", 2 },
        { "Fill", 1 }, { "Part1", 1 }, { "Part2", 1 }, { "Part3", 1 }, { "Part4", 0 },
        { "RecCount", 0 }, { "PlayCount", 0 }, { "Start", 0 }, { "Stop", 1 }, { "ToneLow", 10 },
        { "ToneHigh", 10 }, { "State", 0 } };
    for (const auto& [t, v] : rhythm)
        s += "\t<" + std::string(t) + ">" + std::to_string(v) + "</" + t + ">\n";
    s += "</RHYTHM>\n";
    return s;
}

// A whole two-track memory file, root name "RC-500", 99 memories. Seven of
// them carry the takes measured on a real card of that model (kitchen notes,
// 2026-09-22/23); the numbers are the pedal's own, the file is not:
//
//   mem  1: T1 8 bars 641408, T2 8 bars 641408, 132.0 BPM, LpLen 8, levels 69 / 39
//           (the one memory on that card whose tracks are not at 100)
//   mem  2: T1 128 bars 14119056 (a vendor upload), T2 empty, 95.9, LpLen 128
//   mem  3: T1 4 bars 282240, T2 8 bars 564480, 150.0, LpLen 4
//   mem  4: T1 4 bars 351232, T2 empty, 120.5, LpLen 4
//   mem  6: T1 4 bars 395776, T2 8 bars 791552, 106.9, LpLen 4
//   mem 10: T1 8 bars 685056 recorded first, T2 4 bars 342528, 123.5, LpLen 8
//   mem 11: T1 empty, T2 4 bars 362496 — the only track, 116.7, LpLen 4
//
// LpLen is the length of the first RECORDED track (mem 10 against mem 11),
// not of track 1 and not of the shorter one. The rest are factory-empty.
inline std::string syntheticTwoTrackMemoryText(std::uint32_t tailMarker = 0x38)
{
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                      "<database name=\"RC-500\" revision=\"0\">\n";
    for (int i = 0; i < 99; ++i) {
        const int slot = i + 1;
        const std::string n = std::to_string(slot);
        const std::string name = "Memory" + std::string(n.size() < 2 ? "0" : "") + n;
        std::string body;
        const auto memory = [&name](TrackSpec t1, TrackSpec t2, int tempoTenths, int lpLen,
                                    int level1 = 100, int level2 = 100) {
            return syntheticTwoTrackSlotBody(name, t1, t2, tempoTenths, lpLen, level1, level2);
        };
        switch (slot) {
        case 1: body = memory({ 8, 641408 }, { 8, 641408 }, 1320, 8, 69, 39); break;
        case 2: body = memory({ 128, 14119056 }, {}, 959, 128); break;
        case 3: body = memory({ 4, 282240 }, { 8, 564480 }, 1500, 4); break;
        case 4: body = memory({ 4, 351232 }, {}, 1205, 4); break;
        case 6: body = memory({ 4, 395776 }, { 8, 791552 }, 1069, 4); break;
        case 10: body = memory({ 8, 685056 }, { 4, 342528 }, 1235, 8); break;
        case 11: body = memory({}, { 4, 362496 }, 1167, 4); break;
        default: body = memory({}, {}, 1200, 0); break;
        }
        xml += "<mem id=\"" + std::to_string(i) + "\">" + body + "</mem>\n";
    }
    xml += "</database>";
    xml += "\n";
    for (int i = 0; i < 4; ++i)
        xml.push_back(static_cast<char>((tailMarker >> (8 * i)) & 0xff));
    return xml;
}

struct WavSpec {
    int tag = 1;
    int channels = 2;
    int sampleRate = 44100;
    int bits = 16;
    int frames = 1000;
    bool extraChunk = false;
    int truncateBy = 0;
    // Fill the audio with the frame-index ramp instead of silence:
    // left = frame - frames/2, right = -left (pcm16 stereo only). Injective
    // over the whole file, so any played sample identifies its exact frame.
    bool rampFill = false;
};

// A hand-assembled RIFF/WAVE buffer (helpers.js makeWav): RIFF + fmt(16) +
// optional LIST(26) + data, zero-filled audio, optionally truncated.
inline std::vector<unsigned char> syntheticWav(const WavSpec& spec = {})
{
    const int blockAlign = spec.channels * (spec.bits / 8);
    const int dataSize = spec.frames * blockAlign;
    const int extra = spec.extraChunk ? 8 + 26 : 0;
    std::vector<unsigned char> buf;
    buf.reserve(static_cast<std::size_t>(12 + 24 + extra + 8 + dataSize));
    const auto ascii = [&buf](std::string_view s) {
        for (const char c : s)
            buf.push_back(static_cast<unsigned char>(c));
    };
    const auto p16 = [&buf](int v) {
        buf.push_back(static_cast<unsigned char>(v & 0xff));
        buf.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) {
        p16(v & 0xffff);
        p16((v >> 16) & 0xffff);
    };
    ascii("RIFF"); p32(12 + 24 + extra + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(spec.tag); p16(spec.channels); p32(spec.sampleRate); p32(spec.sampleRate * blockAlign);
    p16(blockAlign); p16(spec.bits);
    if (spec.extraChunk) {
        ascii("LIST"); p32(26);
        buf.insert(buf.end(), 26, 0);
    }
    ascii("data"); p32(dataSize);
    if (spec.rampFill) {
        if (spec.tag != 1 || spec.bits != 16 || spec.channels != 2 || spec.frames > 65535)
            throw loopercat::Error("rampFill supports pcm16 stereo up to 65535 frames");
        const int center = spec.frames / 2;
        for (int frame = 0; frame < spec.frames; ++frame) {
            const int left = frame - center;
            p16(left & 0xffff);
            p16(-left & 0xffff);
        }
    } else {
        buf.insert(buf.end(), static_cast<std::size_t>(dataSize), 0);
    }
    if (spec.truncateBy > 0)
        buf.resize(buf.size() - static_cast<std::size_t>(spec.truncateBy));
    return buf;
}

// The ramp's expected float sample for a frame, as JUCE surfaces pcm16
// (value / 32768). Mirror of the rampFill definition above.
inline float rampSample(int frame, int frames) { return static_cast<float>(frame - frames / 2) / 32768.0f; }

} // namespace testkit

#define CHECK(cond) ::testkit::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::testkit::checkEq((a), (b), #a " == " #b, __FILE__, __LINE__)

// The expression must throw loopercat::Error whose message contains `substr`
// (pass "" to accept any message). A different exception type is a failure —
// the core's contract is typed errors.
#define CHECK_THROWS(expr, substr)                                                                 \
    do {                                                                                           \
        ++::testkit::checksRun;                                                                    \
        try {                                                                                      \
            (void) (expr);                                                                         \
            ::testkit::fail("no throw: " #expr, __FILE__, __LINE__);                               \
        } catch (const loopercat::Error& e) {                                                      \
            if (std::string(e.what()).find(substr) == std::string::npos)                           \
                ::testkit::fail(std::string("wrong message: " #expr " -> \"") + e.what()           \
                                    + "\" (expected to contain \"" + (substr) + "\")",             \
                                __FILE__, __LINE__);                                               \
        } catch (...) {                                                                            \
            ::testkit::fail("wrong exception type (not loopercat::Error): " #expr,                 \
                            __FILE__, __LINE__);                                                   \
        }                                                                                          \
    } while (false)
