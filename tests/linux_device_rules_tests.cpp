// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Linux backend's judgements, tested from what they PROMISE rather than
// from how they are written: a device is removable when the kernel says so
// or when it hangs off USB; the disk to power off is the parent block
// device and never its transport directory; and udisksctl gets exactly the
// arguments a musician's card can survive.
//
// The bias here is towards the two failures that would not announce
// themselves. Treating an internal disk as removable hands it a device
// story it must not have — the app would start ejecting the wrong thing.
// And an argument vector that drifts still compiles, still runs, and only
// misbehaves against real hardware, where the cost is someone's loops.

#include "support.hpp"

#include "../app/LinuxDeviceRules.h"

#include <string>
#include <vector>

using namespace loopercat::linuxrules;

namespace {

// vector<string> has no operator<<, and a joined form gives a readable diff.
std::string joined(const std::vector<std::string>& args)
{
    std::string out;
    for (const std::string& arg : args) {
        if (!out.empty())
            out += ' ';
        out += arg;
    }
    return out;
}

// Shapes taken from real sysfs layouts.
const char* const kUsbStick =
    "/sys/devices/pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host6/target6:0:0/6:0:0:0/block/sdb/sdb1";
const char* const kSataDisk =
    "/sys/devices/pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0/block/sda/sda1";

} // namespace

int main()
{
    // --- the USB transport, and the substring trap ---

    {
        CHECK(hasUsbTransport(kUsbStick));
        CHECK(!hasUsbTransport(kSataDisk));

        // A whole component of the form usb<digits> is the marker. These
        // all CONTAIN "usb" and none of them is a USB controller — a plain
        // substring search would call every one of them removable.
        CHECK(!hasUsbTransport("/sys/devices/pci0000:00/usbekistan/block/sda/sda1"));
        CHECK(!hasUsbTransport("/sys/devices/pci0000:00/usbcore/block/sda/sda1"));
        CHECK(!hasUsbTransport("/sys/devices/pci0000:00/xusb1/block/sda/sda1"));
        CHECK(!hasUsbTransport("/sys/devices/pci0000:00/usb/block/sda/sda1"));
        CHECK(!hasUsbTransport("/sys/devices/pci0000:00/usb2x/block/sda/sda1"));

        // Multi-digit controllers are ordinary on a machine with many buses.
        CHECK(hasUsbTransport("/sys/devices/pci0000:00/usb12/12-4/block/sdc/sdc1"));
        // A leading component with no slash before it still counts.
        CHECK(hasUsbTransport("usb1/1-1/block/sdb/sdb1"));

        CHECK(!hasUsbTransport(""));
        CHECK(!hasUsbTransport("/"));
    }

    // --- removability: two signals, neither sufficient alone ---

    {
        // The kernel's own flag carries an internal card reader.
        CHECK(isRemovable(kSataDisk, "1"));
        CHECK(!isRemovable(kSataDisk, "0"));
        // A USB disk that reports removable=0 is still our business.
        CHECK(isRemovable(kUsbStick, "0"));
        CHECK(isRemovable(kUsbStick, "1"));

        // An unreadable flag is not a yes. Nothing may be assumed removable
        // by default — that is the direction that ends in ejecting a disk
        // the app was never meant to touch.
        CHECK(!isRemovable(kSataDisk, ""));
        CHECK(isRemovable(kUsbStick, ""));

        // Only an exact "1" counts; near-misses are not truthy.
        CHECK(!isRemovable(kSataDisk, "10"));
        CHECK(!isRemovable(kSataDisk, " 1"));
        CHECK(!isRemovable(kSataDisk, "true"));
    }

    // --- which node gets powered off ---

    {
        CHECK_EQ(diskNodeFromSysfs("/sys/devices/pci0000:00/usb2/2-1/block/sdb", true),
                 std::string("/dev/sdb"));
        CHECK_EQ(diskNodeFromSysfs("/sys/devices/pci0000:00/block/nvme0n1", true),
                 std::string("/dev/nvme0n1"));

        // The parent of a WHOLE disk is its transport directory, not another
        // block device: "/dev/host6" is not a thing to power off.
        CHECK_EQ(diskNodeFromSysfs("/sys/devices/pci0000:00/usb2/2-1/host6", false),
                 std::string());

        CHECK_EQ(diskNodeFromSysfs("", true), std::string());
        CHECK_EQ(diskNodeFromSysfs("/sys/devices/block/", true), std::string());
        CHECK_EQ(diskNodeFromSysfs("sdb", true), std::string());
    }

    // --- recognising the pedal itself ---
    //
    // Values captured from the real device: lsusb reports 0582:0251, and
    // udev puts them on the PARTITION as well as the disk, which is what
    // makes this usable from a block `add` event.
    {
        CHECK(isPedalDevice("0582", "0251"));

        // Both halves must match. Roland makes more than one thing, and a
        // vendor-only rule would try to mount a musician's other gear.
        CHECK(!isPedalDevice("0582", "0000"));
        CHECK(!isPedalDevice("0000", "0251"));
        CHECK(!isPedalDevice("", ""));
        CHECK(!isPedalDevice("0582", ""));

        // Not a prefix or substring match: "05820" is a different number.
        CHECK(!isPedalDevice("05820", "0251"));
        CHECK(!isPedalDevice("058", "0251"));
        CHECK(!isPedalDevice("0582", "02510"));

        // The trap this rule exists to escape. The volume LABEL is
        // "BOSS RC-5", but udev hands it over as ID_FS_LABEL=BOSS_RC-5 —
        // space silently turned into an underscore — while mountinfo writes
        // the same space as \040 and ID_FS_LABEL_ENC as \x20. A label
        // comparison matches none of those and fails without a sound, which
        // is how the first version of the auto-mount arm never once fired on
        // real hardware. Renaming the card would break it a second time. The
        // USB identity has neither failure mode, and these two lines stand
        // as the reminder.
        CHECK(std::string("BOSS_RC-5") != std::string("BOSS RC-5"));
        CHECK(isPedalDevice("0582", "0251")); // unaffected by any of that
    }

    // --- the commands themselves, verbatim ---

    {
        CHECK_EQ(joined(mountArgs("/dev/sdb1")),
                 std::string("udisksctl mount -b /dev/sdb1"));
        CHECK_EQ(joined(unmountArgs("/dev/sdb1")),
                 std::string("udisksctl unmount -b /dev/sdb1"));
        CHECK_EQ(joined(powerOffArgs("/dev/sdb")),
                 std::string("udisksctl power-off -b /dev/sdb"));

        // --force belongs to the ghost path and only to it. If it ever
        // migrated into the ordinary unmount, a live card would be torn off
        // mid-write and nothing in the build would notice.
        CHECK_EQ(joined(forceUnmountArgs("/dev/sdb1")),
                 std::string("udisksctl unmount -b /dev/sdb1 --force"));
        CHECK_EQ(unmountArgs("/dev/sdb1").size(), static_cast<std::size_t>(4));
        CHECK_EQ(forceUnmountArgs("/dev/sdb1").size(), static_cast<std::size_t>(5));

        // The device must sit immediately after -b in every one of them: a
        // detached flag turns the command into "act on whatever udisks2
        // picks", which is not a thing we may hand a musician's card.
        for (const std::vector<std::string>& args :
             { mountArgs("/dev/x1"), unmountArgs("/dev/x1"), forceUnmountArgs("/dev/x1"),
               powerOffArgs("/dev/x1") }) {
            CHECK_EQ(args.front(), std::string("udisksctl"));
            CHECK_EQ(args[2], std::string("-b"));
            CHECK_EQ(args[3], std::string("/dev/x1"));
        }
    }

    return testkit::summary("linux device rules");
}
