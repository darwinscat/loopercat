// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Headless harness for the live-refresh contract: the REAL PedalMonitor
// (worker thread + async delivery) driven against a scratch volume that
// mounts, changes and unmounts under it. Proves the browser's rows follow
// the filesystem without any UI present:
//
//   1. no pedal content    -> first delivery is the not-mounted error state
//                             (explicit --volume semantics: a bad path is an
//                             error to show, not a silent no-pedal)
//   2. pedal content lands -> delivery with all 99 slots
//   3. a slot is renamed   -> delivery with the new name (edit detection)
//   4. the content goes    -> delivery drops the mounted state again
//
// A quiet volume between the steps must deliver NOTHING (change-gated).

#include "support.hpp"

#include "../app/PedalMonitor.h"

#include <loopercat/Rc0.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <filesystem>
#include <fstream>

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
    PedalMonitor monitor(volume.string(), [&deliveries](const PedalSnapshot& s) {
        deliveries.push_back(s);
    });
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

    // 4. The content disappears (an unmount): the mounted state drops — no
    // rows and no clean volume left standing.
    fs::remove_all(volume);
    CHECK(pumpUntil([&] {
        return !deliveries.empty() && deliveries.back().slots.empty()
            && !deliveries.back().error.empty();
    }, 5000));

    return testkit::summary("pedal_monitor_harness");
}
