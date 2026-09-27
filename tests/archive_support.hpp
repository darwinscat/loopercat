// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <loopercat/Commands.hpp>

namespace testkit {

// A test double for the core's archive callback. Production uses HistoryRecorder.
inline loopercat::commands::Archive fileArchive(const std::filesystem::path& root,
                                                const std::string& opId)
{
    return [home = root / opId](int slot, const std::string& name, std::string_view bytes) {
        const auto dir = home / loopercat::volume::slotDirName(slot);
        std::filesystem::create_directories(dir);
        if (std::filesystem::exists(dir / name))
            throw loopercat::Error("test archive already holds this take");
        loopercat::commands::writeFileBytes(dir / name, bytes);
    };
}

} // namespace testkit
