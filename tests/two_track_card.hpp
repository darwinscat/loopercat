// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A two-track card on disk ("RC-500" in its root), built from the theory of
// its format rather than from the code under test: the memory pair of
// support.hpp's syntheticTwoTrackMemoryText, a folder per track per memory
// as the real card carries them (99 x 2: NNN_1 and NNN_2, empty ones
// included), and short takes in the memories a test names — each memory
// with a take re-cut so that every track section's WavStat and WavLen say
// what its folder holds.

#pragma once

#include "support.hpp"

#include <loopercat/Commands.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace testkit {

// One take of the card: which memory, which track, the file's name and its
// length; `seed` makes two takes of one length different bytes.
struct CardTake {
    int slot;
    int track;
    std::string name;
    int frames;
    int seed;
};

// A float32 stereo 44.1 kHz take in the pedal's own format, its samples small
// integers over a power of two — exact on every platform — and different for
// every seed.
inline std::string floatTake(int frames, int seed)
{
    std::string b;
    const auto ascii = [&b](std::string_view t) { b += t; };
    const auto p16 = [&b](int v) {
        b.push_back(static_cast<char>(v & 0xff));
        b.push_back(static_cast<char>((v >> 8) & 0xff));
    };
    const auto p32 = [&p16](int v) { p16(v & 0xffff); p16((v >> 16) & 0xffff); };
    const auto sample = [&b](float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        for (int shift = 0; shift < 32; shift += 8)
            b.push_back(static_cast<char>((bits >> shift) & 0xffu));
    };
    const int dataSize = frames * 8;
    ascii("RIFF"); p32(12 + 24 + 8 + dataSize - 8); ascii("WAVE");
    ascii("fmt "); p32(16);
    p16(3); p16(2); p32(44100); p32(44100 * 8); p16(8); p16(32);
    ascii("data"); p32(dataSize);
    for (int frame = 0; frame < frames; ++frame) {
        sample(static_cast<float>((frame + seed) % 128 - 64) / 256.0f);
        sample(static_cast<float>((frame * 3 + seed) % 64) / 128.0f);
    }
    return b;
}

// The card, under `root/name`. Every memory a take names gets a body whose
// two track sections describe exactly its takes; the others stay as the
// synthetic text has them.
inline std::filesystem::path makeTwoTrackCard(const std::filesystem::path& root,
                                              const std::vector<CardTake>& takes,
                                              const std::string& name = "RC-500")
{
    namespace fs = std::filesystem;
    using namespace loopercat;
    const fs::path volume = root / name;
    fs::create_directories(volume::dataDir(volume));
    for (int slot = 1; slot <= 99; ++slot)
        for (int track = 1; track <= 2; ++track)
            fs::create_directories(volume::trackDir(volume, profile::kRc500, slot, track));
    std::string text = syntheticTwoTrackMemoryText();
    std::map<int, std::map<int, int>> frames; // slot -> track -> frames
    for (const CardTake& take : takes) {
        commands::writeFileBytes(volume::trackDir(volume, profile::kRc500, take.slot, take.track)
                                     / take.name,
                                 floatTake(take.frames, take.seed));
        frames[take.slot][take.track] = take.frames;
    }
    for (const auto& [slot, tracks] : frames) {
        const auto spec = [&tracks](int track) {
            const auto found = tracks.find(track);
            return found == tracks.end() ? TrackSpec {} : TrackSpec { 1, found->second };
        };
        text = rc0::replaceSlotBody(
            text, slot,
            syntheticTwoTrackSlotBody("Memory " + std::to_string(slot), spec(1), spec(2), 1200, 1));
    }
    for (const int fileNo : { 1, 2 })
        commands::writeFileBytes(volume::memoryPath(volume, fileNo), rc0::setTailMarker(text, fileNo));
    return volume;
}

} // namespace testkit
