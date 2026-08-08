// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Windows backend — phase 1 of issue #5: device truth.
//
// The same three duties as the macOS backend, by Windows means:
//
// - verdict() — what backs the volume RIGHT NOW. A drive-letter-rooted
//   removable volume gets the uncached-read probe (FILE_FLAG_NO_BUFFERING —
//   the flag forbids serving the read from cache, so only a live medium can
//   answer), on a sacrificial thread with a stuck-timeout so a wedged or
//   yanked stick reads as gone instead of freezing the worker. A plain
//   directory (synthetic test volume, --volume pin) or a non-removable
//   drive is `untracked` — trusted, exactly the policy the worker already
//   has for paths with no device story.
// - push notifications — a hidden top-level window on its own thread
//   receives the WM_DEVICECHANGE broadcasts (message-only windows do NOT —
//   a documented Win32 trap): any volume arrival pokes onDiskAppeared, a
//   removal whose unit mask covers the tracked drive letter fires
//   onDeviceLost within milliseconds, not at the next poll.
// - eject — the standard removable-media release: lock the volume (briefly
//   retried — a lock is refused while anything holds a handle), dismount,
//   allow removal, eject the medium, over DeviceIoControl. Disconnect
//   cannot walk the pedal out of STORAGE without it. Every failure carries
//   the Win32 error text into the connection banner.
//
// No auto-mount arm: Windows assigns drive letters itself, so
// onAutoMountResult never fires here.

#include "DeviceWatcher.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dbt.h>
#include <winioctl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <mutex>
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

    // An I/O probe outstanding longer than this means the volume is wedged
    // or the medium is gone mid-read: the device story is over even though
    // nothing ever returned an error. (Same policy as the macOS backend.)
    constexpr auto kProbeStuckAfter = std::chrono::seconds(2);

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

    // The uppercase drive letter of a path, or 0 when it has none.
    char driveLetterOf(const std::filesystem::path& path)
    {
        const std::string root = path.root_name().string(); // "X:" or ""
        if (root.size() != 2 || root[1] != ':'
            || std::isalpha(static_cast<unsigned char>(root[0])) == 0)
            return 0;
        return static_cast<char>(std::toupper(static_cast<unsigned char>(root[0])));
    }

    // "\\.\X:" for the raw-volume handle eject needs.
    std::string devicePathFor(char letter)
    {
        return std::string("\\\\.\\") + letter + ":";
    }

    // Read a few bytes PAST the cache. FILE_FLAG_NO_BUFFERING demands
    // sector-aligned buffer and length; 4096 satisfies every real sector
    // size. Success requires the actual medium to answer — cached fiction
    // cannot pass this.
    bool uncachedReadWorks(const std::string& path)
    {
        const HANDLE file =
            ::CreateFileA(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        alignas(4096) char buffer[4096];
        DWORD got = 0;
        const bool ok = ::ReadFile(file, buffer, sizeof(buffer), &got, nullptr) != FALSE;
        ::CloseHandle(file);
        return ok && got > 0;
    }

    // The uncached-read truth for one volume. Shared with the sacrificial
    // probe threads so a stuck thread can never dangle into freed state;
    // the threads are detached by design — a thread stuck in device wait
    // cannot be joined, and abandoning it is the price of staying
    // responsive. (Structure mirrors the macOS backend.)
    struct IoProbe {
        std::mutex mutex;
        int generation = 0; // stale probe threads may not report against a newer probe
        bool inFlight = false;
        std::chrono::steady_clock::time_point startedAt;
        bool haveResult = false;
        bool lastOk = false;
        std::string file;
    };

    void launchProbe(const std::shared_ptr<IoProbe>& probe, std::string file)
    {
        int generation = 0;
        {
            const std::lock_guard<std::mutex> lock(probe->mutex);
            generation = ++probe->generation;
            probe->inFlight = true;
            probe->startedAt = std::chrono::steady_clock::now();
            probe->file = file;
        }
        std::thread([probe, generation, target = std::move(file)] {
            const bool ok = uncachedReadWorks(target);
            const std::lock_guard<std::mutex> lock(probe->mutex);
            if (probe->generation != generation)
                return; // a newer probe owns the state now
            probe->inFlight = false;
            probe->haveResult = true;
            probe->lastOk = ok;
        }).detach();
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

//==============================================================================
// Impl: the probe state plus the notification window's thread.
//==============================================================================
struct DeviceWatcher::Impl {
    explicit Impl(DeviceWatcher& o) : owner(o)
    {
        std::promise<HWND> ready;
        std::future<HWND> readyFuture = ready.get_future();
        windowThread = std::thread([this, &ready] { runWindowLoop(std::move(ready)); });
        hwnd = readyFuture.get(); // null when window creation failed: no push, poll still works
    }

    ~Impl()
    {
        if (hwnd != nullptr)
            ::PostMessageA(hwnd, WM_CLOSE, 0, 0);
        if (windowThread.joinable())
            windowThread.join();
    }

    DeviceWatcher& owner;
    std::mutex mutex;
    char trackedLetter = 0; // drive behind the last live verdict; guarded by mutex
    std::shared_ptr<IoProbe> ioProbe = std::make_shared<IoProbe>();

    HWND hwnd = nullptr;
    std::thread windowThread;

    void diskArrived()
    {
        std::function<void()> cb;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            cb = owner.onDiskAppeared;
        }
        if (cb)
            cb();
    }

    void volumesRemoved(DWORD unitMask)
    {
        std::function<void()> cb;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (trackedLetter == 0 || (unitMask & (1u << (trackedLetter - 'A'))) == 0)
                return;
            // One shot per incarnation: the next live verdict re-arms. A
            // stale removal after replug must not ghost the fresh pedal.
            trackedLetter = 0;
            cb = owner.onDeviceLost;
        }
        if (cb)
            cb();
    }

    static LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTA*>(lParam);
            ::SetWindowLongPtrA(window, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return ::DefWindowProcA(window, message, wParam, lParam);
        }
        auto* impl = reinterpret_cast<Impl*>(::GetWindowLongPtrA(window, GWLP_USERDATA));
        if (impl != nullptr && message == WM_DEVICECHANGE) {
            const auto* header = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
            const bool isVolume = header != nullptr && header->dbch_devicetype == DBT_DEVTYP_VOLUME;
            if (wParam == DBT_DEVICEARRIVAL && isVolume)
                impl->diskArrived();
            else if (wParam == DBT_DEVICEREMOVECOMPLETE && isVolume)
                impl->volumesRemoved(reinterpret_cast<DEV_BROADCAST_VOLUME*>(lParam)->dbcv_unitmask);
            return TRUE;
        }
        if (message == WM_DESTROY) {
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcA(window, message, wParam, lParam);
    }

    void runWindowLoop(std::promise<HWND> ready)
    {
        // A hidden TOP-LEVEL window: WM_DEVICECHANGE volume broadcasts are
        // not delivered to message-only (HWND_MESSAGE) windows.
        WNDCLASSA windowClass {};
        windowClass.lpfnWndProc = wndProc;
        windowClass.hInstance = ::GetModuleHandleA(nullptr);
        windowClass.lpszClassName = "LooperCatDeviceWatcher";
        ::RegisterClassA(&windowClass); // already-registered is fine — same proc

        const HWND window =
            ::CreateWindowExA(0, windowClass.lpszClassName, "", 0, 0, 0, 0, 0, nullptr, nullptr,
                              windowClass.hInstance, this);
        ready.set_value(window);
        if (window == nullptr)
            return;

        MSG message;
        while (::GetMessageA(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageA(&message);
        }
    }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>(*this)) {}
DeviceWatcher::~DeviceWatcher() = default;

lifecycle::Backing DeviceWatcher::verdict(const std::filesystem::path& volumePath,
                                          const std::filesystem::path& probeFile)
{
    const char letter = driveLetterOf(volumePath);
    // No drive letter: a relative --volume pin or an exotic mount — no
    // device to track, trusted as-is.
    if (letter == 0)
        return lifecycle::Backing::untracked;

    // A path deeper than the drive root is a plain directory ON some drive
    // (synthetic test volumes), not a medium of its own: untracked.
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(volumePath, ec);
    if (ec || canonical != canonical.root_path())
        return lifecycle::Backing::untracked;

    const std::string root = std::string(1, letter) + ":\\";
    const UINT driveType = ::GetDriveTypeA(root.c_str());
    // The caller just saw pedal content here; the root vanishing now means
    // the medium is being torn away under us — the device story is over.
    if (driveType == DRIVE_NO_ROOT_DIR)
        return lifecycle::Backing::gone;
    // Only removable media get the device story — the pedal is one, and a
    // fixed disk can never be it. Anything else is trusted as-is.
    if (driveType != DRIVE_REMOVABLE)
        return lifecycle::Backing::untracked;

    {
        // Arm the removal push for this drive; a fresh live verdict re-arms
        // after the one-shot fire.
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->trackedLetter = letter;
    }

    // The drive letter existing is not the truth — a yanked medium can keep
    // its letter registered for a while. The final word is the uncached-read
    // probe. Its result is one scan late by design: this call never waits on
    // volume I/O. (State machine mirrors the macOS backend.)
    const std::string target = probeFile.string();
    bool gone = false;
    bool launch = false;
    {
        const std::lock_guard<std::mutex> lock(impl_->ioProbe->mutex);
        IoProbe& probe = *impl_->ioProbe;
        const bool sameFile = probe.file == target;
        if (probe.inFlight) {
            const bool stuck =
                std::chrono::steady_clock::now() - probe.startedAt > kProbeStuckAfter;
            if (stuck) {
                // Wedged on this volume = gone; stuck on a PREVIOUS volume =
                // abandon that thread and start probing the new one.
                gone = sameFile;
                launch = !sameFile;
            } else {
                gone = sameFile && probe.haveResult && !probe.lastOk;
            }
        } else {
            gone = sameFile && probe.haveResult && !probe.lastOk;
            launch = true; // keep the verdict fresh (and recover if media returns)
        }
    }
    if (launch)
        launchProbe(impl_->ioProbe, target);
    return gone ? lifecycle::Backing::gone : lifecycle::Backing::live;
}

void DeviceWatcher::eject(const std::filesystem::path& volumePath,
                          std::function<void(bool unmounted, std::string message)> done)
{
    const char letter = driveLetterOf(volumePath);
    if (letter == 0) {
        done(false, "not a drive-letter volume: " + volumePath.string());
        return;
    }
    // Short-lived worker: the lock retries block, and the caller (the pedal
    // worker thread) must stay responsive. `done` fires from this thread —
    // the owner already hops to the message thread.
    std::thread([devicePath = devicePathFor(letter), done = std::move(done)] {
        const std::string error = ejectBlocking(devicePath);
        done(error.empty(), error);
    }).detach();
}

void DeviceWatcher::forceUnmount(const std::filesystem::path& volumePath,
                                 std::function<void(bool ok, std::string message)> done)
{
    // Ghost cleanup: nothing can flush to a medium that is already gone, so
    // the same release protocol is safe — the dismount drops the stale
    // filesystem state, and the eject IOCTLs are no-ops on a gone medium.
    eject(volumePath, std::move(done));
}

} // namespace loopercat::app
