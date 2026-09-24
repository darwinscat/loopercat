// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The storage panel (issue #74), driven without a mouse, a database or a
// worker, from what it promises:
//
//   - before the history is read it says so, and offers nothing
//   - the numbers read like a person would say them: gigabytes, weeks
//   - the button offers exactly what the plan gives, and nothing when the
//     plan gives nothing — over the limit or under it
//   - lowering "keep at most" names the oldest unheld takes, in order
//   - a confirmed release hands out exactly the takes on offer, once
//   - a typed limit reaches the owner as bytes; a typo snaps back

#include "support.hpp"

#include "../app/HistoryStoragePanel.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace loopercat;
namespace retention = history::retention;

namespace {

constexpr std::int64_t MB = std::int64_t { 1 } << 20;
constexpr std::int64_t GB = std::int64_t { 1 } << 30;

retention::Blob blob(const std::string& hash, const std::string& label, std::int64_t size,
                     std::int64_t created)
{
    retention::Blob b;
    b.hash = hash;
    b.label = label;
    b.size = size;
    b.created = created;
    b.references = 1;
    return b;
}

HistoryStoragePanel::Facts facts(std::vector<retention::Blob> blobs, std::int64_t limit)
{
    HistoryStoragePanel::Facts f;
    std::int64_t kept = 0;
    for (const auto& b : blobs)
        kept += b.size;
    f.usage = { kept + 83 * MB, kept, 83 * MB, 0, 153 * GB };
    f.limit = limit;
    f.blobs = std::move(blobs);
    f.forecast = retention::forecast({ { 1000, 100 * MB } }, 1000 + 10 * retention::kWeekMs, kept,
                                     limit, 153 * GB);
    return f;
}

bool has(const juce::String& text, const char* part) { return text.contains(part); }

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // --- unread: honest, and nothing on offer ---
    {
        HistoryStoragePanel panel;
        CHECK(has(panel.costLine(), "not been read"));
        CHECK(!panel.releaseEnabled());
        CHECK_EQ(panel.offeredRows(), 0);
        CHECK(panel.offeredHashes().empty());
        CHECK_EQ(panel.limitText(), juce::String());
        int released = 0;
        panel.onRelease = [&](std::vector<std::string>) { ++released; };
        panel.confirmRelease();
        CHECK_EQ(released, 0);
        int limits = 0;
        panel.onLimitChanged = [&](std::int64_t) { ++limits; };
        panel.commitLimitText("6");
        CHECK_EQ(limits, 0); // no limit is in force to change
        panel.setKeepTarget(0);
        CHECK(!panel.releaseEnabled());
    }

    // --- over the limit: the numbers, and the offer that brings it back under ---
    {
        HistoryStoragePanel panel;
        std::vector<retention::Blob> blobs = {
            blob("c", "slot 3 take.wav", 400 * MB, 3000),
            blob("a", "slot 1 take.wav", 400 * MB, 1000),
            blob("b", "backups/2026-07-22T17-36-55/MEMORY1.RC0", 400 * MB, 2000),
            blob("d", "slot 4 take.wav", 400 * MB, 4000),
        };
        blobs[3].undo = true;
        panel.show(facts(blobs, 1 * GB)); // 1.6 GB kept, 1 GB limit
        CHECK(has(panel.costLine(), "1.6 GB of takes"));
        CHECK(has(panel.costLine(), "83 MB"));
        CHECK(has(panel.costLine(), "1.6 GB on disk")); // 1600 MB + 83 MB, in gigabytes
        CHECK(!has(panel.costLine(), "1677721600")); // bytes are never shown raw
        CHECK(has(panel.diskLine(), "153 GB"));
        CHECK(has(panel.forecastLine(), "weeks") || has(panel.forecastLine(), "limit"));
        CHECK_EQ(panel.limitText(), juce::String("1"));
        CHECK_EQ(panel.keepTarget(), 1 * GB);
        // 600 MB must go: the two oldest unheld takes
        CHECK(panel.releaseEnabled());
        CHECK_EQ(panel.releaseButtonText(), juce::String("Release 2 takes (800 MB)"));
        CHECK(has(panel.offerLine(), "Freeing 2 takes gives back 800 MB, leaving 800 MB."));
        CHECK(panel.offeredHashes() == (std::vector<std::string> { "a", "b" }));
        CHECK_EQ(panel.offeredRows(), 2);
    }

    // --- under the limit: the button is there, and offers nothing until asked ---
    {
        HistoryStoragePanel panel;
        std::vector<retention::Blob> blobs = {
            blob("a", "slot 1 take.wav", 100 * MB, 1000),
            blob("b", "slot 2 take.wav", 100 * MB, 2000),
            blob("c", "slot 3 take.wav", 100 * MB, 3000),
        };
        panel.show(facts(blobs, 5 * GB));
        CHECK_EQ(panel.keepTarget(), 300 * MB);
        CHECK(!panel.releaseEnabled());
        CHECK_EQ(panel.releaseButtonText(), juce::String("Nothing to release"));
        CHECK(has(panel.offerLine(), "within the limit"));
        CHECK_EQ(panel.offeredRows(), 0);
        // asking to keep at most 150 MB names the oldest two
        panel.setKeepTarget(150 * MB);
        CHECK(panel.releaseEnabled());
        CHECK_EQ(panel.releaseButtonText(), juce::String("Release 2 takes (200 MB)"));
        CHECK(panel.offeredHashes() == (std::vector<std::string> { "a", "b" }));
        // and everything, at zero
        panel.setKeepTarget(0);
        CHECK_EQ(panel.offeredRows(), 3);
        const auto all = panel.offeredHashes();
        CHECK(!all.empty() && all.front() == "a");
        // back to keeping it all: nothing on offer again
        panel.setKeepTarget(300 * MB);
        CHECK(!panel.releaseEnabled());
        CHECK_EQ(panel.offeredRows(), 0);
    }

    // --- everything held: nothing on offer, and the sentence says why ---
    {
        HistoryStoragePanel panel;
        std::vector<retention::Blob> blobs = {
            blob("a", "slot 1 take.wav", 4 * GB, 1000),
            blob("b", "slot 2 take.wav", 4 * GB, 2000),
        };
        blobs[0].pinned = true;
        blobs[1].undo = true;
        panel.show(facts(blobs, 5 * GB));
        CHECK_EQ(panel.keepTarget(), 5 * GB);
        CHECK(!panel.releaseEnabled());
        CHECK(has(panel.offerLine(), "Nothing can be freed"));
        CHECK(has(panel.offerLine(), "4 GB is pinned"));
        CHECK(has(panel.offerLine(), "4 GB is needed by undo"));
        panel.setKeepTarget(0);
        CHECK(!panel.releaseEnabled());
        CHECK(panel.offeredHashes().empty());
        int released = 0;
        panel.onRelease = [&](std::vector<std::string>) { ++released; };
        panel.confirmRelease();
        CHECK_EQ(released, 0);
    }

    // --- nothing kept: no slider to move, no offer, no lie ---
    {
        HistoryStoragePanel panel;
        panel.show(facts({}, 5 * GB));
        CHECK(has(panel.costLine(), "0 bytes of takes"));
        CHECK(!panel.releaseEnabled());
        CHECK(has(panel.offerLine(), "within the limit"));
        panel.setKeepTarget(0);
        CHECK(!panel.releaseEnabled());
    }

    // --- a confirmed release hands out exactly the offer, once ---
    {
        HistoryStoragePanel panel;
        std::vector<retention::Blob> blobs = {
            blob("old", "slot 1 take.wav", 300 * MB, 1000),
            blob("mid", "slot 2 take.wav", 300 * MB, 2000),
            blob("new", "slot 3 take.wav", 300 * MB, 3000),
        };
        std::vector<std::vector<std::string>> releases;
        panel.onRelease = [&](std::vector<std::string> hashes) { releases.push_back(hashes); };
        panel.show(facts(blobs, 500 * MB));
        CHECK(panel.offeredHashes() == (std::vector<std::string> { "old", "mid" }));
        panel.confirmRelease();
        CHECK_EQ(releases.size(), 1u);
        CHECK(!releases.empty() && releases.front() == (std::vector<std::string> { "old", "mid" }));
        // pressed again before the owner comes back: nothing more goes out
        CHECK(!panel.releaseEnabled());
        CHECK(has(panel.releaseButtonText(), "Releasing"));
        panel.confirmRelease();
        CHECK_EQ(releases.size(), 1u);
        // the owner comes back with what is left
        panel.show(facts({ blobs[2] }, 500 * MB));
        CHECK(!panel.releaseEnabled());
        CHECK(has(panel.offerLine(), "within the limit"));
        CHECK(has(panel.costLine(), "300 MB of takes"));
    }

    // --- the limit: typed in gigabytes, handed out in bytes, typos snap back ---
    {
        HistoryStoragePanel panel;
        std::vector<std::int64_t> limits;
        panel.onLimitChanged = [&](std::int64_t bytes) { limits.push_back(bytes); };
        std::vector<retention::Blob> blobs = { blob("a", "slot 1 take.wav", 2 * GB, 1000),
                                               blob("b", "slot 2 take.wav", 2 * GB, 2000) };
        panel.show(facts(blobs, 5 * GB));
        CHECK_EQ(panel.limitText(), juce::String("5"));

        panel.commitLimitText("6");
        CHECK_EQ(limits.size(), 1u);
        CHECK_EQ(limits.back(), 6 * GB);
        CHECK_EQ(panel.limitText(), juce::String("6"));

        panel.commitLimitText(" 2.5 ");
        CHECK_EQ(limits.size(), 2u);
        CHECK_EQ(limits.back(), static_cast<std::int64_t>(2.5 * static_cast<double>(GB)));
        CHECK_EQ(panel.limitText(), juce::String("2.5"));

        for (const char* typo : { "0", "abc", "-3", "", "0.05", "1e3", "99999" }) {
            panel.commitLimitText(typo); // typed into the field and judged
            CHECK_EQ(limits.size(), 2u);
            CHECK_EQ(panel.limitText(), juce::String("2.5")); // the field snapped back, visibly
        }
        panel.commitLimitText("2.5"); // the same limit again is not a change
        CHECK_EQ(limits.size(), 2u);

        // the offer follows the limit the owner confirmed by showing again
        panel.show(facts(blobs, 3 * GB));
        CHECK_EQ(panel.keepTarget(), 3 * GB);
        CHECK_EQ(panel.releaseButtonText(), juce::String("Release 1 take (2 GB)"));

        CHECK_EQ(HistoryStoragePanel::formatGb(5 * GB), juce::String("5"));
        CHECK_EQ(HistoryStoragePanel::formatGb(GB / 2), juce::String("0.5"));
        CHECK(HistoryStoragePanel::parseGb("10000") == std::optional<std::int64_t>(10000 * GB));
        CHECK(!HistoryStoragePanel::parseGb("10001").has_value());
        CHECK(!HistoryStoragePanel::parseGb("0.09").has_value());
        CHECK(HistoryStoragePanel::parseGb("0.1") == std::optional<std::int64_t>(GB / 10));
    }

    // --- the forecast line is the forecast's own sentence ---
    {
        HistoryStoragePanel panel;
        auto f = facts({ blob("a", "slot 1 take.wav", 100 * MB, 1000) }, 5 * GB);
        f.forecast = retention::forecast({}, 5000, 100 * MB, 5 * GB, 153 * GB);
        panel.show(f);
        CHECK_EQ(panel.forecastLine(), juce::String("Nothing has been kept yet, so there is no rate to go by."));
        // 200 MB kept eight weeks ago, the whole window: 25 MB a week, and
        // 100 MB of room -> four weeks
        f.forecast = retention::forecast({ { 2 * retention::kWeekMs, 200 * MB } }, 10 * retention::kWeekMs,
                                         5 * GB - 100 * MB, 5 * GB, 153 * GB);
        panel.show(f);
        CHECK_EQ(panel.forecastLine(),
                 juce::String("About 4 weeks until the limit at the current rate (25 MB a week)."));
    }

    return testkit::summary("history_storage_panel_tests");
}
