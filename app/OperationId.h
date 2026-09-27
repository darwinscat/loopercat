// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

//==============================================================================
// loopercat::opid — the identity of one mutation.
//
// The id identifies an operation in the history. Two operations must never
// share one, even when a batch starts them within the same second.
//
// The label a caller passes in is a readable prefix and nothing more — the core
// compares ids, it never parses them. Uniqueness lives in the tail: a tag drawn
// once per process, so a relaunch inside the same second cannot repeat the run
// before it, and a counter, so a run cannot repeat itself.
//==============================================================================
namespace loopercat::opid
{

inline std::string make(std::string_view label)
{
    static const std::string processTag = [] {
        static constexpr char digits[] = "0123456789abcdef";
        const auto value = static_cast<std::uint16_t>(std::random_device {}());
        std::string out(4, '0');
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = digits[(value >> ((3 - i) * 4)) & 0xF];
        return out;
    }();
    static std::atomic<std::uint64_t> counter { 0 };

    return std::string(label) + '-' + processTag + '-'
           + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

} // namespace loopercat::opid
