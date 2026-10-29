// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// macOS implementation notes. Liveness is answered by the IORegistry (does an
// IOMedia object with this BSD name still exist?), never by the mount table
// or diskarbitrationd's cached view — both kept claiming a detached pedal was
// present for hours in the 2026-07-22 incident. Termination push uses IOKit
// general-interest notifications; unmount/eject go through DiskArbitration.

#include "DeviceWatcher.h"

#include "MountProbe.h"

#include <loopercat/Error.hpp>

#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace loopercat::app {

namespace {

    // The factory volume label of the RC-5's mass-storage export (rc5cat).
    // Auto-mount keys on it because an unmounted volume cannot be content-
    // checked; a user-relabeled pedal just falls back to manual mounting.
    constexpr const char* kPedalVolumeLabel = "BOSS RC-5";

    // How long the OS automount gets before the watcher steps in.
    constexpr int64_t kAutoMountGraceSeconds = 5;

    // The uncached-read truth for ONE mount incarnation (MountProbe.h: a
    // result from an earlier mount of the same path force-unmounted three
    // live cards in a row, issue #166). Shared with the sacrificial probe
    // threads so a stuck thread can never dangle into freed state; the
    // threads are detached by design — a thread in uninterruptible disk wait
    // cannot be joined, and abandoning it is the price of staying responsive.
    // (A probe stuck at quit can still zombie the exiting process; that is
    // the OS bug's residue, not something a userland app can fully escape.)
    struct IoProbe {
        std::mutex mutex;
        int generation = 0; // stale probe threads may not report against a newer probe
        mountprobe::Incarnation mount; // what everything below is about
        std::string file;
        mountprobe::Clock::time_point firstSeenAt{}; // when this mount came under our eye
        bool inFlight = false;
        mountprobe::Clock::time_point startedAt;
        bool haveResult = false;
        mountprobe::Read read{};
        int refusals = 0; // reads in a row that did not answer, this incarnation
    };

    // Read a few bytes PAST the page cache. Success requires the actual
    // device to answer — cached fiction cannot pass this. The byte count and
    // errno come back with it: they are what lets operations.log say why a
    // card was called gone.
    mountprobe::Read uncachedRead(const std::string& path)
    {
        mountprobe::Read result;
        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            result.answer = mountprobe::Answer::refused;
            result.error = errno;
            return result;
        }
        ::fcntl(fd, F_NOCACHE, 1);
        char buffer[4096];
        errno = 0; // a read that delivers nothing without failing (EOF) must not
                   // report whatever errno happened to hold before it
        const ssize_t got = ::read(fd, buffer, sizeof(buffer));
        const int failure = errno;
        ::close(fd);
        result.answer = got > 0    ? mountprobe::Answer::served
                      : got == 0   ? mountprobe::Answer::noVerdict
                                   : mountprobe::Answer::refused;
        result.bytes = got > 0 ? got : 0;
        result.error = got < 0 ? failure : 0;
        return result;
    }

    void launchProbe(const std::shared_ptr<IoProbe>& probe, const mountprobe::Incarnation& mount,
                     std::string file)
    {
        int generation = 0;
        {
            const std::lock_guard<std::mutex> lock(probe->mutex);
            if (!mountprobe::speaksFor(probe->mount, probe->file, mount, file)) {
                // A mount this state knows nothing about: it starts with a
                // clean slate and its own clock, so nothing it inherits can
                // condemn it (issue #166).
                probe->mount = mount;
                probe->file = file;
                probe->firstSeenAt = mountprobe::Clock::now();
                probe->haveResult = false;
                probe->read = {};
                probe->refusals = 0;
            }
            generation = ++probe->generation;
            probe->inFlight = true;
            probe->startedAt = mountprobe::Clock::now();
        }
        std::thread([probe, generation, target = std::move(file)] {
            const mountprobe::Read read = uncachedRead(target);
            const std::lock_guard<std::mutex> lock(probe->mutex);
            if (probe->generation != generation)
                return; // a newer probe owns the state now
            probe->inFlight = false;
            probe->haveResult = true;
            probe->read = read;
            probe->refusals =
                read.answer == mountprobe::Answer::refused ? probe->refusals + 1 : 0;
        }).detach();
    }

    std::string cfToString(CFStringRef s)
    {
        if (s == nullptr)
            return {};
        char buf[256];
        if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8))
            return buf;
        return {};
    }

    std::string dissenterMessage(DADissenterRef dissenter)
    {
        const std::string text = cfToString(DADissenterGetStatusString(dissenter));
        if (!text.empty())
            return text;
        return "refused with status " + std::to_string(DADissenterGetStatus(dissenter));
    }

    // The statfs view of a path: which mount covers it, and what device (if
    // any) that mount comes from.
    struct MountRecord {
        bool statOk = false;
        bool isOwnMount = false;  // the path IS the mount point, not a plain dir
        std::string from;         // f_mntfromname, e.g. "/dev/disk16s1"
        std::string bsd;          // "disk16s1" when device-backed, else empty
    };

    MountRecord mountRecordFor(const std::filesystem::path& path)
    {
        MountRecord rec;
        struct statfs sfs {};
        if (statfs(path.c_str(), &sfs) != 0)
            return rec;
        rec.statOk = true;
        std::error_code ec;
        const auto canonical = std::filesystem::weakly_canonical(path, ec);
        rec.isOwnMount = !ec && std::filesystem::path(sfs.f_mntonname) == canonical;
        rec.from = sfs.f_mntfromname;
        if (rec.from.rfind("/dev/", 0) == 0)
            rec.bsd = rec.from.substr(5);
        return rec;
    }

} // namespace

struct DeviceWatcher::Impl {
    explicit Impl(DeviceWatcher& o) : owner(o) {}

    DeviceWatcher& owner;
    dispatch_queue_t queue = nullptr;
    DASessionRef session = nullptr;
    IONotificationPortRef notifyPort = nullptr;

    struct InterestCtx {
        Impl* impl = nullptr;
        std::string bsd;
    };
    struct Tracked {
        io_object_t notification = IO_OBJECT_NULL;
        InterestCtx* ctx = nullptr;
        bool dead = false;
    };

    std::mutex mutex;
    std::map<std::string, Tracked> tracked; // by BSD name; guarded by mutex
    std::string currentBsd;                 // device behind the last live verdict
    std::string watchedVolume;              // path of the last device-backed verdict
    std::set<std::string> mountAttempted;   // one auto-mount per attach; guarded by mutex
    std::shared_ptr<IoProbe> ioProbe = std::make_shared<IoProbe>();
    // Every mount and unmount DiskArbitration reports, counted. What is
    // mounted at a path after such an event may be another mount of another
    // medium, and nothing in the IORegistry would show it — so the count is
    // part of a mount's identity (MountProbe.h, issue #166).
    std::atomic<long long> mountEpoch{ 0 };
    // One journal line per ghost episode: the worker asks on every 1.5 s
    // scan, and a ghost stays one until its mount is cleared.
    std::atomic<bool> ghostTold{ false };
    // Outlives Impl in dispatch_after contexts: those fire on the queue after
    // the destructor's drain and must become no-ops, not use-after-frees.
    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>>(true);

    void deviceTerminated(const std::string& bsd)
    {
        std::function<void()> cb;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            auto it = tracked.find(bsd);
            if (it == tracked.end() || it->second.dead)
                return;
            it->second.dead = true;
            // A stale termination for a device that no longer backs the
            // pedal (fast unplug/replug cycles renumber the disk) must not
            // ghost the fresh connection.
            if (bsd == currentBsd)
                cb = owner.onDeviceLost;
        }
        if (cb)
            cb();
    }

    static void interestCallback(void* refcon, io_service_t, natural_t messageType, void*)
    {
        if (messageType != kIOMessageServiceIsTerminated)
            return;
        auto* ctx = static_cast<InterestCtx*>(refcon);
        ctx->impl->deviceTerminated(ctx->bsd);
    }

    static void diskAppearedCallback(DADiskRef disk, void* context)
    {
        auto* impl = static_cast<Impl*>(context);
        std::function<void()> cb;
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            cb = impl->owner.onDiskAppeared;
        }
        if (cb)
            cb();
        impl->considerAutoMount(disk);
    }

    // A volume path came or went: a mount happened, or an unmount did. The
    // one event that changes what answers at the path we watch is a volume
    // ARRIVING there — whatever is mounted now is a mount we have never
    // probed, whether it is another medium or this one again (issue #166).
    //
    // An unmount is deliberately not counted, and neither is any other
    // disk's news. Our own forced unmount of a ghost is an unmount of this
    // very path, and counting it would reset the probe state right after the
    // verdict that asked for it — the mount would read live again on the next
    // scan, and the ghost would oscillate while writes were allowed to it.
    // A stick mounting elsewhere would discard the state the wedged-
    // filesystem case needs to keep (2026-07-22) long enough to call a read
    // stuck.
    static void volumePathChangedCallback(DADiskRef disk, CFArrayRef, void* context)
    {
        auto* impl = static_cast<Impl*>(context);
        std::string mountedAt;
        if (CFDictionaryRef description = DADiskCopyDescription(disk)) {
            if (const auto url = static_cast<CFURLRef>(
                    CFDictionaryGetValue(description, kDADiskDescriptionVolumePathKey))) {
                CFStringRef path = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
                mountedAt = cfToString(path);
                if (path != nullptr)
                    CFRelease(path);
            }
            CFRelease(description);
        }
        if (mountedAt.empty()) // an unmount, or a path this build cannot read
            return;
        const std::lock_guard<std::mutex> lock(impl->mutex);
        if (mountedAt == impl->watchedVolume)
            impl->mountEpoch.fetch_add(1);
    }

    // Why a verdict went against the mount, once per episode. Called on the
    // thread that asked for the verdict (the pedal worker).
    void tellGhost(const std::string& line)
    {
        if (ghostTold.exchange(true))
            return;
        std::function<void(std::string)> cb;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            cb = owner.onGhostVerdict;
        }
        if (cb)
            cb(line);
    }

    // --- auto-mount: a pedal-labeled volume the OS never mounted ---

    struct AutoMountCtx {
        std::shared_ptr<std::atomic<bool>> alive;
        Impl* impl = nullptr;
        std::string bsd;
    };

    // True while the disk's description shows no volume path.
    static bool isUnmounted(CFDictionaryRef description)
    {
        return CFDictionaryGetValue(description, kDADiskDescriptionVolumePathKey) == nullptr;
    }

    void considerAutoMount(DADiskRef disk)
    {
        const char* bsdName = DADiskGetBSDName(disk);
        CFDictionaryRef description = bsdName != nullptr ? DADiskCopyDescription(disk) : nullptr;
        if (description == nullptr)
            return;
        const std::string name = cfToString(static_cast<CFStringRef>(
            CFDictionaryGetValue(description, kDADiskDescriptionVolumeNameKey)));
        const auto* internal = static_cast<CFBooleanRef>(
            CFDictionaryGetValue(description, kDADiskDescriptionDeviceInternalKey));
        const bool candidate = isUnmounted(description) && name == kPedalVolumeLabel
                            && (internal == nullptr || !CFBooleanGetValue(internal));
        CFRelease(description);
        {
            // A fresh attach earns a fresh (single) auto-mount attempt.
            const std::lock_guard<std::mutex> lock(mutex);
            mountAttempted.erase(bsdName);
        }
        if (!candidate)
            return;
        auto* ctx = new AutoMountCtx{ aliveFlag, this, bsdName };
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, kAutoMountGraceSeconds * NSEC_PER_SEC),
                         queue, ctx, &autoMountGraceFired);
    }

    static void autoMountGraceFired(void* context)
    {
        std::unique_ptr<AutoMountCtx> ctx(static_cast<AutoMountCtx*>(context));
        if (!ctx->alive->load())
            return;
        Impl* impl = ctx->impl;
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->mountAttempted.insert(ctx->bsd).second)
                return;
        }
        DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, impl->session,
                                                 ctx->bsd.c_str());
        if (disk == nullptr)
            return;
        CFDictionaryRef description = DADiskCopyDescription(disk);
        // Description gone = the device left; a volume path = automount won
        // after all. Either way there is nothing for us to do.
        const bool stillWaiting = description != nullptr && isUnmounted(description);
        if (description != nullptr)
            CFRelease(description);
        if (stillWaiting)
            DADiskMount(disk, nullptr, kDADiskMountOptionDefault, &autoMountDone,
                        new AutoMountCtx{ ctx->alive, impl, ctx->bsd });
        CFRelease(disk);
    }

    static void autoMountDone(DADiskRef, DADissenterRef dissenter, void* context)
    {
        std::unique_ptr<AutoMountCtx> ctx(static_cast<AutoMountCtx*>(context));
        if (!ctx->alive->load())
            return;
        std::function<void(bool, std::string)> cb;
        {
            const std::lock_guard<std::mutex> lock(ctx->impl->mutex);
            cb = ctx->impl->owner.onAutoMountResult;
        }
        if (cb)
            cb(dissenter == nullptr,
               dissenter == nullptr ? std::string(kPedalVolumeLabel) : dissenterMessage(dissenter));
    }

    // Register for termination of `bsd` while we hold a live service ref.
    // Registration failure only degrades push to poll — the next verdict()
    // still consults the registry — so it is not an error.
    void ensureInterest(const std::string& bsd, io_service_t service)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        currentBsd = bsd;
        if (tracked.count(bsd) != 0)
            return;
        auto* ctx = new InterestCtx{ this, bsd };
        io_object_t notification = IO_OBJECT_NULL;
        const kern_return_t kr = IOServiceAddInterestNotification(
            notifyPort, service, kIOGeneralInterest, &interestCallback, ctx, &notification);
        if (kr != KERN_SUCCESS) {
            delete ctx;
            return;
        }
        tracked[bsd] = Tracked{ notification, ctx, false };
    }

    // --- unmount / eject plumbing ---

    struct UnmountCtx {
        std::function<void(bool, std::string)> done;
        bool thenEjectWholeDisk = false;
    };

    static void ejectDone(DADiskRef, DADissenterRef dissenter, void* context)
    {
        auto* ctx = static_cast<UnmountCtx*>(context);
        // The volume is already unmounted — that is the safety guarantee; a
        // refused whole-disk eject is only worth a warning.
        ctx->done(true, dissenter != nullptr
                            ? "volume unmounted, but ejecting the disk was refused: "
                                  + dissenterMessage(dissenter)
                            : std::string{});
        delete ctx;
    }

    static void unmountDone(DADiskRef disk, DADissenterRef dissenter, void* context)
    {
        auto* ctx = static_cast<UnmountCtx*>(context);
        if (dissenter != nullptr) {
            ctx->done(false, dissenterMessage(dissenter));
            delete ctx;
            return;
        }
        if (!ctx->thenEjectWholeDisk) {
            ctx->done(true, {});
            delete ctx;
            return;
        }
        // Ejecting the whole disk drops the lingering device object as well —
        // in the 2026-07-22 recovery that, not the unmount, was what let a
        // fresh attach start from clean state.
        if (DADiskRef whole = DADiskCopyWholeDisk(disk)) {
            DADiskEject(whole, kDADiskEjectOptionDefault, &ejectDone, ctx);
            CFRelease(whole);
            return;
        }
        ctx->done(true, {});
        delete ctx;
    }

    void startUnmount(const std::filesystem::path& volumePath, DADiskUnmountOptions options,
                      bool thenEject, std::function<void(bool, std::string)> done)
    {
        const MountRecord rec = mountRecordFor(volumePath);
        if (!rec.statOk || !rec.isOwnMount || rec.bsd.empty()) {
            done(false, "not a device-backed mount: " + volumePath.string());
            return;
        }
        DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, session, rec.from.c_str());
        if (disk == nullptr) {
            done(false, "cannot address disk " + rec.from);
            return;
        }
        auto* ctx = new UnmountCtx{ std::move(done), thenEject };
        DADiskUnmount(disk, options, &unmountDone, ctx);
        CFRelease(disk);
    }
};

DeviceWatcher::DeviceWatcher() : impl_(std::make_unique<Impl>(*this))
{
    impl_->queue = dispatch_queue_create("com.darwinscat.loopercat.devicewatcher",
                                         DISPATCH_QUEUE_SERIAL);
    // Everything that can fail happens FIRST, before a single callback is
    // registered: a throw out of this constructor runs no destructor of ours,
    // so a registration already made would be left pointing at an Impl that
    // is about to be freed.
    impl_->session = DASessionCreate(kCFAllocatorDefault);
    if (impl_->session == nullptr)
        throw Error("cannot create a DiskArbitration session");
    impl_->notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (impl_->notifyPort == nullptr) {
        CFRelease(impl_->session);
        impl_->session = nullptr;
        throw Error("cannot create an IOKit notification port");
    }
    // Mount, as DiskArbitration sees it: the volume path key appears when a
    // disk is mounted and goes when it is unmounted.
    const void* volumePathKey[] = { kDADiskDescriptionVolumePathKey };
    CFArrayRef watchedKeys =
        CFArrayCreate(kCFAllocatorDefault, volumePathKey, 1, &kCFTypeArrayCallBacks);
    if (watchedKeys == nullptr) {
        IONotificationPortDestroy(impl_->notifyPort);
        CFRelease(impl_->session);
        impl_->session = nullptr;
        throw Error("cannot name the disk description keys to watch");
    }

    DASessionSetDispatchQueue(impl_->session, impl_->queue);
    IONotificationPortSetDispatchQueue(impl_->notifyPort, impl_->queue);
    DARegisterDiskAppearedCallback(impl_->session, kDADiskDescriptionMatchVolumeMountable,
                                   &Impl::diskAppearedCallback, impl_.get());
    DARegisterDiskDescriptionChangedCallback(impl_->session,
                                             kDADiskDescriptionMatchVolumeMountable, watchedKeys,
                                             &Impl::volumePathChangedCallback, impl_.get());
    CFRelease(watchedKeys);
}

DeviceWatcher::~DeviceWatcher()
{
    impl_->aliveFlag->store(false); // pending dispatch_after contexts become no-ops
    DAUnregisterCallback(impl_->session, reinterpret_cast<void*>(&Impl::diskAppearedCallback),
                         impl_.get());
    DAUnregisterCallback(impl_->session,
                         reinterpret_cast<void*>(&Impl::volumePathChangedCallback), impl_.get());
    DASessionSetDispatchQueue(impl_->session, nullptr);
    IONotificationPortDestroy(impl_->notifyPort); // also releases interest notifications
    // Drain the queue so no callback can be mid-flight against freed contexts.
    dispatch_sync_f(impl_->queue, nullptr, [](void*) {});
    CFRelease(impl_->session);
    dispatch_release(impl_->queue);
    for (auto& [bsd, entry] : impl_->tracked)
        delete entry.ctx;
}

lifecycle::Backing DeviceWatcher::verdict(const std::filesystem::path& volumePath,
                                          const std::filesystem::path& probeFile)
{
    const MountRecord rec = mountRecordFor(volumePath);
    // The caller just saw pedal content here; statfs failing now means the
    // mount is being torn down under us — the device story is over either way.
    if (!rec.statOk) {
        impl_->tellGhost(mountprobe::platformGhostLine("statfs no longer answers for "
                                                       + volumePath.string()
                                                       + " - the mount is going away under us"));
        return lifecycle::Backing::gone;
    }
    // A plain directory (synthetic test volume, --volume pin) or a mount with
    // no /dev backing (network share): no device to track, trusted as-is.
    if (!rec.isOwnMount || rec.bsd.empty()) {
        impl_->ghostTold.store(false);
        return lifecycle::Backing::untracked;
    }

    // IOBSDNameMatching's dictionary is consumed by the lookup.
    const io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, rec.bsd.c_str()));
    if (service == IO_OBJECT_NULL) {
        {
            // The registry has no media object behind this mount: a ghost.
            // Mark it so the verdict stays stable even if the registry state
            // flaps.
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->tracked[rec.bsd].dead = true;
        }
        impl_->tellGhost(mountprobe::platformGhostLine(
            "the IORegistry has no medium behind " + rec.bsd + " any more"));
        return lifecycle::Backing::gone;
    }
    {
        // A live registry entry under a name we hold as dead is a NEW device
        // incarnation — macOS recycles disk numbers across unplug/replug
        // cycles. Holding the grudge forever made every reconnect after an
        // eject look like a ghost and kicked the fresh pedal right back out.
        // Drop the stale tracking and bind to the new entry. (If the lookup
        // returned the old entry mid-termination, the fresh interest fires
        // terminated immediately and the next scan converges.)
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        // The path the mount-event callback above compares against.
        impl_->watchedVolume = volumePath.string();
        auto it = impl_->tracked.find(rec.bsd);
        if (it != impl_->tracked.end() && it->second.dead) {
            IOObjectRelease(it->second.notification);
            delete it->second.ctx;
            impl_->tracked.erase(it);
        }
    }
    impl_->ensureInterest(rec.bsd, service);
    // The medium's registry entry ID, which is never reused while the system
    // runs — the only thing that tells a recycled disk number apart from the
    // medium that used to carry it (issue #166: three mounts, all disk10s1).
    // A lookup that fails only degrades the identity to the BSD name plus the
    // mount epoch, so it is not an error.
    std::uint64_t medium = 0;
    if (IORegistryEntryGetRegistryEntryID(service, &medium) != KERN_SUCCESS)
        medium = 0;
    IOObjectRelease(service);

    // The registry says live — but the registry kept a detached pedal's whole
    // USB stack "active" (2026-07-23: leaving STORAGE pulls the medium, not
    // the device). The final word is the uncached-read probe. Its result is
    // one scan late by design: this call never waits on volume I/O.
    const std::string target = probeFile.string();
    const mountprobe::Incarnation here{ rec.bsd, medium, impl_->mountEpoch.load() };
    mountprobe::Facts facts;
    {
        const std::lock_guard<std::mutex> lock(impl_->ioProbe->mutex);
        const IoProbe& probe = *impl_->ioProbe;
        facts.ours = mountprobe::speaksFor(probe.mount, probe.file, here, target);
        if (facts.ours) {
            const auto now = mountprobe::Clock::now();
            facts.inFlight = probe.inFlight;
            facts.outstanding = now - probe.startedAt;
            facts.haveResult = probe.haveResult;
            facts.read = probe.read;
            facts.refusals = probe.refusals;
            facts.seenFor = now - probe.firstSeenAt;
        }
    }
    const mountprobe::Decision decision = mountprobe::decide(facts);
    if (decision.launchProbe)
        launchProbe(impl_->ioProbe, here, target);
    if (decision.call == mountprobe::Call::ghost) {
        impl_->tellGhost(mountprobe::ghostLine(facts));
        return lifecycle::Backing::gone;
    }
    impl_->ghostTold.store(false); // the next ghost is a new episode
    return lifecycle::Backing::live;
}

void DeviceWatcher::eject(const std::filesystem::path& volumePath,
                          std::function<void(bool, std::string)> done)
{
    impl_->startUnmount(volumePath, kDADiskUnmountOptionDefault, true, std::move(done));
}

void DeviceWatcher::forceUnmount(const std::filesystem::path& volumePath,
                                 std::function<void(bool, std::string)> done)
{
    // Force is safe by construction here: this path is only for ghosts, and
    // nothing can flush to a device that is already gone. Ejecting the whole
    // disk afterwards purges the stale device object too.
    impl_->startUnmount(volumePath, kDADiskUnmountOptionForce, true, std::move(done));
}

} // namespace loopercat::app
