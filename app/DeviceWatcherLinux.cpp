// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The Linux backend — the third DeviceWatcher, the same three duties by
// Linux means.
//
// - verdict() — what backs the volume RIGHT NOW. The mount table names the
//   block device behind a mount point; sysfs says whether that device still
//   exists; and the uncached-read probe (O_DIRECT, which the kernel may not
//   serve from page cache) is the final word, on a sacrificial thread with a
//   stuck-timeout so a wedged or yanked stick reads as gone instead of
//   freezing the worker. A path that is not a mount point at all (synthetic
//   test volume, --volume pin) or one backed by a virtual filesystem or a
//   non-removable disk is `untracked` — trusted, the same policy the other
//   two backends already apply to paths with no device story.
// - push notifications — a libudev netlink monitor on the `block` subsystem,
//   on its own thread: any block `add` pokes onDiskAppeared, and a `remove`
//   whose device number matches the tracked volume fires onDeviceLost within
//   milliseconds, not at the next poll.
// - eject — udisks2 over `udisksctl`. A desktop Linux mount belongs to
//   udisks2, not to us: umount(2) on someone else's mount is a privileged
//   operation, while udisksctl asks the same daemon that mounted it and
//   passes through PolicyKit as the logged-in user. Unmount first (the
//   safety-relevant fact), then power-off the whole disk, the equivalent of
//   the macOS whole-disk eject. Its stderr becomes the banner text.
//
// The auto-mount arm matters MORE here than on macOS: Linux desktops mount
// removable media through udisks2 only when a file manager or a session
// agent asks it to, and a plain X session, a tiling WM or a minimal install
// simply has nobody to ask. A pedal-labeled partition that appears and stays
// unmounted through the grace period gets one `udisksctl mount`.

#include "DeviceWatcher.h"

#include "MountTable.h"

#include <libudev.h>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

namespace loopercat::app {

namespace {

    // The FAT label the pedal presents in USB storage mode. Mirrors the
    // macOS backend's constant — the auto-mount arm is the only consumer on
    // either platform, so the two stay side by side rather than moving to a
    // shared header for one use each.
    constexpr const char* kPedalVolumeLabel = "BOSS RC-5";

    // An I/O probe outstanding longer than this means the volume is wedged
    // or the medium is gone mid-read: the device story is over even though
    // nothing ever returned an error. (Same policy as the other backends.)
    constexpr auto kProbeStuckAfter = std::chrono::seconds(2);

    // How long the session's own automounter gets before we do it ourselves.
    constexpr auto kAutoMountGrace = std::chrono::seconds(5);

    // O_DIRECT demands the buffer, the offset and the length be aligned to
    // the device's logical block size; 4096 satisfies every real sector size.
    constexpr size_t kDirectAlign = 4096;

    //==========================================================================
    // /proc/self/mountinfo — parsing lives in MountTable.h, under test
    //==========================================================================

    std::vector<mounttable::Entry> currentMounts()
    {
        std::ifstream file("/proc/self/mountinfo");
        return mounttable::parse(file);
    }

    dev_t deviceOf(const mounttable::Entry& entry)
    {
        return ::makedev(entry.deviceMajor, entry.deviceMinor);
    }

    // The mount whose mount point is exactly this path, canonicalised the
    // same way the caller's path was.
    std::optional<mounttable::Entry> mountAt(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        if (ec)
            return std::nullopt;
        return mounttable::findByMountPoint(currentMounts(), canonical.string());
    }

    bool deviceIsMounted(dev_t device)
    {
        return mounttable::isDeviceMounted(currentMounts(), ::major(device), ::minor(device));
    }

    //==========================================================================
    // sysfs
    //==========================================================================

    std::string sysfsBlockDir(dev_t device)
    {
        return "/sys/dev/block/" + std::to_string(::major(device)) + ":"
             + std::to_string(::minor(device));
    }

    bool blockDeviceExists(dev_t device)
    {
        struct ::stat info { };
        return ::stat(sysfsBlockDir(device).c_str(), &info) == 0;
    }

    std::string readSysfsLine(const std::string& path)
    {
        std::ifstream file(path);
        std::string line;
        std::getline(file, line);
        return line;
    }

    // Only removable media carry a device story — the pedal is one, and an
    // internal disk can never be it. Two independent signals, because
    // neither alone covers the field: the parent disk's `removable` flag is
    // what the kernel advertises for card readers and most sticks, while a
    // USB transport in the sysfs path catches the USB disks that report
    // removable=0.
    bool deviceIsRemovable(dev_t device)
    {
        char resolved[PATH_MAX];
        const char* real = ::realpath(sysfsBlockDir(device).c_str(), resolved);
        if (real != nullptr && std::string(real).find("/usb") != std::string::npos)
            return true;
        // A partition's sysfs node sits inside its disk's, so the parent
        // directory is the whole-disk node that carries `removable`.
        return readSysfsLine(sysfsBlockDir(device) + "/../removable") == "1";
    }

    // "/dev/sdb" for a partition on it — what a whole-disk power-off needs.
    // The partition's sysfs parent directory is the disk, and its name is
    // the kernel name of the block device.
    std::string parentDiskNode(dev_t device)
    {
        char resolved[PATH_MAX];
        const char* real = ::realpath((sysfsBlockDir(device) + "/..").c_str(), resolved);
        if (real == nullptr)
            return {};
        const std::string path(real);
        const size_t slash = path.find_last_of('/');
        if (slash == std::string::npos || slash + 1 >= path.size())
            return {};
        // The parent of a WHOLE disk's node is the transport directory, not
        // another block device; only accept a parent that is itself one.
        if (!std::ifstream(path + "/dev").good())
            return {};
        return "/dev/" + path.substr(slash + 1);
    }

    //==========================================================================
    // The uncached read
    //==========================================================================

    enum class ProbeOutcome {
        answered,    // the medium served bytes past the cache
        refused,     // the read failed — the medium is gone or wedged
        unsupported, // this filesystem cannot do O_DIRECT at all
    };

    ProbeOutcome uncachedRead(const std::string& path)
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECT);
        if (fd < 0) {
            // EINVAL from open means the filesystem refuses direct I/O, not
            // that the device left; anything else is a real read failure.
            return errno == EINVAL ? ProbeOutcome::unsupported : ProbeOutcome::refused;
        }
        void* buffer = nullptr;
        if (::posix_memalign(&buffer, kDirectAlign, kDirectAlign) != 0) {
            ::close(fd);
            return ProbeOutcome::unsupported;
        }
        const ssize_t got = ::read(fd, buffer, kDirectAlign);
        std::free(buffer);
        ::close(fd);
        // A probe file shorter than one block legitimately reads short, and
        // even a zero-length answer proves the medium responded; only an
        // error means it did not.
        return got >= 0 ? ProbeOutcome::answered : ProbeOutcome::refused;
    }

    // Shared with the sacrificial probe threads so a stuck thread can never
    // dangle into freed state; the threads are detached by design — a thread
    // stuck in device wait cannot be joined, and abandoning it is the price
    // of staying responsive. (Structure mirrors the other two backends.)
    struct IoProbe {
        std::mutex mutex;
        int generation = 0; // stale probe threads may not report against a newer probe
        bool inFlight = false;
        std::chrono::steady_clock::time_point startedAt;
        bool haveResult = false;
        ProbeOutcome lastOutcome = ProbeOutcome::answered;
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
            const ProbeOutcome outcome = uncachedRead(target);
            const std::lock_guard<std::mutex> lock(probe->mutex);
            if (probe->generation != generation)
                return; // a newer probe owns the state now
            probe->inFlight = false;
            probe->haveResult = true;
            probe->lastOutcome = outcome;
        }).detach();
    }

    //==========================================================================
    // udisksctl
    //==========================================================================

    struct CommandResult {
        bool ok = false;
        std::string output;
    };

    CommandResult run(const std::vector<std::string>& argv)
    {
        int pipeEnds[2] = { -1, -1 };
        if (::pipe(pipeEnds) != 0)
            return { false, "could not create a pipe for " + argv.front() };

        ::posix_spawn_file_actions_t actions;
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(&actions, pipeEnds[1], STDERR_FILENO);
        ::posix_spawn_file_actions_addclose(&actions, pipeEnds[0]);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const std::string& arg : argv)
            args.push_back(const_cast<char*>(arg.c_str()));
        args.push_back(nullptr);

        ::pid_t child = 0;
        const int spawned =
            ::posix_spawnp(&child, argv.front().c_str(), &actions, nullptr, args.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions);
        ::close(pipeEnds[1]);
        if (spawned != 0) {
            ::close(pipeEnds[0]);
            return { false, argv.front() + " could not be run (" + std::strerror(spawned) + ")" };
        }

        std::string output;
        char chunk[512];
        ssize_t got = 0;
        while ((got = ::read(pipeEnds[0], chunk, sizeof(chunk))) > 0)
            output.append(chunk, static_cast<size_t>(got));
        ::close(pipeEnds[0]);

        int status = 0;
        ::waitpid(child, &status, 0);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r'
                                   || output.back() == ' ' || output.back() == '.'))
            output.pop_back();
        return { WIFEXITED(status) && WEXITSTATUS(status) == 0, output };
    }

} // namespace

//==============================================================================
// Impl: the probe state plus the udev monitor's thread.
//==============================================================================
struct DeviceWatcher::Impl {
    explicit Impl(DeviceWatcher& o) : owner(o)
    {
        // An eventfd rides alongside the monitor fd in poll() so the
        // destructor can wake the loop instead of waiting for the next
        // device event, which may never come.
        wakeFd = ::eventfd(0, EFD_CLOEXEC);
        monitorThread = std::thread([this] { runMonitorLoop(); });
    }

    ~Impl()
    {
        alive->store(false);
        stopping.store(true);
        if (wakeFd >= 0) {
            const uint64_t one = 1;
            const ssize_t written = ::write(wakeFd, &one, sizeof(one));
            (void) written;
        }
        if (monitorThread.joinable())
            monitorThread.join();
        if (wakeFd >= 0)
            ::close(wakeFd);
    }

    DeviceWatcher& owner;
    std::mutex mutex;
    dev_t trackedDevice = 0; // device behind the last live verdict; guarded by mutex
    std::set<std::string> mountAttempted;
    std::shared_ptr<IoProbe> ioProbe = std::make_shared<IoProbe>();
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> stopping { false };
    int wakeFd = -1;
    std::thread monitorThread;

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

    void deviceRemoved(dev_t device)
    {
        std::function<void()> cb;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (trackedDevice == 0 || trackedDevice != device)
                return;
            // One shot per incarnation: the next live verdict re-arms. A
            // stale removal after replug must not ghost the fresh pedal.
            trackedDevice = 0;
            cb = owner.onDeviceLost;
        }
        if (cb)
            cb();
    }

    // A pedal-labeled partition just appeared. Give the session's own
    // automounter the grace period, then mount it ourselves if nobody did.
    void considerAutoMount(const std::string& devNode, dev_t device)
    {
        {
            // A fresh attach earns a fresh (single) auto-mount attempt.
            const std::lock_guard<std::mutex> lock(mutex);
            if (!mountAttempted.insert(devNode).second)
                return;
        }
        std::thread([this, devNode, device, guard = alive] {
            std::this_thread::sleep_for(kAutoMountGrace);
            if (!guard->load())
                return;
            // Mounted after all = automount won; the device gone = nothing
            // to mount. Either way there is nothing for us to do.
            if (deviceIsMounted(device) || !blockDeviceExists(device))
                return;
            const CommandResult result = run({ "udisksctl", "mount", "-b", devNode });
            if (!guard->load())
                return;
            std::function<void(bool, std::string)> cb;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                cb = owner.onAutoMountResult;
            }
            if (cb)
                cb(result.ok, result.ok ? std::string(kPedalVolumeLabel) : result.output);
        }).detach();
    }

    void handleEvent(udev_device* device)
    {
        const char* action = udev_device_get_action(device);
        if (action == nullptr)
            return;
        const dev_t number = udev_device_get_devnum(device);

        if (std::strcmp(action, "remove") == 0) {
            deviceRemoved(number);
            return;
        }
        if (std::strcmp(action, "add") != 0)
            return;

        const char* label = udev_device_get_property_value(device, "ID_FS_LABEL");
        const char* node = udev_device_get_devnode(device);
        if (label != nullptr && node != nullptr && std::strcmp(label, kPedalVolumeLabel) == 0)
            considerAutoMount(node, number);
        diskArrived();
    }

    void runMonitorLoop()
    {
        udev* context = udev_new();
        if (context == nullptr)
            return; // no push; verdict() polling still works

        udev_monitor* monitor = udev_monitor_new_from_netlink(context, "udev");
        if (monitor == nullptr) {
            udev_unref(context);
            return;
        }
        udev_monitor_filter_add_match_subsystem_devtype(monitor, "block", nullptr);
        udev_monitor_enable_receiving(monitor);

        struct pollfd fds[2];
        fds[0].fd = udev_monitor_get_fd(monitor);
        fds[0].events = POLLIN;
        fds[1].fd = wakeFd;
        fds[1].events = POLLIN;

        while (!stopping.load()) {
            const int ready = ::poll(fds, wakeFd >= 0 ? 2 : 1, -1);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (stopping.load())
                break;
            if ((fds[0].revents & POLLIN) == 0)
                continue;
            udev_device* device = udev_monitor_receive_device(monitor);
            if (device == nullptr)
                continue;
            handleEvent(device);
            udev_device_unref(device);
        }

        udev_monitor_unref(monitor);
        udev_unref(context);
    }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>(*this)) {}
DeviceWatcher::~DeviceWatcher() = default;

lifecycle::Backing DeviceWatcher::verdict(const std::filesystem::path& volumePath,
                                          const std::filesystem::path& probeFile)
{
    // Not a mount point of its own: a plain directory (synthetic test
    // volume, --volume pin) has no device story to tell.
    const std::optional<mounttable::Entry> mount = mountAt(volumePath);
    if (!mount.has_value())
        return lifecycle::Backing::untracked;

    // Major 0 is a virtual filesystem (tmpfs, overlay) — no block device
    // exists for it, so its absence from sysfs must not read as `gone`.
    if (mount->deviceMajor == 0)
        return lifecycle::Backing::untracked;

    const dev_t device = deviceOf(*mount);

    // The caller just saw pedal content here; the block device vanishing now
    // means the medium is being torn away under us.
    if (!blockDeviceExists(device))
        return lifecycle::Backing::gone;

    if (!deviceIsRemovable(device))
        return lifecycle::Backing::untracked;

    {
        // Arm the removal push for this device; a fresh live verdict re-arms
        // after the one-shot fire.
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->trackedDevice = device;
    }

    // The device node existing is not the final truth — the RC-5 leaving
    // STORAGE surrenders the medium while its USB stack stays registered.
    // The uncached read settles it. Its result is one scan late by design:
    // this call never waits on volume I/O.
    const std::string target = probeFile.string();
    bool gone = false;
    bool launch = false;
    {
        const std::lock_guard<std::mutex> lock(impl_->ioProbe->mutex);
        IoProbe& probe = *impl_->ioProbe;
        const bool sameFile = probe.file == target;
        // A filesystem with no O_DIRECT support gives no verdict at all; on
        // Linux the sysfs check above is a real answer on its own, so the
        // volume stays live rather than being declared a ghost on a
        // capability gap.
        const bool refused = probe.haveResult && probe.lastOutcome == ProbeOutcome::refused;
        if (probe.inFlight) {
            const bool stuck =
                std::chrono::steady_clock::now() - probe.startedAt > kProbeStuckAfter;
            if (stuck) {
                // Wedged on this volume = gone; stuck on a PREVIOUS volume =
                // abandon that thread and start probing the new one.
                gone = sameFile;
                launch = !sameFile;
            } else {
                gone = sameFile && refused;
            }
        } else {
            gone = sameFile && refused;
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
    const std::optional<mounttable::Entry> mount = mountAt(volumePath);
    if (!mount.has_value() || mount->source.empty()) {
        done(false, "no mounted block device at " + volumePath.string());
        return;
    }

    // Short-lived worker: udisksctl blocks (PolicyKit may even prompt), and
    // the caller (the pedal worker thread) must stay responsive. `done`
    // fires from this thread — the owner already hops to the message thread.
    std::thread([node = mount->source, disk = parentDiskNode(deviceOf(*mount)),
                 done = std::move(done)] {
        const CommandResult unmounted = run({ "udisksctl", "unmount", "-b", node });
        if (!unmounted.ok) {
            done(false, unmounted.output);
            return;
        }
        // Power-off is the whole-disk eject: it is what makes the pedal
        // leave USB storage. Best-effort — the volume is already safely let
        // go by the unmount, so a refusal here is a warning, not a failure.
        if (disk.empty()) {
            done(true, "unmounted, but the disk behind " + node + " could not be identified");
            return;
        }
        const CommandResult off = run({ "udisksctl", "power-off", "-b", disk });
        done(true, off.ok ? std::string() : off.output);
    }).detach();
}

void DeviceWatcher::forceUnmount(const std::filesystem::path& volumePath,
                                 std::function<void(bool ok, std::string message)> done)
{
    const std::optional<mounttable::Entry> mount = mountAt(volumePath);
    if (!mount.has_value() || mount->source.empty()) {
        done(false, "no mounted block device at " + volumePath.string());
        return;
    }
    // Ghost cleanup: nothing can flush to a medium that is already gone, so
    // forcing is safe here by construction.
    std::thread([node = mount->source, done = std::move(done)] {
        const CommandResult result = run({ "udisksctl", "unmount", "-b", node, "--force" });
        done(result.ok, result.output);
    }).detach();
}

} // namespace loopercat::app
