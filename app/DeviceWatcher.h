// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/Lifecycle.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

//==============================================================================
// loopercat::DeviceWatcher — the macOS device-truth source behind the
// lifecycle machine (issue #17), and the eject/cleanup arm.
//
// verdict() answers what backs a volume path RIGHT NOW: a live external
// device, a dead one (the mount serves page cache — a ghost), or no device
// at all (a plain directory: synthetic test volumes, --volume pins). Truth
// comes from the IORegistry, not from the mount table — the 2026-07-22
// incident showed diskarbitrationd's view can lag reality by hours.
//
// Detach notification is push: the first live verdict for a device registers
// an IOKit interest notification, and its termination fires onDeviceLost
// (arbitrary thread) so the worker can react within milliseconds, not at the
// next poll. onDiskAppeared pokes on any new mountable disk — the fast path
// for reconnect.
//
// eject() runs the DiskArbitration unmount + whole-disk eject protocol;
// forceUnmount() clears a ghost mount (nothing can flush to a dead device,
// so force is safe there by construction).
//==============================================================================
namespace loopercat::app {

class DeviceWatcher {
public:
    DeviceWatcher();
    ~DeviceWatcher();
    DeviceWatcher(const DeviceWatcher&) = delete;
    DeviceWatcher& operator=(const DeviceWatcher&) = delete;

    // What backs `volumePath` right now. `probeFile` is a small file on the
    // volume whose UNCACHED readability is the final word: the mount table,
    // diskarbitrationd, and even the IORegistry all kept claiming a detached
    // pedal was present (2026-07-22/23 incidents — the RC-5 leaving STORAGE
    // keeps its whole USB stack registered and just pulls the medium), but
    // only a real device can serve bytes past the page cache. The probe runs
    // on a sacrificial thread with a timeout, so a wedged filesystem reads as
    // gone instead of freezing the caller; this method itself never blocks.
    lifecycle::Backing verdict(const std::filesystem::path& volumePath,
                               const std::filesystem::path& probeFile);

    // Termination of the device behind the most recent live verdict.
    // Arbitrary thread. Set before the first verdict() call.
    std::function<void()> onDeviceLost;

    // A mountable disk appeared — worth rescanning immediately. Arbitrary thread.
    std::function<void()> onDiskAppeared;

    // The watcher mounted (or failed to mount) a pedal-labeled volume that
    // appeared and stayed unmounted through the grace period — the 2026-07-22
    // aftermath showed the OS automount can simply not happen. Arbitrary thread.
    std::function<void(bool ok, std::string message)> onAutoMountResult;

    // Unmount the volume, then eject its whole disk. `done(unmounted, message)`
    // fires on an arbitrary thread: `unmounted` is the safety-relevant fact;
    // `message` carries the unmount dissent or a post-unmount eject warning.
    void eject(const std::filesystem::path& volumePath,
               std::function<void(bool unmounted, std::string message)> done);

    // Force-unmount a ghost. `done(ok, message)` on an arbitrary thread.
    void forceUnmount(const std::filesystem::path& volumePath,
                      std::function<void(bool ok, std::string message)> done);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace loopercat::app
