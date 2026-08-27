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
std::atomic<uint64_t> gSuppressedCount{0};
std::atomic<uint64_t> gPassedCount{0};

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
                "hard-gate threshold changed: %dpx enabled=%d",
                thresholdPx, thresholdPx > 0 ? 1 : 0);
    }
    return thresholdPx;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    const int thresholdPx = readThresholdPx();
    const float absDx = std::fabs(horizontalDistancePx);
    const bool enabled = thresholdPx > 0;
    const bool suppressOriginal = enabled
            && absDx < static_cast<float>(thresholdPx);

    if (suppressOriginal) {
        gSuppressedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        gPassedCount.fetch_add(1, std::memory_order_relaxed);
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 120 && gLastSwipeLogMs.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "HARD_GATE internalDx=%.2f absDx=%.2f thresholdPx=%d enabled=%d suppressOriginal=%d readyFinish=%d side=%u suppressed=%llu passed=%llu",
                horizontalDistancePx, absDx, thresholdPx, enabled ? 1 : 0,
                suppressOriginal ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(
                        gSuppressedCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(
                        gPassedCount.load(std::memory_order_relaxed)));
    }

    // Hard gate: below the configured distance, do not enter Xiaomi's
    // onSwipeProcess path at all. The previous PauseDetector::reset approach
    // was observably ineffective on Launcher 8.01.02.5459: the detector reset
    // returned successfully while the SecurityManager sidebar still opened.
    // A zero threshold disables this gate and always preserves stock behavior.
    if (suppressOriginal) return;

    if (gOriginalOnSwipeProcess != nullptr) {
        gOriginalOnSwipeProcess(self, readyFinish, side, point, horizontalDistancePx);
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
                "HARD_GATE launcher signature mismatch base=%p; hook skipped",
                reinterpret_cast<void *>(base));
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(
            reinterpret_cast<void *>(onSwipe),
            reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HARD_GATE hook_func failed rc=%d backup=%p", rc, backup);
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gHookInstalled.store(true, std::memory_order_release);
    const int thresholdPx = readThresholdPx();
    logLine(ANDROID_LOG_INFO,
            "HARD_GATE hook installed launcher=8.01.02.5459 base=%p thresholdPx=%d enabled=%d",
            reinterpret_cast<void *>(base), thresholdPx, thresholdPx > 0 ? 1 : 0);
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) {
            logLine(ANDROID_LOG_WARN, "HARD_GATE hook worker left target process; aborting");
            return;
        }
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "HARD_GATE found %s base=%p",
                    library.path.c_str(), reinterpret_cast<void *>(library.base));
            installHook(library.base);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "HARD_GATE timed out waiting for %s", kTargetLibrary);
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
            "HARD_GATE native_init candidate api=%u exe=%s process=%s hook_func=%p",
            entries->version, exe.c_str(), process.c_str(),
            reinterpret_cast<void *>(entries->hook_func));

    if (entries->hook_func == nullptr) {
        logLine(ANDROID_LOG_ERROR, "HARD_GATE native_init rejected: hook_func is null");
        return nullptr;
    }
    if (exe != kSpawnerPath || process != kTargetPackage) return nullptr;

    gHookFunction = entries->hook_func;
    logLine(ANDROID_LOG_INFO, "HARD_GATE native_init accepted api=%u", entries->version);
    ensureWorkerStarted();
    return onLibraryLoaded;
}
