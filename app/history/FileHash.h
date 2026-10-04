// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ContentHash.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <functional>
#include <optional>
#include <string>

//==============================================================================
// loopercat::history::fileContentHash — contentHash of a file on disk,
// streamed: the same 32-byte key the bytes in memory get, read through a
// 1 MiB buffer so a take of any length is never held whole (#140). The
// player's read pass hashes what it metered this way, on its own thread.
//
// The answer is the hash of the WHOLE file or nothing: a read that stopped
// short — the caller said stop, or the file could not be read to its end —
// returns nothing rather than the hash of a prefix. juce::SHA256 ends the
// digest on the first short read, so the bytes it consumed are counted and
// compared with the file's length.
//==============================================================================
namespace loopercat::history
{

namespace detail
{
    // The file stream as the digest sees it: dry the moment the caller says
    // stop, and counting every byte handed over.
    class CountedStream final : public juce::InputStream
    {
    public:
        CountedStream(juce::InputStream& source, const std::function<bool()>& shouldStop)
            : source_(source), shouldStop_(shouldStop)
        {
        }

        juce::int64 getTotalLength() override { return source_.getTotalLength(); }
        bool isExhausted() override { return source_.isExhausted(); }
        juce::int64 getPosition() override { return source_.getPosition(); }
        bool setPosition(juce::int64 position) override { return source_.setPosition(position); }
        int read(void* dest, int maxBytes) override
        {
            if (shouldStop_ && shouldStop_())
                return 0;
            const int got = source_.read(dest, maxBytes);
            if (got > 0)
                handedOver_ += got;
            return got;
        }
        juce::int64 handedOver() const { return handedOver_; }

    private:
        juce::InputStream& source_;
        const std::function<bool()>& shouldStop_;
        juce::int64 handedOver_ = 0;
    };
} // namespace detail

inline constexpr int kHashBufferBytes = 1 << 20;

inline std::optional<std::string> fileContentHash(const juce::File& file,
                                                  const std::function<bool()>& shouldStop = {})
{
    juce::FileInputStream in(file);
    if (!in.openedOk())
        return std::nullopt;
    const juce::int64 length = in.getTotalLength();
    if (length < 0)
        return std::nullopt;
    detail::CountedStream counted(in, shouldStop);
    // The digest reads 64 bytes at a time; the buffer turns that into one
    // read of the file per MiB.
    juce::BufferedInputStream buffered(&counted, kHashBufferBytes, false);
    const juce::SHA256 sha(buffered);
    if (counted.handedOver() != length)
        return std::nullopt;
    const juce::MemoryBlock raw = sha.getRawData();
    return std::string(static_cast<const char*>(raw.getData()), raw.getSize());
}

} // namespace loopercat::history
