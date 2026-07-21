// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Minimal RIFF/WAVE header reader — just enough to validate a file for the
// RC-5 and count its frames (no decoding) — plus the pedal-canonical rewriter
// and the pull-side filename rules.
//
// Format knowledge source: rc5cat (lib/wav.js, lib/commands.js,
// macos/RC5Kit/Sources/RC5Kit/Wav.swift); the float32 canonical header is
// pinned byte-for-byte by fixtures/golden.json "canonicalFloat32Header".

#pragma once

#include "Error.hpp"
#include "Rc0.hpp" // defaultSlotName for pull naming

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loopercat::wav {

inline constexpr int kSampleRate = 44100;

using Bytes = std::vector<unsigned char>;
using BytesView = std::span<const unsigned char>;

struct Info {
    int formatTag;
    int channels;
    int sampleRate;
    int bitsPerSample;
    int blockAlign;
    std::int64_t frames;
    std::int64_t dataBytes;

    // "pcm16" / "pcm24" / "float32" — the tag names the pedal's two formats.
    std::string format() const
    {
        return (formatTag == 3 ? "float" : "pcm") + std::to_string(bitsPerSample);
    }

    bool operator==(const Info&) const = default;
};

namespace detail {

    inline std::int64_t u16(BytesView data, std::size_t offset)
    {
        return std::int64_t{ data[offset] } | std::int64_t{ data[offset + 1] } << 8;
    }

    inline std::int64_t u32(BytesView data, std::size_t offset)
    {
        return u16(data, offset) | u16(data, offset + 2) << 16;
    }

    inline bool chunkIdIs(BytesView data, std::size_t offset, std::string_view id)
    {
        for (std::size_t i = 0; i < id.size(); ++i)
            if (data[offset + i] != static_cast<unsigned char>(id[i]))
                return false;
        return true;
    }

} // namespace detail

inline Info readWavInfo(BytesView data)
{
    if (data.size() < 44 || !detail::chunkIdIs(data, 0, "RIFF") || !detail::chunkIdIs(data, 8, "WAVE"))
        throw Error("not a RIFF/WAVE file");

    bool haveFmt = false;
    int fmtTag = 0, channels = 0, blockAlign = 0, bitsPerSample = 0;
    std::int64_t sampleRate = 0;
    std::int64_t dataBytes = -1;
    std::int64_t offset = 12;
    const auto size = static_cast<std::int64_t>(data.size());
    while (offset + 8 <= size) {
        const auto o = static_cast<std::size_t>(offset);
        const std::int64_t chunkSize = detail::u32(data, o + 4);
        if (detail::chunkIdIs(data, o, "fmt ")) {
            if (chunkSize < 16)
                throw Error("malformed fmt chunk");
            fmtTag = static_cast<int>(detail::u16(data, o + 8));
            channels = static_cast<int>(detail::u16(data, o + 10));
            sampleRate = detail::u32(data, o + 12);
            blockAlign = static_cast<int>(detail::u16(data, o + 20));
            bitsPerSample = static_cast<int>(detail::u16(data, o + 22));
            haveFmt = true;
        } else if (detail::chunkIdIs(data, o, "data")) {
            if (offset + 8 + chunkSize > size)
                throw Error("truncated file: data chunk claims " + std::to_string(chunkSize)
                            + " bytes, file has " + std::to_string(size - offset - 8));
            dataBytes = chunkSize;
        }
        offset += 8 + chunkSize + (chunkSize % 2);
    }
    if (!haveFmt)
        throw Error("missing fmt chunk");
    if (dataBytes < 0)
        throw Error("missing data chunk");
    if (fmtTag != 1 && fmtTag != 3)
        throw Error("unsupported WAVE format tag " + std::to_string(fmtTag));
    if (blockAlign == 0)
        throw Error("malformed fmt chunk: blockAlign is 0");

    return { fmtTag, channels, static_cast<int>(sampleRate), bitsPerSample, blockAlign,
             dataBytes / blockAlign, dataBytes };
}

// Rewrite a WAV into the pedal's own canonical shape: RIFF + fmt + data,
// nothing else. The RC-5 rewrites non-canonical files (DAW metadata chunks)
// during boot-time indexing; handing it an already-canonical file means it
// never touches the upload. Header layout is copied byte-for-byte from files
// the pedal normalized itself: float gets a 28-byte fmt body (cbSize=10,
// extension zeroed), PCM the classic 16-byte one. Pinned by golden.json
// "canonicalFloat32Header".
inline Bytes canonicalize(BytesView data)
{
    const Info info = readWavInfo(data);

    std::int64_t offset = 12;
    std::int64_t dataStart = -1;
    while (offset + 8 <= static_cast<std::int64_t>(data.size())) {
        const auto o = static_cast<std::size_t>(offset);
        const std::int64_t chunkSize = detail::u32(data, o + 4);
        if (detail::chunkIdIs(data, o, "data")) {
            dataStart = offset + 8;
            break;
        }
        offset += 8 + chunkSize + (chunkSize % 2);
    }

    const std::int64_t fmtBody = info.formatTag == 3 ? 28 : 16;
    const std::int64_t byteRate = std::int64_t{ info.sampleRate } * info.blockAlign;
    const std::int64_t total = 12 + 8 + fmtBody + 8 + info.dataBytes;
    if (total - 8 > 0xffffffffLL || byteRate > 0xffffffffLL)
        throw Error("file too large for a RIFF header");

    Bytes out;
    out.reserve(static_cast<std::size_t>(total));
    const auto putAscii = [&out](std::string_view s) {
        for (const char c : s)
            out.push_back(static_cast<unsigned char>(c));
    };
    const auto put16 = [&out](std::int64_t v) {
        out.push_back(static_cast<unsigned char>(v & 0xff));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    };
    const auto put32 = [&put16](std::int64_t v) {
        put16(v & 0xffff);
        put16((v >> 16) & 0xffff);
    };

    putAscii("RIFF"); put32(total - 8); putAscii("WAVE");
    putAscii("fmt "); put32(fmtBody);
    put16(info.formatTag);
    put16(info.channels);
    put32(info.sampleRate);
    put32(byteRate);
    put16(info.blockAlign);
    put16(info.bitsPerSample);
    if (fmtBody == 28) {
        put16(10); // cbSize=10, extension stays zero
        for (int i = 0; i < 10; ++i)
            out.push_back(0);
    }
    putAscii("data"); put32(info.dataBytes);
    const auto src = static_cast<std::size_t>(dataStart);
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(src),
               data.begin() + static_cast<std::ptrdiff_t>(src + static_cast<std::size_t>(info.dataBytes)));
    return out;
}

// What we accept for upload. Roland documents 44.1 kHz stereo WAV at 16-bit,
// 24-bit and 32-bit float — float being the pedal's own native recording
// format; 16-bit and float32 are additionally verified on real hardware.
inline const Info& assertUploadable(const Info& info)
{
    if (info.sampleRate != kSampleRate)
        throw Error("sample rate must be " + std::to_string(kSampleRate) + " Hz, got "
                    + std::to_string(info.sampleRate));
    if (info.channels != 2)
        throw Error("file must be stereo, got " + std::to_string(info.channels) + " channel(s)");
    const std::string format = info.format();
    if (format != "pcm16" && format != "pcm24" && format != "float32")
        throw Error("format must be 16/24-bit PCM or 32-bit float, got " + format);
    return info;
}

// --- pull naming (mirrors rc5cat lib/commands.js) ---

// "NN - <cleaned name>.wav": whitespace collapsed, filesystem-hostile
// characters replaced, empty names fall back to the factory slot name.
inline std::string wavFileName(int slot, std::string_view name)
{
    std::string clean;
    clean.reserve(name.size());
    bool pendingSpace = false;
    for (const char c : name) {
        const bool isSpace = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        if (isSpace) {
            pendingSpace = !clean.empty(); // leading whitespace trims away
            continue;
        }
        if (pendingSpace) {
            clean.push_back(' ');
            pendingSpace = false;
        }
        constexpr std::string_view kHostile = "\\/:*?\"<>|";
        clean.push_back(kHostile.find(c) != std::string_view::npos ? '_' : c);
    }
    if (clean.empty())
        clean = rc0::defaultSlotName(slot);
    const std::string n = std::to_string(slot);
    return (n.size() < 2 ? "0" + n : n) + " - " + clean + ".wav";
}

// A filename the pedal manufactured rather than a human chose: DOS 8.3
// leftovers from FAT directory rebuilds ("FIFTH-~2.WAV") or bare uppercase
// 8.3 names. Those get replaced by "NN - Slot Name.wav" on pull; meaningful
// names (a DAW export you uploaded) are kept as-is.
inline bool isTechnicalWavName(std::string_view name)
{
    for (std::size_t i = 0; i + 1 < name.size(); ++i)
        if (name[i] == '~' && name[i + 1] >= '0' && name[i + 1] <= '9')
            return true;
    constexpr std::string_view kSuffix = ".WAV";
    if (name.size() < kSuffix.size() + 1 || name.size() > 8 + kSuffix.size())
        return false;
    if (name.substr(name.size() - kSuffix.size()) != kSuffix)
        return false;
    for (const char c : name.substr(0, name.size() - kSuffix.size()))
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '~' || c == '_' || c == '-'))
            return false;
    return true;
}

inline std::string pullFileName(int slot, std::string_view slotName, std::string_view onPedalName)
{
    return isTechnicalWavName(onPedalName) ? wavFileName(slot, slotName)
                                           : std::string(onPedalName);
}

} // namespace loopercat::wav
