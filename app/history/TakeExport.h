// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <loopercat/Commands.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

//==============================================================================
// loopercat::history::exportTake — an archived take, written where the
// player asked ("Export take to...", #73). The bytes live in the store now,
// so the app writes a file instead of revealing a folder.
//
// Whole or absent: the bytes land under a partial name and the file appears
// only by the rename, so a name that is there is a file that is complete. A
// take the store no longer keeps is refused before anything is written. A
// file already at `file` is replaced — the chooser that named it has asked.
//==============================================================================
namespace loopercat::history
{

inline constexpr const char* kExportPartial = ".part";
inline constexpr std::string_view kExportExtension = ".wav";

// The name the save dialog proposes for a take: the take's own name, the one
// the card gave it and the row shows, so the file a player saves is the take
// they recognize. It gains ".wav" only when it does not end so already, in
// any case — the pedal's own recordings are upper case. A take without a name
// (the store requires one, so this is a row from elsewhere) is proposed as
// "slot N take.wav": the dialog still opens on something the player can
// recognize and change.
inline std::string exportName(const std::string& takeName, int slot)
{
    if (takeName.empty())
        return "slot " + std::to_string(slot) + " take" + std::string(kExportExtension);
    const bool hasExtension = takeName.size() >= kExportExtension.size()
        && std::equal(kExportExtension.begin(), kExportExtension.end(),
                      takeName.end() - static_cast<std::ptrdiff_t>(kExportExtension.size()),
                      [](char want, char got) {
                          return want == static_cast<char>(std::tolower(static_cast<unsigned char>(got)));
                      });
    return hasExtension ? takeName : takeName + std::string(kExportExtension);
}

// Returns the bytes written.
inline std::int64_t exportTake(HistoryStore& store, const std::string& hash,
                               const std::filesystem::path& file)
{
    if (file.empty() || file.filename().empty())
        throw Error("the export needs a file name");
    const std::optional<std::string> bytes = store.takeBytes(hash);
    if (!bytes)
        throw Error("that take is no longer kept in the history");
    const std::filesystem::path partial(file.string() + kExportPartial);
    std::error_code ec;
    try {
        commands::writeFileBytes(partial, *bytes);
    } catch (...) {
        std::filesystem::remove(partial, ec);
        throw;
    }
    std::filesystem::rename(partial, file, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw Error("cannot write " + file.string() + ": " + ec.message());
    }
    return static_cast<std::int64_t>(bytes->size());
}

} // namespace loopercat::history
