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
// A quiet volume between the steps must deliver NOTHING (change-gated).

#include "support.hpp"

#include "../app/PedalWorker.h"

#include <loopercat/Rc0.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

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
    }

    // 2. The pedal content lands (a mount): a delivery with all 99 slots.
    writeMemoryPair(volume, testkit::syntheticMemoryText());
    CHECK(pumpUntil([&] { return deliveries.size() >= 2; }, 5000));
    if (deliveries.size() >= 2) {
        const auto& s = deliveries.back();
        CHECK_EQ(s.volume, volume.string());
        CHECK_EQ(s.error, "");
        CHECK_EQ(s.slots.size(), static_cast<std::size_t>(rc0::kSlotCount));
        CHECK_EQ(s.slots.at(0).info.name, "Memory 01   ");
    }

    // A quiet volume delivers nothing: give the monitor two poll periods.
    {
        const auto count = deliveries.size();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(3500);
        CHECK_EQ(deliveries.size(), count);
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

    return testkit::summary("pedal_monitor_harness");
}
