// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pedal as a mounted USB volume: layout, detection, junk hygiene.
//
// The pedal is found by its content (ROLAND/DATA + ROLAND/WAVE), not by its
// volume label — a renamed volume still works. macOS writes AppleDouble
// sidecars ("._name", ".DS_Store") onto FAT volumes to carry extended
// attributes; the RC-5 chokes on them, so anything junk-shaped is filtered
// out of every listing here (and swept after writes, once mutations land).
//
// Format knowledge source: rc5cat lib/volume.js.

#pragma once

#include "Error.hpp"
#include "Rc0.hpp" // kSlotCount

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loopercat::volume {

namespace fs = std::filesystem;

inline bool looksLikePedal(const fs::path& root)
{
    std::error_code ec;
    return fs::exists(root / "ROLAND" / "DATA", ec) && fs::exists(root / "ROLAND" / "WAVE", ec);
}

namespace detail {

    inline std::vector<fs::path> listDirs(const fs::path& root)
    {
        std::vector<fs::path> out;
        std::error_code ec;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            out.push_back(it->path());
        return out;
    }

} // namespace detail

// Where a mounted pedal can appear, per platform.
inline std::vector<fs::path> candidateVolumes()
{
#if defined(__APPLE__)
    return detail::listDirs("/Volumes");
#elif defined(_WIN32)
    std::vector<fs::path> out;
    for (char letter = 'C'; letter <= 'Z'; ++letter)
        out.push_back(std::string{ letter } + ":\\");
    return out;
#else
    std::vector<fs::path> out;
    for (const auto& media : detail::listDirs("/media")) {
        out.push_back(media);
        for (const auto& nested : detail::listDirs(media))
            out.push_back(nested);
    }
    for (const auto& user : detail::listDirs("/run/media"))
        for (const auto& nested : detail::listDirs(user))
            out.push_back(nested);
    for (const auto& mnt : detail::listDirs("/mnt"))
        out.push_back(mnt);
    return out;
#endif
}

// The first candidate that has pedal content, or no value.
inline std::optional<fs::path> detectVolume(const std::vector<fs::path>& candidates)
{
    for (const auto& candidate : candidates)
        if (looksLikePedal(candidate))
            return candidate;
    return std::nullopt;
}

inline std::string slotDirName(int slot)
{
    if (slot < 1 || slot > rc0::kSlotCount)
        throw Error("slot out of range 1.." + std::to_string(rc0::kSlotCount) + ": "
                    + std::to_string(slot));
    const std::string n = std::to_string(slot);
    return std::string(3 - n.size(), '0') + n + "_1";
}

inline fs::path dataDir(const fs::path& volume) { return volume / "ROLAND" / "DATA"; }

inline fs::path wavDir(const fs::path& volume, int slot)
{
    return volume / "ROLAND" / "WAVE" / slotDirName(slot);
}

inline fs::path memoryPath(const fs::path& volume, int fileNo)
{
    rc0::tailMarkerFor(fileNo); // validates fileNo is 1 or 2
    return dataDir(volume) / ("MEMORY" + std::to_string(fileNo) + ".RC0");
}

inline bool isJunkName(std::string_view name)
{
    return name.starts_with("._") || name == ".DS_Store" || name == "Thumbs.db"
        || name == "desktop.ini";
}

// Every junk file under the volume's ROLAND tree, recursively. macOS writes
// AppleDouble sidecars onto FAT volumes even for xattr-free files (fresh
// files get com.apple.provenance) — and the RC-5 chokes on them at boot.
inline std::vector<fs::path> findJunk(const fs::path& volume)
{
    std::vector<fs::path> junk;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(volume / "ROLAND", ec), end; !ec && it != end;
         it.increment(ec))
        if (!it->is_directory(ec) && isJunkName(it->path().filename().string()))
            junk.push_back(it->path());
    return junk;
}

// Remove them. Every mutating command sweeps afterwards — a volume with any
// sidecar left is one boot away from "LOOPER DATA READ ERR".
inline std::vector<fs::path> sweepJunk(const fs::path& volume)
{
    const std::vector<fs::path> junk = findJunk(volume);
    for (const auto& file : junk) {
        std::error_code ec;
        if (!fs::remove(file, ec) || ec)
            throw Error("cannot remove junk file " + file.string());
    }
    return junk;
}

// Slot audio files (usually 0 or 1), junk filtered, sorted for determinism.
// A missing slot directory is a normal empty slot, not an error.
inline std::vector<std::string> listSlotWavs(const fs::path& volume, int slot)
{
    const fs::path dir = wavDir(volume, slot);
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!isJunkName(name))
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace loopercat::volume
