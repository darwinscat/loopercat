// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Windows backend — the phase-0 honest minimum (issue #5).
//
// No device truth yet: verdict() answers `untracked`, which the worker
// treats as "trust the volume" — the same deliberate policy the synthetic
// and --volume-pinned paths use. No arrival/removal push either: the
// worker's poll discovers mounts within a cycle, and Windows assigns drive
// letters itself, so the auto-mount arm has no job here. The ghost story
// (a mount serving cached fiction for a detached device) therefore cannot
// be DETECTED on Windows yet — that is phase 1: WM_DEVICECHANGE arrival/
// removal plus an uncached FILE_FLAG_NO_BUFFERING probe behind the same
// interface.
//
// What IS real is eject: Disconnect must walk the pedal out of STORAGE on
// Windows too, and that requires releasing the volume first. The standard
// removable-media protocol: lock the volume (retried briefly — a lock is
// refused while anything holds a handle), dismount it, allow removal, eject
// the medium. Every failure carries the Win32 error text — the banner tells
// the musician what actually stood in the way.

#include "DeviceWatcher.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <winioctl.h>

#include <cctype>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace loopercat::app {

namespace {

    // A lock is refused while any handle to the volume is open; explorer
    // windows and indexers let go quickly, so a short retry ladder converts
    // most transient holds into a clean eject.
    constexpr int kLockAttempts = 5;
    constexpr auto kLockRetryDelay = std::chrono::milliseconds(400);

    std::string lastErrorText(const char* what)
    {
        const DWORD code = ::GetLastError();
        char* buffer = nullptr;
        const DWORD length = ::FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<char*>(&buffer), 0, nullptr);
        std::string text = std::string(what) + " failed (error " + std::to_string(code) + ")";
        if (length != 0 && buffer != nullptr) {
            std::string detail(buffer, length);
            while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n'
                                       || detail.back() == '.' || detail.back() == ' '))
                detail.pop_back();
            if (!detail.empty())
                text += ": " + detail;
        }
        if (buffer != nullptr)
            ::LocalFree(buffer);
        return text;
    }

    // "\\.\X:" for a drive-letter-rooted volume path; empty when the path
    // has no drive letter (fail fast — a pedal never mounts as a UNC share).
    std::string devicePathFor(const std::filesystem::path& volumePath)
    {
        const std::string root = volumePath.root_name().string(); // "X:" or ""
        if (root.size() != 2 || root[1] != ':'
            || std::isalpha(static_cast<unsigned char>(root[0])) == 0)
            return {};
        return "\\\\.\\" + root;
    }

    bool ioctl(HANDLE volume, DWORD code, void* inBuffer, DWORD inSize)
    {
        DWORD returned = 0;
        return ::DeviceIoControl(volume, code, inBuffer, inSize, nullptr, 0, &returned, nullptr)
            != FALSE;
    }

    // The whole eject protocol, blocking. Returns an empty string on
    // success, the failing step's error text otherwise.
    std::string ejectBlocking(const std::string& devicePath)
    {
        const HANDLE volume =
            ::CreateFileA(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (volume == INVALID_HANDLE_VALUE)
            return lastErrorText("opening the volume");

        std::string error;
        bool locked = false;
        for (int attempt = 0; attempt < kLockAttempts && !locked; ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(kLockRetryDelay);
            locked = ioctl(volume, FSCTL_LOCK_VOLUME, nullptr, 0);
        }
        if (!locked)
            error = lastErrorText("locking the volume (something still holds it)");
        else if (!ioctl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0))
            error = lastErrorText("dismounting the volume");
        else {
            PREVENT_MEDIA_REMOVAL removal { FALSE };
            // Best-effort: some sticks refuse the removal IOCTLs after a
            // clean dismount; the volume is already safely let go by then.
            ioctl(volume, IOCTL_STORAGE_MEDIA_REMOVAL, &removal, sizeof(removal));
            ioctl(volume, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0);
        }
        ::CloseHandle(volume);
        return error;
    }

} // namespace

struct DeviceWatcher::Impl {
    // Phase 0 holds no platform state; the pimpl keeps the header identical
    // across backends.
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>()) {}
DeviceWatcher::~DeviceWatcher() = default;

lifecycle::Backing DeviceWatcher::verdict(const std::filesystem::path&,
                                          const std::filesystem::path&)
{
    return lifecycle::Backing::untracked;
}

void DeviceWatcher::eject(const std::filesystem::path& volumePath,
                          std::function<void(bool unmounted, std::string message)> done)
{
    const std::string devicePath = devicePathFor(volumePath);
    if (devicePath.empty()) {
        done(false, "not a drive-letter volume: " + volumePath.string());
        return;
    }
    // Short-lived worker: the lock retries block, and the caller (the pedal
    // worker thread) must stay responsive. `done` fires from this thread —
    // the owner already hops to the message thread.
    std::thread([devicePath, done = std::move(done)] {
        const std::string error = ejectBlocking(devicePath);
        done(error.empty(), error);
    }).detach();
}

void DeviceWatcher::forceUnmount(const std::filesystem::path& volumePath,
                                 std::function<void(bool ok, std::string message)> done)
{
    // Unreachable while verdict() never answers `gone` (no ghosts without
    // device truth), but honest if called: the same release protocol.
    eject(volumePath, std::move(done));
}

} // namespace loopercat::app
