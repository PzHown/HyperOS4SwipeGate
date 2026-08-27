#include "native_api.h"

#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kSpawnerPath = "/system_ext/bin/hyos_spawner";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr const char *kThresholdPxProperty = "persist.hyperos4swipegate.threshold_px";
constexpr int kDefaultThresholdPx = 660;
constexpr int kMaxThresholdPx = 1600;

// HyperOS 4's stock gesture is already a reversible X-axis state machine:
// while the finger remains down, moving inward can switch from BACK to the
// sidebar-prep state and moving outward can switch back to BACK. Keep feeding
// every update to Xiaomi, but before the user-configured gate is reached clamp
// the X distance to just below the stock sidebar transition region. This keeps
// stock BACK animation/haptics/release behavior alive without letting the
// stock state machine cross into the sidebar branch early.
//
// 199 px is intentionally one pixel below the first 200 px boundary used by
// the previous window-gate experiment. If device testing shows the actual
// stock transition boundary differs, only this guard value needs tuning.
constexpr float kStockSidebarGuardPx = 199.0f;

// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R
constexpr uintptr_t kOnSwipeProcessOffset = 0x816fc4;
constexpr uint8_t kOnSwipeProcessSignature[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};

using OnSwipeProcessFn = void (*)(void *, bool, uint32_t, const void *, float);

HookFunType gHookFunction = nullptr;
OnSwipeProcessFn gOriginalOnSwipeProcess = nullptr;

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gHookInstalled{false};
std::atomic<int> gCachedThresholdPx{kDefaultThresholdPx};
std::atomic<int64_t> gLastThresholdReadMs{0};
std::atomic<int64_t> gLastSwipeLogMs{0};
std::atomic<uint64_t> gClampedCount{0};
std::atomic<uint64_t> gPassthroughCount{0};

int64_t monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string readSmallFile(const char *path, size_t limit = 512) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string value(limit, '\0');
    const ssize_t n = read(fd, value.data(), value.size() - 1);
    close(fd);
    if (n <= 0) return {};
    value.resize(static_cast<size_t>(n));
    const size_t zero = value.find('\0');
    if (zero != std::string::npos) value.resize(zero);
    return value;
}

std::string readExecutable() {
    char buffer[256]{};
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buffer)) return {};
    buffer[n] = '\0';
    return std::string(buffer);
}

std::string readProcessName() {
    return readSmallFile("/proc/self/cmdline", 256);
}

bool isTargetProcess() {
    return readExecutable() == kSpawnerPath && readProcessName() == kTargetPackage;
}

void fileLog(const char *message) {
    static constexpr const char *paths[] = {
            "/data/user_de/0/com.miui.home/cache/hyperos4swipegate_native.log",
            "/data/user/0/com.miui.home/cache/hyperos4swipegate_native.log",
            "/data/data/com.miui.home/cache/hyperos4swipegate_native.log",
    };
    for (const char *path : paths) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        dprintf(fd, "%s\n", message == nullptr ? "" : message);
        close(fd);
        return;
    }
}

void logLine(int priority, const char *format, ...) {
    char buffer[1536]{};
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    __android_log_write(priority, kTag, buffer);
    fileLog(buffer);
}

struct LibraryInfo {
    uintptr_t base = 0;
    std::string path;
};

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    const std::string path(info->dlpi_name);
    if (path.find(kTargetLibrary) == std::string::npos) return 0;
    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->path = path;
    return 1;
}

LibraryInfo findLauncherLibrary() {
    LibraryInfo result;
    dl_iterate_phdr(libraryCallback, &result);
    return result;
}

bool matchesSignature(uintptr_t address, const uint8_t *signature, size_t size) {
    return address != 0
            && std::memcmp(reinterpret_cast<const void *>(address), signature, size) == 0;
}

int readThresholdPx() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastThresholdReadMs.load(std::memory_order_relaxed);
    if (now - last < 250) return gCachedThresholdPx.load(std::memory_order_relaxed);

    int thresholdPx = kDefaultThresholdPx;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(kThresholdPxProperty, value) > 0) {
        char *end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        if (end != value) {
            if (parsed < 0) parsed = 0;
            if (parsed > kMaxThresholdPx) parsed = kMaxThresholdPx;
            thresholdPx = static_cast<int>(parsed);
        }
    }

    const int previous = gCachedThresholdPx.exchange(thresholdPx, std::memory_order_relaxed);
    gLastThresholdReadMs.store(now, std::memory_order_relaxed);
    if (previous != thresholdPx) {
        logLine(ANDROID_LOG_INFO,
                "x-clamp threshold changed: %dpx enabled=%d guard=%.0fpx",
                thresholdPx, thresholdPx > 0 ? 1 : 0, kStockSidebarGuardPx);
    }
    return thresholdPx;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    const int thresholdPx = readThresholdPx();
    const float absDx = std::fabs(horizontalDistancePx);
    const bool enabled = thresholdPx > 0;
    const bool thresholdReached = !enabled
            || absDx >= static_cast<float>(thresholdPx);

    float effectiveDistancePx = horizontalDistancePx;
    bool clamped = false;

    if (enabled && !thresholdReached && absDx > kStockSidebarGuardPx) {
        effectiveDistancePx = std::copysign(kStockSidebarGuardPx,
                                             horizontalDistancePx);
        clamped = true;
        gClampedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        gPassthroughCount.fetch_add(1, std::memory_order_relaxed);
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 120 && gLastSwipeLogMs.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "X_CLAMP rawDx=%.2f effectiveDx=%.2f thresholdPx=%d guardPx=%.0f enabled=%d thresholdReached=%d clamped=%d readyFinish=%d side=%u clampedCount=%llu passthroughCount=%llu",
                horizontalDistancePx, effectiveDistancePx, thresholdPx,
                kStockSidebarGuardPx, enabled ? 1 : 0,
                thresholdReached ? 1 : 0, clamped ? 1 : 0,
                readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(
                        gClampedCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                        gPassthroughCount.load(std::memory_order_relaxed)));
    }

    // Always call Xiaomi's original state machine. Only the X distance is
    // delayed. This preserves the stock reversible transition: after crossing
    // the user gate, moving the still-held finger back below the gate clamps X
    // again and lets Xiaomi naturally animate back to the BACK state.
    if (gOriginalOnSwipeProcess != nullptr) {
        gOriginalOnSwipeProcess(
                self, readyFinish, side, point, effectiveDistancePx);
    }
}

bool installHook(uintptr_t base) {
    if (gHookInstalled.load(std::memory_order_acquire)) return true;
    if (base == 0 || gHookFunction == nullptr) return false;

    const uintptr_t onSwipe = base + kOnSwipeProcessOffset;
    const bool swipeOk = matchesSignature(
            onSwipe, kOnSwipeProcessSignature, sizeof(kOnSwipeProcessSignature));
    if (!swipeOk) {
        logLine(ANDROID_LOG_ERROR,
                "X_CLAMP launcher signature mismatch base=%p; hook skipped",
                reinterpret_cast<void *>(base));
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(
            reinterpret_cast<void *>(onSwipe),
            reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "X_CLAMP hook_func failed rc=%d backup=%p", rc, backup);
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gHookInstalled.store(true, std::memory_order_release);
    const int thresholdPx = readThresholdPx();
    logLine(ANDROID_LOG_INFO,
            "X_CLAMP hook installed launcher=8.01.02.5459 base=%p thresholdPx=%d enabled=%d guardPx=%.0f",
            reinterpret_cast<void *>(base), thresholdPx,
            thresholdPx > 0 ? 1 : 0, kStockSidebarGuardPx);
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) {
            logLine(ANDROID_LOG_WARN, "X_CLAMP hook worker left target process; aborting");
            return;
        }
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "X_CLAMP found %s base=%p",
                    library.path.c_str(), reinterpret_cast<void *>(library.base));
            installHook(library.base);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "X_CLAMP timed out waiting for %s", kTargetLibrary);
}

void ensureWorkerStarted() {
    if (!isTargetProcess()) return;
    bool expected = false;
    if (!gWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread(hookWorker).detach();
}

void onLibraryLoaded(const char *name, void *) {
    if (!isTargetProcess() || name == nullptr) return;
    if (std::strstr(name, kTargetLibrary) != nullptr) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) installHook(library.base);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) return nullptr;

    const std::string exe = readExecutable();
    const std::string process = readProcessName();
    logLine(ANDROID_LOG_INFO,
            "X_CLAMP native_init candidate api=%u exe=%s process=%s hook_func=%p",
            entries->version, exe.c_str(), process.c_str(),
            reinterpret_cast<void *>(entries->hook_func));

    if (entries->hook_func == nullptr) {
        logLine(ANDROID_LOG_ERROR, "X_CLAMP native_init rejected: hook_func is null");
        return nullptr;
    }
    if (exe != kSpawnerPath || process != kTargetPackage) return nullptr;

    gHookFunction = entries->hook_func;
    logLine(ANDROID_LOG_INFO, "X_CLAMP native_init accepted api=%u", entries->version);
    ensureWorkerStarted();
    return onLibraryLoaded;
}
