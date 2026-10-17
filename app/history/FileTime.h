// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Error.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

//==============================================================================
// loopercat::history::statFile — what a file's directory entry says about
// it, size and modification time, from ONE query of the file system. The
// connect scan asks this of every take on every poll over a USB mount (#141),
// so the facts come together or not at all, never from three separate stats
// that could each see another file.
//
// The time is in milliseconds since the epoch, the clock every other time in
// the store is kept in (ops.at, blobs_meta.created), and it is the number
// JUCE's File::getLastModificationTime gives for the same file on each
// platform — macOS keeps the milliseconds, Linux whole seconds, Windows the
// FILETIME's — so stamps stored before this function existed compare equal
// to the ones it reads. #141 compares two sightings of one unchanged file for
// equality, so the number has to be the platform's own, exactly, every time.
//
// No answer for an entry that is not a regular file (a folder, a link to
// nothing) or that the platform will not describe: the caller decides what
// an undescribed file means. modifiedMs is the time alone, for a caller that
// must have it: an entry that gives none is an error there, since
// slot_audio.modified is NULL only where a row may not carry a stamp
// (Schema.h), and a null that meant "could not tell" would read as one.
//==============================================================================
namespace loopercat::history
{

struct FileStat
{
    std::int64_t size = 0;
    std::int64_t modifiedMs = 0;

    bool operator==(const FileStat&) const = default;
};

inline std::optional<FileStat> statFile(const std::filesystem::path& file)
{
#if defined(_WIN32)
    // A directory entry built from a path queries the file once and keeps
    // what it learned: the type, the size and the write time below are read
    // from that one answer. The write time is a FILETIME (100 ns ticks since
    // 1601-01-01), which JUCE turns into milliseconds since 1970 this way.
    constexpr std::int64_t kFiletimeAtUnixEpoch = 116444736000000000LL;
    constexpr std::int64_t kFiletimeTicksPerMs = 10000;
    std::error_code ec;
    const std::filesystem::directory_entry entry(file, ec);
    if (ec || !entry.is_regular_file(ec) || ec)
        return std::nullopt;
    const std::uintmax_t size = entry.file_size(ec);
    if (ec)
        return std::nullopt;
    const auto written = entry.last_write_time(ec);
    if (ec)
        return std::nullopt;
    return FileStat { static_cast<std::int64_t>(size),
                      (static_cast<std::int64_t>(written.time_since_epoch().count()) - kFiletimeAtUnixEpoch)
                          / kFiletimeTicksPerMs };
#else
    struct stat info {};
    if (::stat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
#if defined(__APPLE__)
    const std::int64_t ms = static_cast<std::int64_t>(info.st_mtimespec.tv_sec) * 1000
                          + static_cast<std::int64_t>(info.st_mtimespec.tv_nsec) / 1000000;
#else
    const std::int64_t ms = static_cast<std::int64_t>(info.st_mtime) * 1000;
#endif
    return FileStat { static_cast<std::int64_t>(info.st_size), ms };
#endif
}

inline std::int64_t modifiedMs(const std::filesystem::path& file)
{
    const std::optional<FileStat> stat = statFile(file);
    if (!stat)
        throw Error("cannot read the modification time of " + file.string());
    return stat->modifiedMs;
}

} // namespace loopercat::history
