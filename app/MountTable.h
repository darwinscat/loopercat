// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <istream>
#include <optional>
#include <string>
#include <vector>

//==============================================================================
// loopercat::mounttable — the Linux mount table, parsed.
//
// /proc/self/mountinfo is how the Linux DeviceWatcher learns which block
// device backs a mount point. Its format (Linux Documentation/filesystems/
// proc.rst, "3.5 /proc/<pid>/mountinfo") is:
//
//   36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
//   (1)(2)(3)  (4)   (5)   (6)        (7)     (8)(9)  (10)      (11)
//
// with (7) a VARIABLE number of optional tagged fields terminated by the
// lone "-" separator — so fields after it can only be reached by scanning
// for that separator, never by a fixed index.
//
// The escaping is the part that bites: mountinfo replaces space, tab,
// newline and backslash with their three-digit OCTAL escapes so its own
// whitespace splitting stays unambiguous. The pedal mounts as
// "/media/<user>/BOSS RC-5", so its mount point always arrives containing
// \040. A parser that skips unescaping never matches the real path, and the
// device-truth layer would silently answer `untracked` for every pedal
// forever — no error, no log, just a port that quietly does nothing.
//
// Parsing lives here, apart from the syscalls, so it can be tested from a
// fixture string on any platform rather than only on a live Linux box.
//==============================================================================
namespace loopercat::mounttable {

struct Entry {
    unsigned int deviceMajor = 0;
    unsigned int deviceMinor = 0;
    std::string mountPoint; // unescaped
    std::string source;     // unescaped block device node, e.g. /dev/sdb1
};

// Octal escapes back to their characters. A backslash that does not begin a
// valid three-digit octal escape is a literal backslash — mountinfo only
// escapes the four characters that would break field splitting, so anything
// else must survive untouched.
inline std::string unescape(const std::string& field)
{
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        const bool escaped = field[i] == '\\' && i + 3 < field.size()
                          && field[i + 1] >= '0' && field[i + 1] <= '3'
                          && field[i + 2] >= '0' && field[i + 2] <= '7'
                          && field[i + 3] >= '0' && field[i + 3] <= '7';
        if (!escaped) {
            out.push_back(field[i]);
            continue;
        }
        const int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8
                        + (field[i + 3] - '0');
        out.push_back(static_cast<char>(value));
        i += 3;
    }
    return out;
}

// A decimal field, or no value when it is empty, non-numeric, or wider than
// a device number can be. Hand-rolled rather than stoul: a malformed line is
// data we do not control, and skipping it must not cost an exception.
inline std::optional<unsigned int> parseNumber(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    unsigned long long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + static_cast<unsigned long long>(c - '0');
        if (value > 0xFFFFFFFFull)
            return std::nullopt;
    }
    return static_cast<unsigned int>(value);
}

// Every well-formed line. A line that does not parse is dropped rather than
// guessed at — a half-understood mount is worse than no mount at all.
inline std::vector<Entry> parse(std::istream& mountInfo)
{
    std::vector<Entry> entries;
    std::string line;
    while (std::getline(mountInfo, line)) {
        std::vector<std::string> fields;
        size_t position = 0;
        while (position < line.size()) {
            const size_t start = line.find_first_not_of(' ', position);
            if (start == std::string::npos)
                break;
            const size_t end = line.find(' ', start);
            fields.push_back(line.substr(start, end == std::string::npos ? end : end - start));
            position = end == std::string::npos ? line.size() : end + 1;
        }
        // id, parent, major:minor, root, mount point — then at least the
        // separator, the filesystem type and the source.
        if (fields.size() < 8)
            continue;

        const std::string& majorMinor = fields[2];
        const size_t colon = majorMinor.find(':');
        if (colon == std::string::npos)
            continue;
        const std::optional<unsigned int> major = parseNumber(majorMinor.substr(0, colon));
        const std::optional<unsigned int> minor = parseNumber(majorMinor.substr(colon + 1));
        if (!major || !minor)
            continue;

        // The optional fields are variable in number; the lone "-" ends them
        // and the two fields after it are the type and the source.
        size_t separator = 6;
        while (separator < fields.size() && fields[separator] != "-")
            ++separator;
        if (separator + 2 >= fields.size())
            continue;

        Entry entry;
        entry.deviceMajor = *major;
        entry.deviceMinor = *minor;
        entry.mountPoint = unescape(fields[4]);
        entry.source = unescape(fields[separator + 2]);
        entries.push_back(std::move(entry));
    }
    return entries;
}

// The mount whose mount point IS this path. Exact match by design: a path
// inside a mounted volume is a directory on it, not a medium of its own.
inline std::optional<Entry> findByMountPoint(const std::vector<Entry>& entries,
                                             const std::string& path)
{
    for (const Entry& entry : entries)
        if (entry.mountPoint == path)
            return entry;
    return std::nullopt;
}

inline bool isDeviceMounted(const std::vector<Entry>& entries, unsigned int major,
                            unsigned int minor)
{
    for (const Entry& entry : entries)
        if (entry.deviceMajor == major && entry.deviceMinor == minor)
            return true;
    return false;
}

} // namespace loopercat::mounttable
