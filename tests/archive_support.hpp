// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <loopercat/Commands.hpp>

namespace testkit {

inline loopercat::commands::Journal noOpJournal()
{
    return { .bodiesChanging = [](const auto&) {}, .systemChanging = [](const auto&) {} };
}

inline loopercat::commands::WriteOptions unrecordedWrite()
{
    return { .journal = noOpJournal() };
}

// A test double for the core's archive callback. Production uses HistoryRecorder.
// Each take is kept under its slot and track, spelled as the card spells the
// track's folder (NNN_t): the two takes of a two-track memory can share a name.
inline loopercat::commands::Archive fileArchive(const std::filesystem::path& root,
                                                const std::string& opId)
{
    return [home = root / opId](int slot, int track, const std::string& name,
                                std::string_view bytes) {
        const std::string n = std::to_string(slot);
        const auto dir = home / (std::string(3 - n.size(), '0') + n + "_" + std::to_string(track));
        std::filesystem::create_directories(dir);
        if (std::filesystem::exists(dir / name))
            throw loopercat::Error("test archive already holds this take");
        loopercat::commands::writeFileBytes(dir / name, bytes);
    };
}

} // namespace testkit
