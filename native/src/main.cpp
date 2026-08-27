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

// Exact reverse engineering target:
// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R.
//
// GestureInputBackHelper::on_swipe_process keeps the real horizontal distance
// in s8. It calls BackGestureUtils::convert_offset(distance), divides the result
// by 20, and repeatedly compares that normalized value against float 0.8.
// convert_offset normalizes pixels by (110dp), therefore the stock transition
// boundary is 110dp * 0.8 = 88dp. On the test device (480 dpi / density 3.0)
// this is exactly 264px.
//
// Before the user gate is reached we keep calling Xiaomi's original function,
// but clamp its X input to one pixel below the stock 88dp boundary. This keeps
// the complete stock BACK animation, release handling, haptics and reversible
// state machine alive, while preventing IntentBallPauseDetector::fire() and
// start_intent_ball_exit_anim() from being reached early.
constexpr uintptr_t kOnSwipeProcessOffset = 0x816fc4;
constexpr uintptr_t kConvertOffsetOffset = 0x773814;

constexpr uint8_t kOnSwipeProcessSignature[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};
constexpr uint8_t kConvertOffsetSignature[] = {
        0xe9, 0x23, 0xbd, 0x6d, 0xfd, 0x7b, 0x01, 0xa9,
        0xf4, 0x4f, 0x02, 0xa9, 0xfd, 0x43, 0x00, 0x91,
        0x08, 0x40, 0x20, 0x1e, 0x00, 0xe4, 0x00, 0x2f,
};

using OnSwipeProcessFn = void (*)(void *, bool, uint32_t, const void *, float);
using ConvertOffsetFn = float (*)(float);

HookFunType gHookFunction = nullptr;
OnSwipeProcessFn gOriginalOnSwipeProcess = nullptr;
ConvertOffsetFn gConvertOffset = nullptr;

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gHookInstalled{false};
std::atomic<int> gCachedThresholdPx{kDefaultThresholdPx};
std::atomic<int64_t> gLastThresholdReadMs{0};
std::atomic<int64_t> gLastBoundaryReadMs{0};
std::atomic<int64_t> gLastSwipeLogMs{0};
std::atomic<uint32_t> gBoundaryBits{0};
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

uint32_t floatBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bitsFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
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
        if (end != value && *end == '\0') {
            if (parsed < 0) parsed = 0;
            if (parsed > kMaxThresholdPx) parsed = kMaxThresholdPx;
            thresholdPx = static_cast<int>(parsed);
        }
    }

    const int previous = gCachedThresholdPx.exchange(thresholdPx, std::memory_order_relaxed);
    gLastThresholdReadMs.store(now, std::memory_order_relaxed);
    if (previous != thresholdPx) {
        logLine(ANDROID_LOG_INFO, "STOCK_GATE threshold changed: %dpx enabled=%d",
                thresholdPx, thresholdPx > 0 ? 1 : 0);
    }
    return thresholdPx;
}

float readStockBoundaryPx() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastBoundaryReadMs.load(std::memory_order_relaxed);
    const float cached = bitsFloat(gBoundaryBits.load(std::memory_order_relaxed));
    if (cached > 0.0f && now - last < 2000) return cached;
    if (gConvertOffset == nullptr) return cached;

    // In convert_offset's linear region:
    // convert_offset(px) = 20 * px / stockScalePx.
    // The stock state boundary is normalized 0.8, therefore:
    // boundaryPx = 0.8 * stockScalePx = 16 / convert_offset(1px).
    const float convertedOnePx = gConvertOffset(1.0f);
    if (!std::isfinite(convertedOnePx) || convertedOnePx <= 0.0f) return cached;

    const float boundaryPx = 16.0f / convertedOnePx;
    if (!std::isfinite(boundaryPx) || boundaryPx < 40.0f || boundaryPx > 800.0f) {
        return cached;
    }

    const uint32_t oldBits = gBoundaryBits.exchange(floatBits(boundaryPx),
                                                     std::memory_order_relaxed);
    gLastBoundaryReadMs.store(now, std::memory_order_relaxed);
    const float previous = bitsFloat(oldBits);
    if (std::fabs(previous - boundaryPx) >= 0.5f) {
        logLine(ANDROID_LOG_INFO,
                "STOCK_GATE stock boundary resolved: %.2fpx (88dp), guard=%.2fpx",
                boundaryPx, std::max(0.0f, boundaryPx - 1.0f));
    }
    return boundaryPx;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    const int thresholdPx = readThresholdPx();
    const float stockBoundaryPx = readStockBoundaryPx();
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);
    const bool enabled = thresholdPx > 0 && stockGuardPx > 0.0f;
    const bool userGateReached = !enabled || absDx >= static_cast<float>(thresholdPx);

    float effectiveDistancePx = horizontalDistancePx;
    bool clamped = false;

    if (enabled && !userGateReached && absDx > stockGuardPx) {
        effectiveDistancePx = std::copysign(stockGuardPx, horizontalDistancePx);
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
                "STOCK_GATE rawDx=%.2f effectiveDx=%.2f thresholdPx=%d stockBoundaryPx=%.2f guardPx=%.2f gateReached=%d clamped=%d readyFinish=%d side=%u clampedCount=%llu passthroughCount=%llu",
                horizontalDistancePx, effectiveDistancePx, thresholdPx,
                stockBoundaryPx, stockGuardPx, userGateReached ? 1 : 0,
                clamped ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(gClampedCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(gPassthroughCount.load(std::memory_order_relaxed)));
    }

    if (gOriginalOnSwipeProcess != nullptr) {
        gOriginalOnSwipeProcess(self, readyFinish, side, point, effectiveDistancePx);
    }
}

bool installHook(uintptr_t base) {
    if (gHookInstalled.load(std::memory_order_acquire)) return true;
    if (base == 0 || gHookFunction == nullptr) return false;

    const uintptr_t onSwipe = base + kOnSwipeProcessOffset;
    const uintptr_t convertOffset = base + kConvertOffsetOffset;
    if (!matchesSignature(onSwipe, kOnSwipeProcessSignature, sizeof(kOnSwipeProcessSignature))) {
        logLine(ANDROID_LOG_ERROR,
                "STOCK_GATE on_swipe_process signature mismatch; hook skipped");
        return false;
    }
    if (!matchesSignature(convertOffset, kConvertOffsetSignature,
                          sizeof(kConvertOffsetSignature))) {
        logLine(ANDROID_LOG_ERROR,
                "STOCK_GATE convert_offset signature mismatch; hook skipped");
        return false;
    }

    gConvertOffset = reinterpret_cast<ConvertOffsetFn>(convertOffset);
    const float boundary = readStockBoundaryPx();
    if (!(boundary > 0.0f)) {
        logLine(ANDROID_LOG_ERROR,
                "STOCK_GATE failed to resolve stock 88dp boundary; hook skipped");
        gConvertOffset = nullptr;
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(onSwipe),
                                 reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "STOCK_GATE hook_func failed rc=%d backup=%p", rc, backup);
        gConvertOffset = nullptr;
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gHookInstalled.store(true, std::memory_order_release);
    const int thresholdPx = readThresholdPx();
    logLine(ANDROID_LOG_INFO,
            "STOCK_GATE hook installed launcher=8.01.02.5459 base=%p thresholdPx=%d stockBoundaryPx=%.2f guardPx=%.2f",
            reinterpret_cast<void *>(base), thresholdPx, boundary, boundary - 1.0f);
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) return;
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "STOCK_GATE found %s base=%p",
                    library.path.c_str(), reinterpret_cast<void *>(library.base));
            installHook(library.base);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "STOCK_GATE timed out waiting for %s", kTargetLibrary);
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
    if (entries == nullptr || entries->hook_func == nullptr || !isTargetProcess()) {
        return nullptr;
    }
    gHookFunction = entries->hook_func;
    logLine(ANDROID_LOG_INFO,
            "STOCK_GATE native_init api=%u exe=%s process=%s",
            entries->version, readExecutable().c_str(), readProcessName().c_str());
    ensureWorkerStarted();
    return onLibraryLoaded;
}
