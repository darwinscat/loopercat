// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <juce_core/juce_core.h>

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

} // namespace loopercat
