// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

//==============================================================================
// loopercat::history::retention — what freeing space WOULD do (issue #74).
//
// The app deletes nothing by itself. It shows what the history costs and, on
// a button, releases the bytes of the oldest takes that nothing holds. This is
// the decision behind that button, and only the decision: a pure function from
// the store's facts about its blobs to a plan — what would go, in what order,
// how much comes back, and what stays and why. The store carries it out
// (HistoryStore::releaseBlobs) and refuses a held blob on its own terms too.
//
// What holds a blob, from #74, plus one guard:
//   - a pinned row names it (the pin is on the operation, #73; a pin on the
//     blob itself counts as well);
//   - the undo the app offers right now would put it back;
//   - a row of an operation still in flight names it.
// A blob is held while ANY row naming it holds it: an object two rows share is
// released only when the last one lets go. References are counted and
// reported; a deduplicated blob counts once against the limit, not once per
// row — the input is one entry per blob.
//
// Age is not a measure of a take's worth, so nothing here expires. Age only
// orders what the person is offered: the oldest unheld take first.
//==============================================================================
namespace loopercat::history::retention
{

// The limit's default: 5 GB (Alisa, 2026-09-24). It was 2 GB until the
// folders from before the store were imported on a real machine and put
// 1.7 GB in on day one.
inline constexpr std::int64_t kDefaultLimit = std::int64_t { 5 } << 30;

// One blob whose bytes the store keeps, as the store reports it.
struct Blob {
    std::string hash;
    std::int64_t size = 0;
    std::int64_t created = 0; // when the bytes were first kept, ms since the epoch
    int references = 0;       // rows naming the hash, both sides
    bool pinned = false;      // a pinned operation names it, or the blob is pinned
    bool undo = false;        // the offered undo would put it back
    bool inFlight = false;    // an operation still pending names it

    bool held() const { return pinned || undo || inFlight; }
};

struct Plan {
    std::int64_t target = 0;     // keep at most this many bytes
    std::int64_t kept = 0;       // kept now, each blob once
    std::int64_t held = 0;       // of those, bytes no release may touch
    std::int64_t freed = 0;      // what `release` gives back
    std::vector<Blob> release;   // oldest first; enough to reach the target, or all there is
    std::vector<Blob> holds;     // what stays held, oldest first

    std::int64_t after() const { return kept - freed; }
    std::int64_t releasable() const { return kept - held; }
    bool withinTarget() const { return kept <= target; }
    bool reachesTarget() const { return after() <= target; }
};

// Oldest unheld blobs first, until the kept bytes fit the target. With the
// target at 0 that is everything unheld. Passing the limit is only reported —
// nothing is released by a plan; the store releases what a person confirmed.
inline Plan plan(std::vector<Blob> blobs, std::int64_t target)
{
    if (target < 0)
        throw Error("the retention target cannot be negative");
    std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) {
        if (a.created != b.created)
            return a.created < b.created;
        if (a.size != b.size)
            return a.size > b.size;
        return a.hash < b.hash;
    });
    Plan out;
    out.target = target;
    for (const Blob& blob : blobs) {
        if (blob.size < 0)
            throw Error("a blob cannot have a negative size");
        out.kept += blob.size;
        if (blob.held()) {
            out.held += blob.size;
            out.holds.push_back(blob);
        }
    }
    for (const Blob& blob : blobs) {
        if (out.after() <= target)
            break; // within the target, from the start or by now: nothing more goes
        if (blob.held())
            continue;
        out.release.push_back(blob);
        out.freed += blob.size;
    }
    return out;
}

// "161 MB", "4.2 GB", "0 bytes": the shape the Settings line and the offer use.
inline std::string bytesText(std::int64_t bytes)
{
    if (bytes < 0)
        throw Error("a byte count cannot be negative");
    constexpr std::int64_t kKB = 1024;
    constexpr std::int64_t kMB = kKB * 1024;
    constexpr std::int64_t kGB = kMB * 1024;
    const auto oneDecimal = [](std::int64_t value, std::int64_t unit, const char* suffix) {
        const std::int64_t tenths = (value * 10 + unit / 2) / unit;
        std::string out = std::to_string(tenths / 10);
        if (tenths % 10 != 0)
            out += "." + std::to_string(tenths % 10);
        return out + suffix;
    };
    if (bytes >= kGB)
        return oneDecimal(bytes, kGB, " GB");
    if (bytes >= kMB)
        return std::to_string((bytes + kMB / 2) / kMB) + " MB";
    if (bytes >= kKB)
        return std::to_string((bytes + kKB / 2) / kKB) + " KB";
    return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
}

// The plan in one honest sentence: what freeing would give back, or why it
// cannot reach the target and what is holding the rest.
inline std::string describe(const Plan& plan)
{
    const auto takes = [](std::size_t n) {
        return std::to_string(n) + (n == 1 ? " take" : " takes");
    };
    if (plan.withinTarget())
        return "The history keeps " + bytesText(plan.kept) + ", within the limit of "
               + bytesText(plan.target) + "; nothing needs to go.";
    std::string text;
    if (plan.freed > 0)
        text = "Freeing " + takes(plan.release.size()) + " gives back " + bytesText(plan.freed)
               + ", leaving " + bytesText(plan.after());
    else
        text = "Nothing can be freed";
    if (plan.reachesTarget())
        return text + ".";
    // Why the rest stays: the holds, by kind, and the one take that alone
    // would not fit if there is one.
    std::int64_t pinned = 0, undo = 0, inFlight = 0;
    const Blob* largest = nullptr;
    for (const Blob& blob : plan.holds) {
        if (blob.pinned)
            pinned += blob.size;
        else if (blob.undo)
            undo += blob.size;
        else
            inFlight += blob.size;
        if (largest == nullptr || blob.size > largest->size)
            largest = &blob;
    }
    text += ": " + bytesText(plan.after()) + " stays above the limit of " + bytesText(plan.target);
    std::vector<std::string> reasons;
    if (pinned > 0)
        reasons.push_back(bytesText(pinned) + " is pinned");
    if (undo > 0)
        reasons.push_back(bytesText(undo) + " is needed by undo");
    if (inFlight > 0)
        reasons.push_back(bytesText(inFlight) + " belongs to an operation still running");
    if (!reasons.empty()) {
        text += " because ";
        for (std::size_t i = 0; i < reasons.size(); ++i)
            text += (i == 0 ? "" : i + 1 == reasons.size() ? " and " : ", ") + reasons[i];
    }
    if (largest != nullptr && largest->size > plan.target)
        text += "; one take alone, " + bytesText(largest->size) + ", is larger than the limit";
    return text + ".";
}

} // namespace loopercat::history::retention
