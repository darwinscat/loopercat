// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "FileTime.h"
#include "HistoryRecorder.h"

#include <loopercat/Commands.hpp>
#include <loopercat/Normalize.hpp>
#include <loopercat/Volume.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <string>

//==============================================================================
// loopercat::history::readSlotLoudness — one slot's loudness read as the
// worker runs it, for Check loudness and the background check (issue #61):
// the take's bytes come off the card once and are measured, and the reading
// is filed in the history under the hash of those very bytes (#140) before
// the caller turns it into words.
//
// It files what the read saw too (#141): the file's name, size and stamp
// with the hash of the bytes read under them, so the next connect knows the
// file in the slot by the bytes, not by a row that only named it — when the
// file stood still under the read (one stat before, one after, and the bytes
// as long as the entry said), and not otherwise.
//
// The read is the job; the filing is its tail. A history with no card in
// front of it takes nothing (`kept` false, no failure), and a store that
// refuses the row is reported in `failure` with the reading still returned —
// the player asked how loud the loop is, and that answer does not depend on
// the history being able to remember it. A slot with no take, and bytes that
// are not the pedal's own stereo float, are errors as before.
//
// Worker thread, like every read of the card and every write to the store.
//==============================================================================
namespace loopercat::history
{

struct SlotLoudness {
    wav::LoudnessReading reading;
    std::string hash;    // of the bytes measured — the key the reading is filed under
    bool kept = false;   // the history holds the reading now
    std::string failure; // the store's refusal, when it refused; empty otherwise
};

inline SlotLoudness readSlotLoudness(const volume::fs::path& volume, int slot,
                                     HistoryRecorder& recorder)
{
    const std::vector<std::string> files = volume::listSlotWavs(volume, slot);
    if (files.empty())
        throw Error("slot " + std::to_string(slot) + " has no audio to measure");
    const volume::fs::path file = volume::wavDir(volume, slot) / files.front();
    const std::optional<FileStat> before = statFile(file);
    const std::string raw = commands::readFileBytes(file);
    const std::optional<FileStat> after = statFile(file);
    SlotLoudness out;
    out.reading = wav::measureLoudness(
        wav::BytesView(reinterpret_cast<const unsigned char*>(raw.data()), raw.size()));
    out.hash = HistoryStore::contentHash(raw);
    const bool stoodStill = before && after && *before == *after
                         && before->size == static_cast<std::int64_t>(raw.size());
    try {
        out.kept = recorder.reading(out.hash, out.reading);
        if (out.kept && stoodStill)
            recorder.sighted(volume, slot, files.front(), before->size, before->modifiedMs, out.hash);
    } catch (const std::exception& e) {
        out.failure = e.what(); // the store's refusal, or anything else the filing threw
    }
    return out;
}

} // namespace loopercat::history
