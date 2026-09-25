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

#include "DeviceProfile.hpp"
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

// One track's audio folder: the memory number, three digits, then the track
// number — 001_1 on the RC-5, 001_1 and 001_2 on the two-track model (its
// card: 99 × 2 folders, plus the pedal's TEMP). The profile says how many
// tracks a memory has, so a track the model does not have is refused, not
// spelled.
inline std::string trackDirName(const profile::DeviceProfile& family, int slot, int track)
{
    if (slot < 1 || slot > rc0::kSlotCount)
        throw Error("slot out of range 1.." + std::to_string(rc0::kSlotCount) + ": "
                    + std::to_string(slot));
    if (track < 1 || track > family.trackCount)
        throw Error("track out of range 1.." + std::to_string(family.trackCount) + " for the \""
                    + std::string(family.familyName) + "\" model: " + std::to_string(track));
    const std::string n = std::to_string(slot);
    return std::string(3 - n.size(), '0') + n + "_" + std::to_string(track);
}

// The one-track spelling, the RC-5's: NNN_1.
inline std::string slotDirName(int slot)
{
    return trackDirName(profile::kRc5, slot, 1);
}

inline fs::path dataDir(const fs::path& volume) { return volume / "ROLAND" / "DATA"; }

inline fs::path trackDir(const fs::path& volume, const profile::DeviceProfile& family, int slot,
                         int track)
{
    return volume / "ROLAND" / "WAVE" / trackDirName(family, slot, track);
}

// The RC-5's one track.
inline fs::path wavDir(const fs::path& volume, int slot)
{
    return trackDir(volume, profile::kRc5, slot, 1);
}

// The card carries two kinds of .RC0, each as a pair of banks: the memories
// and the pedal's own settings. Same layout, same trailer with its write
// counter, different contents — see Rc0.hpp and SystemFile.hpp.
enum class Bank { memory, system };

inline std::string_view bankStem(Bank bank)
{
    switch (bank) {
    case Bank::memory: return "MEMORY";
    case Bank::system: return "SYSTEM";
    }
    throw Error("unknown bank"); // an enumerator added without a name here
}

inline fs::path bankPath(const fs::path& volume, Bank bank, int fileNo)
{
    rc0::tailMarkerFor(fileNo); // validates fileNo is 1 or 2
    return dataDir(volume) / (std::string(bankStem(bank)) + std::to_string(fileNo) + ".RC0");
}

inline fs::path memoryPath(const fs::path& volume, int fileNo)
{
    return bankPath(volume, Bank::memory, fileNo);
}

inline fs::path systemPath(const fs::path& volume, int fileNo)
{
    return bankPath(volume, Bank::system, fileNo);
}

inline bool isJunkName(std::string_view name)
{
    return name.starts_with("._") || name == ".DS_Store" || name == "Thumbs.db"
        || name == "desktop.ini";
}

// Every junk file the app may have caused: at the volume ROOT, one level,
// and under the ROLAND tree, recursively. macOS writes AppleDouble sidecars
// onto FAT volumes even for xattr-free files (fresh files get
// com.apple.provenance) — and the RC-5 chokes on them at boot. The root
// level exists for the card marker (CardMarker.hpp): a write to
// /loopercat-card.json leaves a /._loopercat-card.json beside it, measured
// on both RC-5s and the RC-500 on 2026-09-24.
//
// The root is not descended into. The directories a host OS keeps there
// (.Spotlight-V100, .fseventsd, System Volume Information) are not ours to
// enter — the pedal has booted beside them for a year and a half, macOS
// refuses to open .Spotlight-V100 at all, and nothing in them is a sidecar
// of ours. ROLAND is the pedal's tree, where every sidecar is a boot hazard.
inline std::vector<fs::path> findJunk(const fs::path& volume)
{
    std::vector<fs::path> junk;
    {
        std::error_code ec;
        for (fs::directory_iterator it(volume, ec), end; !ec && it != end; it.increment(ec))
            if (!it->is_directory(ec) && isJunkName(it->path().filename().string()))
                junk.push_back(it->path());
    }
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(volume / "ROLAND", ec), end; !ec && it != end;
             it.increment(ec))
            if (!it->is_directory(ec) && isJunkName(it->path().filename().string()))
                junk.push_back(it->path());
    }
    return junk;
}

struct SweepResult {
    std::vector<fs::path> removed;
    std::vector<fs::path> failed; // junk that would not delete — still on the volume
};

// Remove them, best-effort. Every mutating command sweeps AFTER its real
// write has already succeeded, so a sidecar that will not delete is reported
// in `failed`, never thrown — an exception here would mislabel a completed
// write as a failure (and a swap would falsely un-swap its audio over a
// locked .DS_Store). A volume with any sidecar left is still one boot away
// from "LOOPER DATA READ ERR"; surfacing `failed` is the caller's job, and
// doctor() reports every remaining sidecar as an error regardless.
inline SweepResult sweepJunk(const fs::path& volume)
{
    SweepResult result;
    for (const auto& file : findJunk(volume)) {
        std::error_code ec;
        fs::remove(file, ec);
        (ec ? result.failed : result.removed).push_back(file);
    }
    return result;
}

// One track's audio files (usually 0 or 1), junk filtered, sorted for
// determinism. A missing folder is a normal empty track, not an error.
inline std::vector<std::string> listTrackWavs(const fs::path& volume,
                                              const profile::DeviceProfile& family, int slot,
                                              int track)
{
    const fs::path dir = trackDir(volume, family, slot, track);
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

// The RC-5's one track.
inline std::vector<std::string> listSlotWavs(const fs::path& volume, int slot)
{
    return listTrackWavs(volume, profile::kRc5, slot, 1);
}

} // namespace loopercat::volume
