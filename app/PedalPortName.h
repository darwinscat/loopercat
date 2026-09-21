// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Rc0.hpp> // kFamilyName — the model this app speaks

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

//==============================================================================
// loopercat::portname — which pedal a MIDI endpoint name announces.
//
// Outside STORAGE the pedal is visible only as a USB-MIDI device, so its port
// name is the one clue to which pedal is on the bus. The RC family names
// every model the same way — "BOSS_RC-5" and "BOSS_RC-500" side by side on
// macOS (2026-09-16), and the RC-300, RC-505, RC-600 and RC-10R after them —
// and the name the pedal gives the bus is the same model name its card
// carries in the root element (rc0::kFamilyName).
//
// A substring search cannot tell those apart: "RC-5" sits inside
// "BOSS_RC-500" and "BOSS_RC-505". The model is therefore read as a whole
// token — "RC-" at the start of a word, a digit, then every letter and digit
// that follows — and compared whole. Only ASCII counts as a word character:
// a byte outside it, whatever an OS or driver wrapped the name in, ends the
// token like a space does.
//
// Pure: a name in, a decision out — the JUCE and ALSA enumerations that feed
// it stay in PedalLink.h and PedalLinkLinux.cpp, so a test can hand it the
// names directly.
//==============================================================================
namespace loopercat::portname {

namespace detail {

    inline bool isWordChar(char c)
    {
        const auto u = static_cast<unsigned char>(c);
        return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
    }

    inline char upper(char c)
    {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }

} // namespace detail

// The RC-series model the name announces — "RC-5", "RC-500", "RC-10R" —
// upper-cased the way the pedal spells it, or no value for any other device.
// The first such token wins; a pedal's endpoint names one model.
inline std::optional<std::string> announcedModel(std::string_view portName)
{
    static constexpr std::size_t kPrefixLength = 3; // "RC-"
    for (std::size_t at = 0; at + kPrefixLength < portName.size(); ++at) {
        if (at > 0 && detail::isWordChar(portName[at - 1]))
            continue;
        if (detail::upper(portName[at]) != 'R' || detail::upper(portName[at + 1]) != 'C'
            || portName[at + 2] != '-')
            continue;
        const std::size_t first = at + kPrefixLength;
        if (portName[first] < '0' || portName[first] > '9')
            continue;
        std::string model = "RC-";
        for (std::size_t i = first; i < portName.size() && detail::isWordChar(portName[i]); ++i)
            model.push_back(detail::upper(portName[i]));
        return model;
    }
    return std::nullopt;
}

// Is this the endpoint of the pedal LooperCat speaks to?
inline bool isRc5(std::string_view portName)
{
    const auto model = announcedModel(portName);
    return model.has_value() && *model == rc0::kFamilyName;
}

} // namespace loopercat::portname
