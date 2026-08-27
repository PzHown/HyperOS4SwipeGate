#include "native_api.h"

#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
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
constexpr const char *kThresholdDpProperty = "persist.hyperos4swipegate.threshold_dp";
constexpr int kDefaultThresholdDp = 0;       // 0 = Xiaomi stock/default boundary.
constexpr int kStockBoundaryDp = 88;
constexpr int kMaxThresholdDp = 320;

// Exact reverse engineering target:
// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R.
//
// GestureInputBackHelper::on_swipe_process keeps the real horizontal distance
// in s8. It calls BackGestureUtils::convert_offset(distance), divides the result
// by 20, and repeatedly compares that normalized value against float 0.8.
// convert_offset normalizes pixels by 110dp, therefore Xiaomi's stock sidebar
// transition boundary is 110dp * 0.8 = 88dp.
//
// IMPORTANT: never call BackGestureUtils::convert_offset ourselves. The Rust
// implementation unwraps launcher state which is not initialized yet when the
// LSPosed native module is loaded, and an early call aborts com.miui.home with
// Option::unwrap(None). We only use the reverse-engineered 88dp constant and
// convert dp to px from Android/Xiaomi density system properties. If density
// cannot be resolved, custom delaying fails closed to Xiaomi stock behavior.
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
std::atomic<int> gCachedThresholdDp{kDefaultThresholdDp};
std::atomic<int> gCachedDensityDpi{-1};  // -1 unresolved, 0 unavailable.
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

int parseDensityDpi(const char *text) {
    if (text == nullptr || *text == '\0') return 0;
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0' || parsed < 120 || parsed > 1000) return 0;
    return static_cast<int>(parsed);
}

int densityFromProperty(const char *name) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) <= 0) return 0;
    return parseDensityDpi(value);
}

int densityFromMiuiResolution() {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("persist.sys.miui_resolution", value) <= 0) return 0;
    const char *lastComma = std::strrchr(value, ',');
    if (lastComma == nullptr || *(lastComma + 1) == '\0') return 0;
    return parseDensityDpi(lastComma + 1);
}

int readDensityDpi() {
    const int cached = gCachedDensityDpi.load(std::memory_order_acquire);
    if (cached >= 0) return cached;

    int densityDpi = densityFromMiuiResolution();
    const char *source = "persist.sys.miui_resolution";
    if (densityDpi <= 0) {
        densityDpi = densityFromProperty("persist.sys.dpi");
        source = "persist.sys.dpi";
    }
    if (densityDpi <= 0) {
        densityDpi = densityFromProperty("ro.sf.lcd_density");
        source = "ro.sf.lcd_density";
    }
    if (densityDpi <= 0) {
        densityDpi = densityFromProperty("qemu.sf.lcd_density");
        source = "qemu.sf.lcd_density";
    }

    if (densityDpi <= 0) {
        densityDpi = 0;
        source = "unavailable";
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE density unavailable; custom delay disabled, stock passthrough");
    } else {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE density resolved: %ddpi source=%s pxPerDp=%.3f stock88dp=%.2fpx",
                densityDpi, source,
                static_cast<float>(densityDpi) / 160.0f,
                static_cast<float>(kStockBoundaryDp * densityDpi) / 160.0f);
    }

    gCachedDensityDpi.store(densityDpi, std::memory_order_release);
    return densityDpi;
}

float dpToPx(int dp, int densityDpi) {
    if (dp <= 0 || densityDpi <= 0) return 0.0f;
    return static_cast<float>(dp) * static_cast<float>(densityDpi) / 160.0f;
}

int readThresholdDp() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastThresholdReadMs.load(std::memory_order_relaxed);
    if (now - last < 250) return gCachedThresholdDp.load(std::memory_order_relaxed);

    int thresholdDp = kDefaultThresholdDp;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(kThresholdDpProperty, value) > 0) {
        char *end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        if (end != value && *end == '\0') {
            if (parsed < 0) parsed = 0;
            if (parsed > kMaxThresholdDp) parsed = kMaxThresholdDp;
            thresholdDp = static_cast<int>(parsed);
        }
    }

    const int previous = gCachedThresholdDp.exchange(thresholdDp, std::memory_order_relaxed);
    gLastThresholdReadMs.store(now, std::memory_order_relaxed);
    if (previous != thresholdDp) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE threshold changed: configured=%ddp effective=%ddp defaultAlias=%d",
                thresholdDp,
                thresholdDp == 0 ? kStockBoundaryDp : std::max(thresholdDp, kStockBoundaryDp),
                thresholdDp == 0 ? 1 : 0);
    }
    return thresholdDp;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp
            : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0 ? dpToPx(kStockBoundaryDp, densityDpi) : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

    // 0dp (and any custom value <= stock 88dp) is true Xiaomi stock behavior:
    // no clamp at all. For >88dp, clamp the distance just below Xiaomi's stock
    // state boundary until the configured dp gate is reached. If density cannot
    // be resolved, custom delaying is disabled and the stock value is passed.
    const bool delayBeyondStock = effectiveDp > kStockBoundaryDp
            && densityDpi > 0
            && stockGuardPx > 0.0f
            && userGatePx > stockBoundaryPx;
    const bool userGateReached = !delayBeyondStock || absDx >= userGatePx;

    float effectiveDistancePx = horizontalDistancePx;
    bool clamped = false;

    if (delayBeyondStock && !userGateReached && absDx > stockGuardPx) {
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
                "DP_GATE rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f guardPx=%.2f delayBeyondStock=%d gateReached=%d clamped=%d readyFinish=%d side=%u clampedCount=%llu passthroughCount=%llu",
                horizontalDistancePx, effectiveDistancePx,
                configuredDp, effectiveDp, densityDpi, userGatePx,
                stockBoundaryPx, stockGuardPx,
                delayBeyondStock ? 1 : 0, userGateReached ? 1 : 0,
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
    if (!matchesSignature(onSwipe, kOnSwipeProcessSignature, sizeof(kOnSwipeProcessSignature))) {
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE on_swipe_process signature mismatch; hook skipped");
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(onSwipe),
                                 reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE hook_func failed rc=%d backup=%p", rc, backup);
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gHookInstalled.store(true, std::memory_order_release);

    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp
            : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE hook installed launcher=8.01.02.5459 base=%p configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f noRustConvertOffset=1",
            reinterpret_cast<void *>(base), configuredDp, effectiveDp, densityDpi,
            densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f);
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) return;
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "DP_GATE found %s base=%p",
                    library.path.c_str(), reinterpret_cast<void *>(library.base));
            installHook(library.base);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "DP_GATE timed out waiting for %s", kTargetLibrary);
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
            "DP_GATE native_init accepted api=%u exe=%s process=%s hook_func=%p",
            entries->version, readExecutable().c_str(), readProcessName().c_str(),
            reinterpret_cast<void *>(entries->hook_func));
    ensureWorkerStarted();
    return onLibraryLoaded;
}
