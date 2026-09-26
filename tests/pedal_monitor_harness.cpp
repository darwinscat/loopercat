// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the live-refresh contract: the REAL PedalWorker
// (worker thread + async delivery) driven against a scratch volume that
// mounts, changes and unmounts under it. Proves the browser's rows follow
// the filesystem without any UI present:
//
//   1. no pedal content    -> first delivery is the not-mounted error state
//                             (explicit --volume semantics: a bad path is an
//                             error to show, not a silent no-pedal)
//   2. pedal content lands -> delivery with all 99 slots
//   3. a slot is renamed on disk -> delivery with the new name
//   4. a mutation JOB runs on the same worker -> busy brackets, ok result,
//      the post-job snapshot already carries the change
//   5. a failing job surfaces its typed error and changes nothing
//   6. the content goes    -> delivery drops the mounted state again
//   7. a ghost mount (device watcher says GONE while the path still serves
//      content) -> reported as ghost, mutations refused, bytes untouched
//
// A quiet volume between the steps must deliver nothing NEW ABOUT THE CARD
// (change-gated): the one thing that may still arrive is a free-space
// change, because the scratch volume lives on a real disk.

#include "support.hpp"

#include "../app/PedalWorker.h"

#include <loopercat/DeviceProfile.hpp>
#include <loopercat/Rc0.hpp>
#include <loopercat/Volume.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <tuple>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace loopercat;
namespace fs = std::filesystem;

namespace {

void writeMemoryPair(const fs::path& volume, const std::string& text)
{
    fs::create_directories(volume / "ROLAND" / "DATA");
    fs::create_directories(volume / "ROLAND" / "WAVE");
    for (const int fileNo : { 1, 2 }) {
        const std::string withTail = rc0::setTailMarker(text, fileNo);
        std::ofstream out(volume / "ROLAND" / "DATA" / ("MEMORY" + std::to_string(fileNo) + ".RC0"),
                          std::ios::binary);
        out.write(withTail.data(), static_cast<std::streamsize>(withTail.size()));
    }
}

std::string readTextFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The card's content of a snapshot — everything the browser shows about the
// pedal except the free space, which is the disk's, not the card's.
bool sameCard(const PedalSnapshot& a, const PedalSnapshot& b)
{
    return a.volume == b.volume && a.error == b.error && a.state == b.state && a.slots == b.slots
        && a.findings == b.findings;
}

bool pumpUntil(const std::function<bool()>& condition, const int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (!condition() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    return condition();
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceRuntime;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path volume = fs::temp_directory_path() / ("loopercat-monitor-" + std::to_string(stamp));
    fs::remove_all(volume);
    fs::create_directories(volume); // the mount point exists; no pedal content yet

    std::vector<PedalSnapshot> deliveries;
    PedalWorker monitor(volume.string(), [&deliveries](const PedalSnapshot& s) {
        deliveries.push_back(s);
    });
    std::vector<bool> busyEvents;
    std::vector<std::pair<juce::String, juce::String>> jobResults;
    monitor.onBusy = [&busyEvents](bool busy, int, bool) { busyEvents.push_back(busy); };
    monitor.onJobResult = [&jobResults](juce::String description, juce::String error, int, int) {
        jobResults.emplace_back(std::move(description), std::move(error));
    };
    monitor.start();

    // 1. First delivery: the pinned path has no pedal content -> the error
    // state (explicit-mode fail-fast), and certainly no rows.
    CHECK(pumpUntil([&] { return deliveries.size() >= 1; }, 5000));
    if (!deliveries.empty()) {
        CHECK(!deliveries.back().error.empty());
        CHECK(deliveries.back().slots.empty());
        CHECK(deliveries.back().family.empty()); // nothing read, no model
    }

    // 2. The pedal content lands (a mount): a delivery with all 99 slots.
    writeMemoryPair(volume, testkit::syntheticMemoryText());
    CHECK(pumpUntil([&] { return deliveries.size() >= 2; }, 5000));
    if (deliveries.size() >= 2) {
        const auto& s = deliveries.back();
        CHECK_EQ(s.volume, volume.string());
        CHECK_EQ(s.error, "");
        CHECK_EQ(s.family, "RC-5"); // the card's own word for its model
        CHECK_EQ(s.slots.size(), static_cast<std::size_t>(rc0::kSlotCount));
        CHECK_EQ(s.slots.at(0).info.name, "Memory 01   ");
    }

    // A quiet volume delivers nothing new about the card: give the monitor
    // two poll periods. This deliberately does NOT count deliveries. The
    // scratch volume sits on a real disk that other processes keep writing
    // to, and PedalSnapshot compares free space at the 0.1 GB the status
    // line shows: when the figure crosses that bucket, the worker is right
    // to deliver — the number a user reads really did change. Counting
    // deliveries called that a failure on a busy machine, about every other
    // run, and never in CI where the disk is quiet. So every delivery that
    // arrives here must be a change (the gate still holds: no snapshot is
    // delivered twice) and must differ from the previous one in nothing but
    // free space. Tightening this back to a count reintroduces the flake,
    // not a stricter test.
    if (!deliveries.empty()) {
        const auto count = deliveries.size();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(3500);
        for (std::size_t i = count; i < deliveries.size(); ++i) {
            CHECK(sameCard(deliveries[i], deliveries[i - 1]));
            CHECK(!(deliveries[i] == deliveries[i - 1]));
        }
    }

    // 3. A slot rename on disk is picked up.
    {
        std::string text = testkit::syntheticMemoryText();
        text = rc0::replaceSlotBody(text, 42, rc0::setName(rc0::slotBody(text, 42), "Live Refresh"));
        writeMemoryPair(volume, text);
    }
    CHECK(pumpUntil([&] {
        return !deliveries.empty() && !deliveries.back().slots.empty()
            && deliveries.back().slots.at(41).info.name == "Live Refresh";
    }, 5000));

    // 4. A mutation goes through the SAME worker: busy brackets it, the
    // result reports success, and the fresh snapshot already carries the
    // change — no wait for the next poll.
    monitor.enqueue({ "Rename slot 7", 7,
                      [](const volume::fs::path& volumePath) {
                          commands::rename(volumePath, 7, "Via Worker",
                                           { .skipBackup = true });
                      } });
    CHECK(pumpUntil([&] { return !jobResults.empty(); }, 5000));
    if (!jobResults.empty()) {
        CHECK_EQ(jobResults.back().first, juce::String("Rename slot 7"));
        CHECK_EQ(jobResults.back().second, juce::String());
    }
    CHECK(pumpUntil([&] { return busyEvents.size() >= 2; }, 5000));
    if (busyEvents.size() >= 2) {
        CHECK(busyEvents.front()); // true before…
        CHECK(!busyEvents.back()); // …false after
    }
    CHECK(!deliveries.empty() && !deliveries.back().slots.empty()
          && deliveries.back().slots.at(6).info.name == "Via Worker  ");

    // 5. A failing mutation surfaces its typed error and changes nothing.
    monitor.enqueue({ "Rename slot 7 badly", 7,
                      [](const volume::fs::path& volumePath) {
                          commands::rename(volumePath, 7, "ThirteenChars",
                                           { .skipBackup = true });
                      } });
    CHECK(pumpUntil([&] { return jobResults.size() >= 2; }, 5000));
    if (jobResults.size() >= 2)
        CHECK(jobResults.back().second.contains("longer than 12"));
    CHECK(!deliveries.empty() && !deliveries.back().slots.empty()
          && deliveries.back().slots.at(6).info.name == "Via Worker  ");

    // 6. The content disappears (an unmount): the mounted state drops — no
    // rows and no clean volume left standing.
    fs::remove_all(volume);
    CHECK(pumpUntil([&] {
        return !deliveries.empty() && deliveries.back().slots.empty()
            && !deliveries.back().error.empty();
    }, 5000));

    // 7. The ghost-mount gate (issue #17): a path that still serves pedal
    // content while the device watcher says the device is GONE must be
    // reported as a ghost, and a mutation against it must be REFUSED — the
    // 2026-07-22 phantom "one-shot saved" replayed as a test. A fresh worker
    // with a gone-verdict probe stands in for the dead USB device.
    {
        const fs::path ghostVolume =
            fs::temp_directory_path() / ("loopercat-ghost-" + std::to_string(stamp));
        fs::remove_all(ghostVolume);
        fs::create_directories(ghostVolume / "ROLAND" / "WAVE");
        writeMemoryPair(ghostVolume, testkit::syntheticMemoryText());

        std::vector<PedalSnapshot> ghostDeliveries;
        PedalWorker ghostWorker(ghostVolume.string(), [&ghostDeliveries](const PedalSnapshot& s) {
            ghostDeliveries.push_back(s);
        });
        std::vector<std::pair<juce::String, juce::String>> ghostResults;
        ghostWorker.onJobResult = [&ghostResults](juce::String description, juce::String error, int, int) {
            ghostResults.emplace_back(std::move(description), std::move(error));
        };
        ghostWorker.setBackingProbe(
            [](const volume::fs::path&) { return lifecycle::Backing::gone; });
        ghostWorker.start();

        CHECK(pumpUntil([&] { return !ghostDeliveries.empty(); }, 5000));
        if (!ghostDeliveries.empty())
            CHECK(ghostDeliveries.back().state == lifecycle::State::ghost);

        ghostWorker.enqueue({ "Rename slot 7 into the void", 7,
                              [](const volume::fs::path& volumePath) {
                                  commands::rename(volumePath, 7, "Phantom",
                                                   { .skipBackup = true });
                              } });
        CHECK(pumpUntil([&] { return !ghostResults.empty(); }, 5000));
        if (!ghostResults.empty())
            CHECK(ghostResults.back().second.contains("refusing to touch"));

        // The write really was refused: the file on the ghost is untouched.
        const std::string after = readTextFile(ghostVolume / "ROLAND" / "DATA" / "MEMORY1.RC0");
        CHECK(after.find("Phantom") == std::string::npos);

        fs::remove_all(ghostVolume);
    }

    // 8. A two-track model's card (the synthetic file from #107, its takes at
    // the pedal's addresses NNN_1 and NNN_2) scans as one row per memory:
    // the model named, every track's file in the row with its track, playback
    // on track 1's file — and a memory whose take sits on track 2 alone shows
    // its file and plays nothing. The scan changes nothing on the card.
    {
        const fs::path twoTrack =
            fs::temp_directory_path() / ("loopercat-twotrack-" + std::to_string(stamp));
        fs::remove_all(twoTrack);
        fs::create_directories(twoTrack / "ROLAND" / "WAVE");
        writeMemoryPair(twoTrack, testkit::syntheticTwoTrackMemoryText());
        const auto take = testkit::syntheticWav({ .tag = 3, .bits = 32, .frames = 4410 });
        const std::string_view bytes(reinterpret_cast<const char*>(take.data()), take.size());
        for (const auto& [slot, track, name] :
             { std::tuple { 1, 1, "001_1.WAV" }, std::tuple { 1, 2, "001_2.WAV" },
               std::tuple { 11, 2, "011_2.WAV" } }) {
            const fs::path dir = volume::trackDir(twoTrack, profile::kRc500, slot, track);
            fs::create_directories(dir);
            commands::writeFileBytes(dir / name, bytes);
        }
        std::vector<PedalSnapshot> ignored;
        PedalWorker scanner(twoTrack.string(), [&ignored](const PedalSnapshot& s) {
            ignored.push_back(s);
        });
        const PedalSnapshot s = scanner.scanOnce();
        CHECK_EQ(s.error, "");
        CHECK_EQ(s.family, "RC-500");
        CHECK_EQ(s.slots.size(), static_cast<std::size_t>(rc0::kSlotCount));
        if (s.slots.size() == static_cast<std::size_t>(rc0::kSlotCount)) {
            CHECK_EQ(s.slots.at(0).info.tracks.size(), 2u);
            CHECK_EQ(s.slots.at(0).wavFile, "T1 001_1.WAV, T2 001_2.WAV");
            CHECK_EQ(s.slots.at(0).wavPath,
                     (volume::trackDir(twoTrack, profile::kRc500, 1, 1) / "001_1.WAV").string());
            CHECK_EQ(s.slots.at(10).wavFile, "T2 011_2.WAV");
            CHECK_EQ(s.slots.at(10).wavPath, ""); // nothing on track 1 to play
            CHECK(!s.slots.at(10).info.hasAudio); // TRACK1's fact, as the flat field says
            CHECK(s.slots.at(10).info.tracks.at(1).hasAudio);
            CHECK_EQ(s.slots.at(1).wavFile, "");  // memory 2: a take indexed, no file on disk here
        }
        // An RC-5 card's rows read exactly as before: no track prefix.
        fs::remove_all(twoTrack);
    }

    return testkit::summary("pedal_monitor_harness");
}
