// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

//==============================================================================
// loopercat::linuxrules — the decisions the Linux DeviceWatcher makes, apart
// from the syscalls that feed them.
//
// The backend's judgements are small but consequential: whether a block
// device is removable at all (get it wrong and either the pedal is ignored
// or an internal disk gets treated as one), which disk to power off, and
// exactly which arguments go to udisksctl. Each of those is a pure function
// of evidence someone else gathered — so they live here, where a test can
// hand them the evidence directly instead of needing a real /sys and a real
// device on the machine running the suite.
//==============================================================================
namespace loopercat::linuxrules {

// True when a resolved sysfs path runs through a USB controller. Real paths
// look like ".../0000:00:14.0/usb2/2-1/2-1:1.0/host6/.../block/sdb/sdb1",
// so the marker is a whole path COMPONENT of the form usb<digits>. A plain
// substring search for "usb" would also fire on a vendor or model directory
// that merely contains those three letters, which is how an internal disk
// would quietly acquire a device story it must not have.
inline bool hasUsbTransport(const std::string& sysfsPath)
{
    size_t start = 0;
    while (start <= sysfsPath.size()) {
        const size_t end = sysfsPath.find('/', start);
        const std::string component =
            sysfsPath.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (component.size() > 3 && component.compare(0, 3, "usb") == 0) {
            bool allDigits = true;
            for (size_t i = 3; i < component.size(); ++i)
                if (component[i] < '0' || component[i] > '9')
                    allDigits = false;
            if (allDigits)
                return true;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

// Only removable media carry a device story — the pedal is one, an internal
// disk can never be it. Two independent signals, because neither alone
// covers the field: the parent disk's `removable` flag is what the kernel
// advertises for card readers and most sticks, while the USB transport
// catches the USB disks that report removable=0.
//
// `removableFlag` is the raw first line of <disk>/removable, empty when the
// file could not be read at all.
inline bool isRemovable(const std::string& sysfsPath, const std::string& removableFlag)
{
    return hasUsbTransport(sysfsPath) || removableFlag == "1";
}

// "/dev/sdb" from the resolved sysfs directory of a whole disk. `isDisk`
// says whether that directory really is a block device (it carries a `dev`
// file) — the parent of a whole disk is its transport directory, not
// another block device, and powering off "/dev/host6" would be nonsense.
inline std::string diskNodeFromSysfs(const std::string& sysfsParentPath, bool isDisk)
{
    if (!isDisk || sysfsParentPath.empty())
        return {};
    const size_t slash = sysfsParentPath.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= sysfsParentPath.size())
        return {};
    return "/dev/" + sysfsParentPath.substr(slash + 1);
}

//==============================================================================
// udisksctl argument vectors.
//
// Spelled out here, and asserted verbatim by the suite, because these are
// the commands that unmount and power off a musician's card. A flag that
// silently moves, drops or attaches itself to the wrong device is the kind
// of mistake that compiles perfectly and only shows up on hardware.
//==============================================================================
inline std::vector<std::string> mountArgs(const std::string& deviceNode)
{
    return { "udisksctl", "mount", "-b", deviceNode };
}

inline std::vector<std::string> unmountArgs(const std::string& deviceNode)
{
    return { "udisksctl", "unmount", "-b", deviceNode };
}

// Ghost cleanup only: nothing can flush to a medium that is already gone,
// so forcing is safe there by construction — and nowhere else.
inline std::vector<std::string> forceUnmountArgs(const std::string& deviceNode)
{
    return { "udisksctl", "unmount", "-b", deviceNode, "--force" };
}

inline std::vector<std::string> powerOffArgs(const std::string& diskNode)
{
    return { "udisksctl", "power-off", "-b", diskNode };
}

} // namespace loopercat::linuxrules
