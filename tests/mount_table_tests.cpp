// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The mountinfo parser, tested from the format's specification (Linux
// Documentation/filesystems/proc.rst, "3.5 /proc/<pid>/mountinfo") rather
// than from the parser's own shape — every case here is a promise the
// FORMAT makes, so a parser rewritten from scratch must still pass.
//
// The stakes are specific: the pedal mounts as "/media/<user>/BOSS RC-5",
// whose space is escaped, and it sits on a variable-length line whose tail
// fields can only be found by scanning for the "-" separator. Get either
// wrong and every lookup misses — not loudly, but by answering "this is not
// a mount point", which the DeviceWatcher reads as "no device story here".
// So the suite leans on the ways the parser can be wrong QUIETLY.

#include "support.hpp"

#include "../app/MountTable.h"

#include <sstream>
#include <string>

using namespace loopercat::mounttable;

namespace {

std::vector<Entry> parseText(const std::string& text)
{
    std::istringstream stream(text);
    return parse(stream);
}

// A real mountinfo line for a udisks2-mounted pedal, captured shape: the
// mount point carries \040 for its space and the line has one optional
// field before the separator.
const char* const kPedalLine =
    "42 31 8:17 / /media/alisa/BOSS\\040RC-5 rw,nosuid,nodev,relatime shared:1 - vfat "
    "/dev/sdb1 rw,fmask=0022";

} // namespace

int main()
{
    // --- the escape that decides whether the port works at all ---

    {
        const std::vector<Entry> entries = parseText(kPedalLine);
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        // If this reads "/media/alisa/BOSS\040RC-5", every pedal on Linux is
        // invisible and nothing reports an error.
        CHECK_EQ(entries[0].mountPoint, std::string("/media/alisa/BOSS RC-5"));
        CHECK_EQ(entries[0].source, std::string("/dev/sdb1"));
        CHECK_EQ(entries[0].deviceMajor, 8u);
        CHECK_EQ(entries[0].deviceMinor, 17u);
    }

    // All four characters mountinfo escapes, not just the space that we
    // happen to hit in practice.
    {
        const std::vector<Entry> entries = parseText(
            "1 0 8:1 / /mnt/a\\040b\\011c\\012d\\134e rw - vfat /dev/sda1 rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].mountPoint, std::string("/mnt/a b\tc\nd\\e"));
    }

    // A backslash that begins no valid octal escape is a literal backslash.
    // Over-eager unescaping would corrupt a legitimate path.
    {
        const std::vector<Entry> entries =
            parseText("1 0 8:1 / /mnt/back\\slash rw - vfat /dev/sda1 rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].mountPoint, std::string("/mnt/back\\slash"));
    }

    // A truncated escape at the very end of the field must not read past it.
    {
        const std::vector<Entry> entries =
            parseText("1 0 8:1 / /mnt/trail\\04 rw - vfat /dev/sda1 rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].mountPoint, std::string("/mnt/trail\\04"));
    }

    // \134 is the escape for a backslash, so an escaped backslash followed
    // by digits must not be re-read as another escape.
    {
        const std::vector<Entry> entries =
            parseText("1 0 8:1 / /mnt/\\134040 rw - vfat /dev/sda1 rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].mountPoint, std::string("/mnt/\\040"));
    }

    // --- the variable-length optional fields ---

    // Zero optional fields: the separator sits right after the options.
    {
        const std::vector<Entry> entries =
            parseText("36 35 98:0 / /mnt1 rw,noatime - ext3 /dev/root rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].source, std::string("/dev/root"));
        CHECK_EQ(entries[0].deviceMajor, 98u);
        CHECK_EQ(entries[0].deviceMinor, 0u);
    }

    // Three optional fields: a fixed-index parser reads the wrong source
    // here while still returning an entry — the quiet failure this suite
    // exists for.
    {
        const std::vector<Entry> entries = parseText(
            "36 35 8:32 / /mnt2 rw shared:2 master:1 propagate_from:3 - btrfs /dev/sdc rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].source, std::string("/dev/sdc"));
        CHECK_EQ(entries[0].deviceMajor, 8u);
        CHECK_EQ(entries[0].deviceMinor, 32u);
    }

    // --- malformed input is dropped, never guessed at ---

    {
        // No separator at all; a non-numeric device; a missing minor; a line
        // that ends before the source; an empty line.
        const std::vector<Entry> entries = parseText(
            "1 0 8:1 / /mnt/a rw vfat /dev/sda1 rw\n"
            "2 0 xx:1 / /mnt/b rw - vfat /dev/sdb1 rw\n"
            "3 0 8 / /mnt/c rw - vfat /dev/sdc1 rw\n"
            "4 0 8:2 / /mnt/d rw - vfat\n"
            "\n"
            "5 0 8:3 / /mnt/good rw - vfat /dev/sdd1 rw\n");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(1));
        CHECK_EQ(entries[0].mountPoint, std::string("/mnt/good"));
    }

    // A device number too wide to be one is malformed, not truncated.
    {
        const std::vector<Entry> entries =
            parseText("1 0 99999999999:1 / /mnt/a rw - vfat /dev/sda1 rw");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(0));
    }

    // Empty input is empty output, not a crash.
    {
        CHECK_EQ(parseText("").size(), static_cast<std::size_t>(0));
    }

    // --- lookup semantics ---

    {
        const std::vector<Entry> entries = parseText(
            std::string(kPedalLine) + "\n"
            + "1 0 0:22 / /tmp rw - tmpfs tmpfs rw\n"
            + "2 0 8:2 / / rw - ext4 /dev/sda2 rw\n");
        CHECK_EQ(entries.size(), static_cast<std::size_t>(3));

        const std::optional<Entry> pedal =
            findByMountPoint(entries, "/media/alisa/BOSS RC-5");
        CHECK(pedal.has_value());
        // Guarded on purpose: a failed CHECK does not stop the suite, and
        // dereferencing the empty optional here would turn a readable FAIL
        // into a segfault — which is exactly what the first mutation run of
        // this file produced.
        if (pedal.has_value())
            CHECK_EQ(pedal->source, std::string("/dev/sdb1"));

        // A directory INSIDE the volume is not the volume: the DeviceWatcher
        // treats a non-mount-point as untracked on purpose, and a prefix
        // match here would hand it a device story it must not have.
        CHECK(!findByMountPoint(entries, "/media/alisa/BOSS RC-5/ROLAND").has_value());
        // The still-escaped spelling must NOT match, or the unescaping above
        // is decorative.
        CHECK(!findByMountPoint(entries, "/media/alisa/BOSS\\040RC-5").has_value());
        CHECK(!findByMountPoint(entries, "/media/alisa").has_value());

        CHECK(isDeviceMounted(entries, 8, 17));
        CHECK(!isDeviceMounted(entries, 8, 18));
        // Major 0 is a virtual filesystem — present in the table, but the
        // caller must be able to tell it apart by its major, not by absence.
        CHECK(isDeviceMounted(entries, 0, 22));
    }

    // --- the numeric field parser, at its edges ---

    {
        CHECK(!parseNumber("").has_value());
        CHECK(!parseNumber("12a").has_value());
        CHECK(!parseNumber("-1").has_value());
        CHECK(!parseNumber(" 1").has_value());
        CHECK_EQ(parseNumber("0").value_or(1u), 0u);
        CHECK_EQ(parseNumber("4294967295").value_or(0u), 4294967295u);
        CHECK(!parseNumber("4294967296").has_value());
    }

    return testkit::summary("mount table");
}
