// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The retention policy (issue #74), attacked from what it promises — the
// edge cases the issue lists are the whole point:
//
//   - more is pinned than the limit allows: nothing to free, and it says so
//   - one take is larger than the whole limit: it stays while held, and the
//     person hears that it alone would not fit
//   - everything is held by the undo on offer: nothing to free, honestly
//   - a shared take is held while any row holds it
//   - a deduplicated take counts once, not once per row
//   - passing the limit is reported, never acted on: a plan releases nothing
//   - the oldest unheld take goes first, and no more than the target needs

#include "support.hpp"

#include "../app/history/Retention.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace loopercat;
namespace retention = history::retention;
using retention::Blob;
using retention::Plan;

namespace {

constexpr std::int64_t MB = std::int64_t { 1 } << 20;
constexpr std::int64_t GB = std::int64_t { 1 } << 30;

Blob blob(const std::string& hash, std::int64_t size, std::int64_t created, int references = 1)
{
    Blob b;
    b.hash = hash;
    b.size = size;
    b.created = created;
    b.references = references;
    return b;
}

Blob pinned(Blob b) { b.pinned = true; return b; }
Blob undo(Blob b) { b.undo = true; return b; }
Blob inFlight(Blob b) { b.inFlight = true; return b; }

std::vector<std::string> hashes(const std::vector<Blob>& blobs)
{
    std::vector<std::string> out;
    for (const auto& b : blobs)
        out.push_back(b.hash);
    return out;
}

bool contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

int main()
{
    // --- more is pinned than the limit allows: an 8 GB snapshot under 5 GB ---
    {
        std::vector<Blob> blobs;
        for (int i = 0; i < 8; ++i)
            blobs.push_back(pinned(blob("p" + std::to_string(i), GB, 1000 + i)));
        const Plan plan = retention::plan(blobs, retention::kDefaultLimit);
        CHECK_EQ(retention::kDefaultLimit, 5 * GB);
        CHECK_EQ(plan.kept, 8 * GB);
        CHECK_EQ(plan.held, 8 * GB);
        CHECK_EQ(plan.releasable(), 0);
        CHECK_EQ(plan.release.size(), 0u);
        CHECK_EQ(plan.freed, 0);
        CHECK_EQ(plan.after(), 8 * GB);
        CHECK(!plan.withinTarget());
        CHECK(!plan.reachesTarget());
        CHECK_EQ(plan.holds.size(), 8u);
        const std::string words = retention::describe(plan);
        CHECK(contains(words, "Nothing can be freed"));
        CHECK(contains(words, "8 GB is pinned"));
        CHECK(contains(words, "limit of 5 GB"));
        // the pins are exactly as they were: a plan reads, it does not write
        for (const auto& b : blobs)
            CHECK(b.pinned);
    }

    // --- one take larger than the whole limit: held, it stays, and is named ---
    {
        const std::vector<Blob> blobs = { undo(blob("big", 161 * MB, 1000)),
                                          blob("small", 3 * MB, 900) };
        const Plan plan = retention::plan(blobs, 100 * MB);
        CHECK_EQ(plan.kept, 164 * MB);
        CHECK_EQ(plan.release.size(), 1u); // the small one is offered, it is all there is
        CHECK_EQ(plan.freed, 3 * MB);
        CHECK(!plan.reachesTarget());
        const std::string words = retention::describe(plan);
        CHECK(contains(words, "Freeing 1 take gives back 3 MB"));
        CHECK(contains(words, "161 MB is needed by undo"));
        CHECK(contains(words, "one take alone, 161 MB, is larger than the limit"));
        // no longer held: it can go like any other, on the button
        const Plan later = retention::plan({ blob("big", 161 * MB, 1000) }, 100 * MB);
        CHECK_EQ(later.release.size(), 1u);
        CHECK(later.reachesTarget());
        CHECK_EQ(later.after(), 0);
    }

    // --- everything is held by the undo on offer ---
    {
        const std::vector<Blob> blobs = { undo(blob("a", 2 * MB, 1)), undo(blob("b", 2 * MB, 2)),
                                          undo(blob("c", 2 * MB, 3)) };
        const Plan plan = retention::plan(blobs, 4 * MB);
        CHECK_EQ(plan.release.size(), 0u);
        CHECK_EQ(plan.held, 6 * MB);
        CHECK(!plan.reachesTarget());
        const std::string words = retention::describe(plan);
        CHECK(contains(words, "Nothing can be freed"));
        CHECK(contains(words, "6 MB is needed by undo"));
        CHECK(!contains(words, "pinned"));
        CHECK(!contains(words, "larger than the limit"));
    }

    // --- an operation still running holds what it named ---
    {
        const Plan plan = retention::plan({ inFlight(blob("a", 5 * MB, 1)) }, MB);
        CHECK_EQ(plan.release.size(), 0u);
        CHECK(contains(retention::describe(plan), "belongs to an operation still running"));
    }

    // --- a shared take is held while any row holds it; free once the last lets go ---
    {
        Blob shared = blob("s", 4 * MB, 10, 2);
        shared.undo = true;
        CHECK_EQ(retention::plan({ shared }, 0).release.size(), 0u);
        shared.undo = false;
        shared.pinned = true;
        CHECK_EQ(retention::plan({ shared }, 0).release.size(), 0u);
        shared.pinned = false;
        const Plan free = retention::plan({ shared }, 0);
        CHECK_EQ(free.release.size(), 1u);
        CHECK_EQ(free.release.front().references, 2);
    }

    // --- a deduplicated take counts once against the limit ---
    {
        const Plan plan = retention::plan({ blob("d", 10 * MB, 1, 3) }, 25 * MB);
        CHECK_EQ(plan.kept, 10 * MB); // not 30
        CHECK(plan.withinTarget());
        CHECK_EQ(plan.release.size(), 0u);
        CHECK(contains(retention::describe(plan), "within the limit"));
    }

    // --- the oldest unheld take goes first, and no more than the target needs ---
    {
        const std::vector<Blob> blobs = {
            blob("newest", 4 * MB, 500),
            pinned(blob("oldest-but-pinned", 4 * MB, 100)),
            blob("old", 4 * MB, 200),
            blob("older-same-second-small", 1 * MB, 150),
            blob("older-same-second-big", 3 * MB, 150),
            blob("middle", 4 * MB, 300),
        };
        // 20 MB kept, 16 releasable; 9 MB target: needs 11 MB gone
        const Plan plan = retention::plan(blobs, 9 * MB);
        CHECK_EQ(plan.kept, 20 * MB);
        const std::vector<std::string> expected { "older-same-second-big", "older-same-second-small",
                                                  "old", "middle" };
        CHECK(hashes(plan.release) == expected);
        CHECK_EQ(plan.freed, 12 * MB);
        CHECK_EQ(plan.after(), 8 * MB);
        CHECK(plan.reachesTarget());
        CHECK(contains(retention::describe(plan), "Freeing 4 takes gives back 12 MB, leaving 8 MB."));
        // a target of zero asks for everything unheld, oldest first
        const Plan all = retention::plan(blobs, 0);
        CHECK_EQ(all.release.size(), 5u);
        CHECK_EQ(all.release.front().hash, std::string("older-same-second-big"));
        CHECK_EQ(all.release.back().hash, std::string("newest"));
        CHECK_EQ(all.after(), 4 * MB); // the pinned one
        CHECK(!all.reachesTarget());
        // the input is not the output's order: a plan sorts its own copy
        CHECK_EQ(blobs.front().hash, std::string("newest"));
    }

    // --- passing the limit is only reported ---
    {
        const std::vector<Blob> blobs = { blob("a", 3 * MB, 1), blob("b", 3 * MB, 2) };
        const Plan plan = retention::plan(blobs, 5 * MB);
        CHECK(!plan.withinTarget());
        CHECK_EQ(plan.release.size(), 1u); // what WOULD go
        CHECK_EQ(blobs.size(), 2u);        // and nothing did: the plan has no hands
        CHECK_EQ(plan.holds.size(), 0u);
    }

    // --- nothing kept, nothing to say beyond that ---
    {
        const Plan plan = retention::plan({}, 5 * GB);
        CHECK_EQ(plan.kept, 0);
        CHECK(plan.withinTarget());
        CHECK(contains(retention::describe(plan), "keeps 0 bytes"));
    }

    // --- refusals ---
    {
        CHECK_THROWS(retention::plan({}, -1), "negative");
        CHECK_THROWS(retention::plan({ blob("x", -5, 1) }, 0), "negative");
        CHECK_THROWS(retention::bytesText(-1), "negative");
    }

    // --- the words for bytes: what the Settings line shows ---
    {
        CHECK_EQ(retention::bytesText(0), std::string("0 bytes"));
        CHECK_EQ(retention::bytesText(1), std::string("1 byte"));
        CHECK_EQ(retention::bytesText(1023), std::string("1023 bytes"));
        CHECK_EQ(retention::bytesText(1024), std::string("1 KB"));
        CHECK_EQ(retention::bytesText(161 * MB), std::string("161 MB"));
        CHECK_EQ(retention::bytesText(1534 * MB), std::string("1.5 GB"));
        CHECK_EQ(retention::bytesText(5 * GB), std::string("5 GB"));
        CHECK_EQ(retention::bytesText(1720426076), std::string("1.6 GB"));
        CHECK_EQ(retention::bytesText(2 * GB - 1), std::string("2 GB"));
    }

    return testkit::summary("retention_policy_tests");
}
