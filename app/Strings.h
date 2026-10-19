// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>
#include <string>

namespace loopercat
{

// The one crossing from core std::strings (UTF-8 bytes: error messages with
// typographic dashes, filesystem paths, on-pedal filenames) into juce::String.
// juce::String(const char*/std::string) widens BYTES (Latin-1), which turns
// any multi-byte character into mojibake — every boundary goes through here.
inline juce::String utf8(const std::string& bytes)
{
    return juce::String::fromUTF8(bytes.c_str(), static_cast<int>(bytes.size()));
}

// The same crossing for a path's own UTF-8 (std::filesystem::path::u8string),
// which never goes through the host's narrow code page.
inline juce::String utf8(const std::u8string& bytes)
{
    return juce::String::fromUTF8(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
}

// And back: a file the player chose, as a path the core can open — through
// UTF-8 too, since a narrow std::string is read in the host's code page on
// Windows, where a folder named outside it would not be found.
inline std::filesystem::path fsPath(const juce::File& file)
{
    const juce::String full = file.getFullPathName();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(full.toRawUTF8())));
}

} // namespace loopercat
