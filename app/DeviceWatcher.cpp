// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// macOS implementation notes. Liveness is answered by the IORegistry (does an
// IOMedia object with this BSD name still exist?), never by the mount table
// or diskarbitrationd's cached view — both kept claiming a detached pedal was
// present for hours in the 2026-07-22 incident. Termination push uses IOKit
// general-interest notifications; unmount/eject go through DiskArbitration.

#include "DeviceWatcher.h"

#include <loopercat/Error.hpp>

#include <DiskArbitration/DiskArbitration.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>

#include <dispatch/dispatch.h>
#include <sys/mount.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace loopercat::app {

namespace {

    // The factory volume label of the RC-5's mass-storage export (rc5cat).
    // Auto-mount keys on it because an unmounted volume cannot be content-
    // checked; a user-relabeled pedal just falls back to manual mounting.
    constexpr const char* kPedalVolumeLabel = "BOSS RC-5";

    // How long the OS automount gets before the watcher steps in.
    constexpr int64_t kAutoMountGraceSeconds = 5;

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
    std::set<std::string> mountAttempted;   // one auto-mount per attach; guarded by mutex
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
    impl_->session = DASessionCreate(kCFAllocatorDefault);
    if (impl_->session == nullptr)
        throw Error("cannot create a DiskArbitration session");
    DASessionSetDispatchQueue(impl_->session, impl_->queue);
    DARegisterDiskAppearedCallback(impl_->session, kDADiskDescriptionMatchVolumeMountable,
                                   &Impl::diskAppearedCallback, impl_.get());

    impl_->notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (impl_->notifyPort == nullptr)
        throw Error("cannot create an IOKit notification port");
    IONotificationPortSetDispatchQueue(impl_->notifyPort, impl_->queue);
}

DeviceWatcher::~DeviceWatcher()
{
    impl_->aliveFlag->store(false); // pending dispatch_after contexts become no-ops
    DAUnregisterCallback(impl_->session, reinterpret_cast<void*>(&Impl::diskAppearedCallback),
                         impl_.get());
    DASessionSetDispatchQueue(impl_->session, nullptr);
    IONotificationPortDestroy(impl_->notifyPort); // also releases interest notifications
    // Drain the queue so no callback can be mid-flight against freed contexts.
    dispatch_sync_f(impl_->queue, nullptr, [](void*) {});
    CFRelease(impl_->session);
    dispatch_release(impl_->queue);
    for (auto& [bsd, entry] : impl_->tracked)
        delete entry.ctx;
}

lifecycle::Backing DeviceWatcher::verdict(const std::filesystem::path& volumePath)
{
    const MountRecord rec = mountRecordFor(volumePath);
    // The caller just saw pedal content here; statfs failing now means the
    // mount is being torn down under us — the device story is over either way.
    if (!rec.statOk)
        return lifecycle::Backing::gone;
    // A plain directory (synthetic test volume, --volume pin) or a mount with
    // no /dev backing (network share): no device to track, trusted as-is.
    if (!rec.isOwnMount || rec.bsd.empty())
        return lifecycle::Backing::untracked;

    // IOBSDNameMatching's dictionary is consumed by the lookup.
    const io_service_t service = IOServiceGetMatchingService(
        kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, rec.bsd.c_str()));
    if (service == IO_OBJECT_NULL) {
        // The registry has no media object behind this mount: a ghost. Mark
        // it so the verdict stays stable even if the registry state flaps.
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->tracked[rec.bsd].dead = true;
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
        auto it = impl_->tracked.find(rec.bsd);
        if (it != impl_->tracked.end() && it->second.dead) {
            IOObjectRelease(it->second.notification);
            delete it->second.ctx;
            impl_->tracked.erase(it);
        }
    }
    impl_->ensureInterest(rec.bsd, service);
    IOObjectRelease(service);
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
