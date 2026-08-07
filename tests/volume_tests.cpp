// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal-as-a-volume layer: content-based detection (ROLAND/DATA +
// ROLAND/WAVE), the slot directory naming, junk-name hygiene, and slot wav
// listing — exercised against real temp directories, junk included.

#include "support.hpp"

#include <loopercat/Volume.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

// A scratch tree that cleans up after itself.
struct TempDir {
    fs::path path;
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("loopercat-test-" + std::to_string(stamp));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

void touch(const fs::path& p)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p.string()).put('x');
}

} // namespace

int main()
{
    // --- slot directory naming ---

    CHECK_EQ(volume::slotDirName(1), "001_1");
    CHECK_EQ(volume::slotDirName(42), "042_1");
    CHECK_EQ(volume::slotDirName(99), "099_1");
    CHECK_THROWS(volume::slotDirName(0), "out of range");
    CHECK_THROWS(volume::slotDirName(100), "out of range");

    // --- layout paths ---

    {
        // fs::path comparisons, not strings: the layout must hold under both
        // native separators (Windows renders these with backslashes).
        const fs::path v = "/Volumes/BOSS RC-5";
        CHECK_EQ(volume::dataDir(v), fs::path("/Volumes/BOSS RC-5/ROLAND/DATA"));
        CHECK_EQ(volume::wavDir(v, 7), fs::path("/Volumes/BOSS RC-5/ROLAND/WAVE/007_1"));
        CHECK_EQ(volume::memoryPath(v, 1), fs::path("/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY1.RC0"));
        CHECK_EQ(volume::memoryPath(v, 2), fs::path("/Volumes/BOSS RC-5/ROLAND/DATA/MEMORY2.RC0"));
        CHECK_THROWS(volume::memoryPath(v, 3), "must be 1 or 2");
    }

    // --- junk names ---

    CHECK(volume::isJunkName("._01 - Loop.wav"));
    CHECK(volume::isJunkName(".DS_Store"));
    CHECK(volume::isJunkName("Thumbs.db"));
    CHECK(volume::isJunkName("desktop.ini"));
    CHECK(!volume::isJunkName("01 - Loop.wav"));
    CHECK(!volume::isJunkName(".hidden")); // dot alone is not AppleDouble

    // --- content-based detection ---

    {
        TempDir tmp;
        const fs::path pedal = tmp.path / "PEDAL";
        const fs::path other = tmp.path / "OTHER";
        fs::create_directories(pedal / "ROLAND" / "DATA");
        fs::create_directories(pedal / "ROLAND" / "WAVE");
        fs::create_directories(other / "Music");

        CHECK(volume::looksLikePedal(pedal));
        CHECK(!volume::looksLikePedal(other));
        CHECK(!volume::looksLikePedal(tmp.path / "MISSING"));

        // Detection is by content, first match wins; no pedal -> no value.
        const auto found = volume::detectVolume({ other, pedal });
        CHECK(found.has_value() && *found == pedal);
        CHECK(!volume::detectVolume({ other }).has_value());
        CHECK(!volume::detectVolume({}).has_value());

        // DATA alone (half a pedal) is not a pedal.
        const fs::path half = tmp.path / "HALF";
        fs::create_directories(half / "ROLAND" / "DATA");
        CHECK(!volume::looksLikePedal(half));
    }

    // --- slot wav listing ---

    {
        TempDir tmp;
        const fs::path pedal = tmp.path / "PEDAL";
        touch(volume::wavDir(pedal, 3) / "03 - Loop.wav");
        touch(volume::wavDir(pedal, 3) / "._03 - Loop.wav"); // AppleDouble sidecar
        touch(volume::wavDir(pedal, 3) / ".DS_Store");
        touch(volume::wavDir(pedal, 5) / "b.wav");
        touch(volume::wavDir(pedal, 5) / "a.wav");

        const auto slot3 = volume::listSlotWavs(pedal, 3);
        CHECK_EQ(slot3.size(), 1u);
        CHECK_EQ(slot3.at(0), "03 - Loop.wav");

        // Sorted for determinism.
        const auto slot5 = volume::listSlotWavs(pedal, 5);
        CHECK_EQ(slot5.size(), 2u);
        CHECK_EQ(slot5.at(0), "a.wav");
        CHECK_EQ(slot5.at(1), "b.wav");

        // A missing slot directory is an empty slot, not an error.
        CHECK(volume::listSlotWavs(pedal, 9).empty());

        CHECK_THROWS(volume::listSlotWavs(pedal, 0), "out of range");
    }

    return testkit::summary("volume");
}
