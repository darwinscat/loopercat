// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "HistoryStore.h"

#include <loopercat/Commands.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

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
