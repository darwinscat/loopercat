// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Linux backend's judgements, tested from what they PROMISE rather than
// from how they are written: a device is removable when the kernel says so
// or when it hangs off USB; udisksctl gets exactly the arguments a
// musician's card can survive; and a refused unmount is waited out only when
// waiting can help.
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

    // --- the commands themselves, verbatim ---

    {
        CHECK_EQ(joined(mountArgs("/dev/sdb1")),
                 std::string("udisksctl mount -b /dev/sdb1"));
        CHECK_EQ(joined(unmountArgs("/dev/sdb1")),
                 std::string("udisksctl unmount -b /dev/sdb1"));

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
             { mountArgs("/dev/x1"), unmountArgs("/dev/x1"), forceUnmountArgs("/dev/x1") }) {
            CHECK_EQ(args.front(), std::string("udisksctl"));
            CHECK_EQ(args[2], std::string("-b"));
            CHECK_EQ(args[3], std::string("/dev/x1"));
        }
    }

    // --- which unmount failures are worth waiting out ---

    {
        // The real message, as udisks2 phrased it on the machine.
        CHECK(isBusyError("Error unmounting /dev/sdb1: GDBus.Error:"
                          "org.freedesktop.UDisks2.Error.DeviceBusy: Device busy"));

        // Retrying these only delays an honest banner: they read the same
        // in four hundred milliseconds as they do now.
        CHECK(!isBusyError("Error unmounting /dev/sdb1: GDBus.Error:"
                           "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain: "
                           "Not authorized to perform operation"));
        CHECK(!isBusyError("Error looking up object for device /dev/sdb1"));
        CHECK(!isBusyError(""));
        // Success has no message at all, and must not read as busy.
        CHECK(!isBusyError("Unmounted /dev/sdb1"));
    }

    return testkit::summary("linux device rules");
}
