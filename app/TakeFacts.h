// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "history/FileTime.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

//==============================================================================
// loopercat::TakeFacts — what a take's directory entry says about it for free
// (#141): its name, its size and its modification time. No byte of audio is
// read for these, which is the point: the connect scan lists directories and
// reads the two memory files, and that is all it may cost. Together with the
// slot's WavLen these facts identify a take well enough for the history to
// say what it already knows about those bytes (HistoryStore::hashOfSighting).
//
// The facts come from one stat (history::statFile), read the way the store
// writes them: the platform's own stamp, exactly, because the history
// compares two sightings of one unchanged file for equality — FAT keeps it
// at two-second steps, and a tolerance would make two files one.
//
// An entry the file system will not describe whole — not a regular file, a
// link to nothing, a name with nothing behind it — has no facts: the slot
// keeps its name on screen and nothing is inferred about it, rather than the
// whole scan failing on one take.
//==============================================================================
namespace loopercat
{

struct TakeFacts
{
    std::string name;
    std::int64_t size = 0;
    std::int64_t modifiedMs = 0;

    bool operator==(const TakeFacts&) const = default;
};

inline std::optional<TakeFacts> takeFacts(const std::filesystem::path& file)
{
    const std::optional<history::FileStat> stat = history::statFile(file);
    if (!stat)
        return std::nullopt;
    return TakeFacts { file.filename().string(), stat->size, stat->modifiedMs };
}

} // namespace loopercat
