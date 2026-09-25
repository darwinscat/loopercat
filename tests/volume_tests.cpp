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
#include <algorithm>
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

    // A track's folder is NNN_T, and the model says how many T there are: the
    // two-track model's card has NNN_1 and NNN_2 per memory, the RC-5's NNN_1.
    CHECK_EQ(volume::trackDirName(profile::kRc500, 3, 1), "003_1");
    CHECK_EQ(volume::trackDirName(profile::kRc500, 3, 2), "003_2");
    CHECK_EQ(volume::trackDirName(profile::kRc500, 99, 2), "099_2");
    CHECK_THROWS(volume::trackDirName(profile::kRc500, 3, 3), "track out of range 1..2");
    CHECK_THROWS(volume::trackDirName(profile::kRc500, 3, 0), "track out of range");
    CHECK_THROWS(volume::trackDirName(profile::kRc5, 3, 2), "track out of range 1..1");
    CHECK_THROWS(volume::trackDirName(profile::kRc5, 3, 2), "\"RC-5\" model");
    CHECK_THROWS(volume::trackDirName(profile::kRc500, 0, 1), "out of range");
    CHECK_THROWS(volume::trackDirName(profile::kRc500, 100, 2), "out of range");
    for (int slot = 1; slot <= rc0::kSlotCount; ++slot)
        CHECK_EQ(volume::slotDirName(slot), volume::trackDirName(profile::kRc5, slot, 1));

    // --- layout paths ---

    {
        // fs::path comparisons, not strings: the layout must hold under both
        // native separators (Windows renders these with backslashes).
        const fs::path v = "/Volumes/BOSS RC-5";
        CHECK_EQ(volume::dataDir(v), fs::path("/Volumes/BOSS RC-5/ROLAND/DATA"));
        CHECK_EQ(volume::wavDir(v, 7), fs::path("/Volumes/BOSS RC-5/ROLAND/WAVE/007_1"));
        CHECK_EQ(volume::trackDir(v, profile::kRc500, 7, 2),
                 fs::path("/Volumes/BOSS RC-5/ROLAND/WAVE/007_2"));
        CHECK_EQ(volume::trackDir(v, profile::kRc5, 7, 1), volume::wavDir(v, 7));
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

        // A second track's folder is listed by its own address, junk filtered
        // the same way, and the RC-5's one-track listing is track 1's.
        touch(volume::trackDir(pedal, profile::kRc500, 3, 2) / "second.wav");
        touch(volume::trackDir(pedal, profile::kRc500, 3, 2) / "._second.wav");
        const auto track2 = volume::listTrackWavs(pedal, profile::kRc500, 3, 2);
        CHECK_EQ(track2.size(), 1u);
        CHECK_EQ(track2.front(), "second.wav");
        CHECK(volume::listTrackWavs(pedal, profile::kRc500, 3, 1) == slot3);
        CHECK(volume::listTrackWavs(pedal, profile::kRc5, 3, 1) == volume::listSlotWavs(pedal, 3));
        CHECK_THROWS(volume::listTrackWavs(pedal, profile::kRc5, 3, 2), "track out of range");

        CHECK_THROWS(volume::listSlotWavs(pedal, 0), "out of range");
    }

    // --- junk at the volume ROOT: the marker's sidecar (measured 2026-09-24:
    // macOS plants ._loopercat-card.json beside every write of
    // /loopercat-card.json), swept one level deep, without entering the
    // directories a host OS keeps there ---

    {
        TempDir tmp;
        const fs::path pedal = tmp.path / "PEDAL";
        touch(pedal / "ROLAND" / "DATA" / "MEMORY1.RC0");
        touch(pedal / "loopercat-card.json");
        touch(pedal / "._loopercat-card.json");
        touch(pedal / ".DS_Store");
        touch(pedal / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav");
        touch(pedal / ".Spotlight-V100" / "._store");             // macOS's, not ours
        touch(pedal / ".fseventsd" / "._log");                     // macOS's, not ours
        touch(pedal / "System Volume Information" / "._sys");     // Windows's, not ours
        touch(pedal / "OTHER" / "._deep");                         // a stray directory: not entered either

        const auto junk = volume::findJunk(pedal);
        CHECK_EQ(junk.size(), 3u);
        CHECK(std::find(junk.begin(), junk.end(), pedal / "._loopercat-card.json") != junk.end());
        CHECK(std::find(junk.begin(), junk.end(), pedal / ".DS_Store") != junk.end());
        CHECK(std::find(junk.begin(), junk.end(), pedal / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav")
              != junk.end());

        const auto swept = volume::sweepJunk(pedal);
        CHECK_EQ(swept.removed.size(), 3u);
        CHECK(swept.failed.empty());
        CHECK(!fs::exists(pedal / "._loopercat-card.json"));
        CHECK(!fs::exists(pedal / ".DS_Store"));
        CHECK(!fs::exists(pedal / "ROLAND" / "WAVE" / "001_1" / "._01 - Loop.wav"));
        CHECK(fs::exists(pedal / "loopercat-card.json"));
        CHECK(fs::exists(pedal / ".Spotlight-V100" / "._store"));
        CHECK(fs::exists(pedal / ".fseventsd" / "._log"));
        CHECK(fs::exists(pedal / "System Volume Information" / "._sys"));
        CHECK(fs::exists(pedal / "OTHER" / "._deep"));
        CHECK(fs::exists(pedal / "ROLAND" / "DATA" / "MEMORY1.RC0"));
        CHECK(volume::findJunk(pedal).empty());
    }

    return testkit::summary("volume");
}
