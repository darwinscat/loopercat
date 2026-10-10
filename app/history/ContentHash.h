// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_cryptography/juce_cryptography.h>

#include <string>
#include <string_view>

//==============================================================================
// loopercat::history::contentHash — the history's one name for a take's
// bytes: SHA-256, raw, 32 bytes. It keys blobs_meta and blobs, it is what
// slot_audio.hash names, and it is what a loudness reading is filed under
// (#140). Every place that hashes bytes for the history comes here — the
// recorder archiving a take, the player's read pass metering one, a loudness
// check reading one — so the same bytes get the same key wherever they were
// read, and a reading taken in one place is found from another.
//==============================================================================
namespace loopercat::history
{

inline std::string contentHash(std::string_view bytes)
{
    const juce::SHA256 sha(bytes.data(), bytes.size());
    const juce::MemoryBlock raw = sha.getRawData();
    return std::string(static_cast<const char*>(raw.getData()), raw.getSize());
}

} // namespace loopercat::history
