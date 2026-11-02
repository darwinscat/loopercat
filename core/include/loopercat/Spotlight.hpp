// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Spotlight, kept off the pedal's card (issue #169).
//
// macOS indexes every volume it mounts, the pedal's card included: the card
// root carries a .Spotlight-V100 store and `mdutil -s` answers "Indexing
// enabled." Indexing reads the card's files right after the mount, which is
// exactly when a player disconnects after a quick look — and a file an
// indexer holds open refuses the unmount. That is how the first Disconnect
// after a Connect was refused as busy on 2026-10-19 (an RC-500, 57 s after
// Connect) and on 2026-11-02 (an RC-5, 25 s after): no DiskArbitration client
// dissented, the kernel answered EBUSY, and a second press went through.
//
// An empty .metadata_never_index at a volume's root tells Spotlight to leave
// that volume alone from its next mount on. Checked on macOS 26 (Darwin 25.5,
// 2026-11-02) on a FAT32 volume served by the same FSKit msdos module that
// serves the card, with indexing switched on and a .Spotlight-V100 store in
// place: "Indexing enabled." before the file, "Indexing and searching
// disabled." after a remount with it — the file outranks the store's own
// setting. Writing it changes nothing for the mount that is up: Spotlight
// decides at mount time, so the eject retries (app/EjectRetry.h) still carry
// the first session.
//
// What this file may do, and nothing more:
//
//   - create the file, empty, when the root has nothing by that name;
//   - leave anything already there exactly as it is — a file of any content,
//     a directory, a link. Never rewrite it, never delete it: whatever put it
//     there wanted Spotlight off too, or knows better than we do;
//   - never touch .Spotlight-V100. mds may hold the store open, and macOS
//     refuses to open it to anyone else anyway (Volume.hpp).
//
// The pedal ignores foreign files at the root (CardMarker.hpp: a marker file
// survived power cycles on both RC-5s and the RC-500). Linux and Windows read
// no such file; it costs nothing there and protects the card the day it meets
// a Mac. Like every write to the card, creating it ends with the sweep of the
// AppleDouble sidecar macOS plants beside a fresh file (measured on this one:
// a ._.metadata_never_index appeared next to it).

#pragma once

#include "Error.hpp"
#include "Volume.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Lean windows.h, for the same reason as CardMarker.hpp: the full header's
// rpcndr.h defines `small`, which breaks JUCE headers included after it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace loopercat::spotlight {

namespace fs = std::filesystem;

// Spotlight's own name for "do not index this volume", at the volume root.
inline constexpr std::string_view kNeverIndexFileName = ".metadata_never_index";

inline fs::path neverIndexPath(const fs::path& volume) { return volume / kNeverIndexFileName; }

struct KeptOff {
    bool created = false;      // the file was written just now; false: something by that name was there
    volume::SweepResult sweep; // after a write: the sidecar macOS planted, removed — or, in `failed`, not
};

namespace detail {

    // Create `file` empty, only if nothing by that name exists — atomically,
    // so a file that appears between the look and the write is still left
    // alone. True when this call created it.
    inline bool createExclusive(const fs::path& file)
    {
#if defined(_WIN32)
        const HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                return false;
            throw Error("cannot create " + file.string() + ": "
                        + std::system_category().message(static_cast<int>(error)));
        }
        if (!::CloseHandle(handle))
            throw Error("cannot close " + file.string() + ": "
                        + std::system_category().message(static_cast<int>(::GetLastError())));
#else
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST)
                return false;
            throw Error("cannot create " + file.string() + ": "
                        + std::error_code(errno, std::generic_category()).message());
        }
        if (::close(fd) != 0)
            throw Error("cannot close " + file.string() + ": "
                        + std::error_code(errno, std::generic_category()).message());
#endif
        return true;
    }

} // namespace detail

// Make sure the volume carries .metadata_never_index at its root. Writes only
// when nothing by that name is there; a failure to write is an Error with the
// path and the OS's reason.
inline KeptOff keepOff(const fs::path& volume)
{
    const fs::path file = neverIndexPath(volume);
    std::error_code ec;
    const fs::file_status status = fs::symlink_status(file, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        throw Error("cannot look for " + file.string() + ": " + ec.message());
    KeptOff kept;
    if (fs::exists(status) || !detail::createExclusive(file))
        return kept;
    kept.created = true;
    kept.sweep = volume::sweepJunk(volume);
    return kept;
}

} // namespace loopercat::spotlight
