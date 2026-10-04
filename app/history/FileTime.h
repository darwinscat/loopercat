// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <juce_core/juce_core.h>

#include <cstdint>
#include <filesystem>

//==============================================================================
// loopercat::history::modifiedMs — a file's modification time as its
// directory entry carries it, in milliseconds since the epoch: the clock
// every other time in the store is kept in (ops.at, blobs_meta.created).
//
// Read through JUCE rather than std::filesystem: file_time_type has no
// portable conversion to the system clock across the three standard
// libraries the app builds with, and the usual two-now() workaround can be a
// millisecond off between two readings. #141 compares two sightings of one
// unchanged file for equality, so the number has to be the platform's own
// stamp, exactly, every time.
//
// One stat, read as the answer: JUCE answers a file it cannot stat with the
// epoch itself, and the epoch is not a time a take on a card was written at,
// so that answer is an error here, not a null. slot_audio.modified is NULL
// only in rows written before the store asked (Schema.h), and a null that
// meant "could not tell" would read as one of those.
//==============================================================================
namespace loopercat::history
{

inline std::int64_t modifiedMs(const std::filesystem::path& file)
{
    const std::u8string utf8 = file.u8string();
    const juce::File entry(juce::String::fromUTF8(reinterpret_cast<const char*>(utf8.data()),
                                                  static_cast<int>(utf8.size())));
    const std::int64_t stamp = entry.getLastModificationTime().toMilliseconds();
    if (stamp == 0)
        throw Error("cannot read the modification time of " + file.string());
    return stamp;
}

} // namespace loopercat::history
