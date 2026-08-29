#include "native_api.h"

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kSpawnerPath = "/system_ext/bin/hyos_spawner";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr const char *kThresholdDpProperty = "persist.hyperos4swipegate.threshold_dp";
constexpr int kDefaultThresholdDp = 0;
constexpr int kStockBoundaryDp = 88;
constexpr int kMaxThresholdDp = 320;
constexpr int64_t kHookHealthIntervalMs = 500;
constexpr int64_t kHealthyLogIntervalMs = 60000;
constexpr int64_t kRepairCooldownMs = 1500;
constexpr int64_t kHookIdleWaitMs = 120;
constexpr int64_t kPatternRescanIntervalMs = 5000;
constexpr size_t kHookProbeSize = 16;
constexpr size_t kMaxExecutableRanges = 12;

// Reverse-engineering reference only. This offset is deliberately NOT used to
// locate the hook target anymore. It is kept so diagnostics can show how far a
// newly resolved target moved from the build that was originally analysed.
constexpr uintptr_t kReferenceOnSwipeProcessOffset = 0x816fc4;

// Exact build used to derive the first validated pattern:
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
// convert dp to px from Android/Xiaomi density system properties.
constexpr uint8_t kOnSwipeProcessPatternV1[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};
constexpr uint8_t kOnSwipeProcessMaskV1[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

constexpr uint8_t kOnSwipeProcessPatternV2[] = {
        0xff, 0x83, 0x04, 0xd1, 0xeb, 0x2b, 0x0a, 0x6d,
        0xe9, 0x23, 0x0b, 0x6d, 0xfd, 0x7b, 0x0c, 0xa9,
        0xfc, 0x6b, 0x00, 0xf9, 0xfa, 0x67, 0x0e, 0xa9,
};
constexpr uint8_t kOnSwipeProcessMaskV2[] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

enum class SwipeProcessAbi : uint8_t { PointPointerV1, InlinePointFloatsV2 };

struct PatternSpec {
    const char *name;
    const uint8_t *bytes;
    const uint8_t *mask;
    size_t size;
    SwipeProcessAbi abi;
};

constexpr PatternSpec kOnSwipeProcessPatterns[] = {
        {"8.01.02.5459-v1", kOnSwipeProcessPatternV1, kOnSwipeProcessMaskV1,
         sizeof(kOnSwipeProcessPatternV1), SwipeProcessAbi::PointPointerV1},
        {"8.01.02.6174-v2", kOnSwipeProcessPatternV2, kOnSwipeProcessMaskV2,
         sizeof(kOnSwipeProcessPatternV2), SwipeProcessAbi::InlinePointFloatsV2},
};

static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));
static_assert(sizeof(kOnSwipeProcessPatternV1) >= kHookProbeSize);

using OnSwipeProcessFnV1 = void (*)(void *, bool, uint32_t, const void *, float);
using OnSwipeProcessFnV2 = void (*)(void *, bool, uint32_t, float, float, float);

HookFunType gHookFunction = nullptr;
UnhookFunType gUnhookFunction = nullptr;
std::atomic<OnSwipeProcessFnV1> gOriginalOnSwipeProcessV1{nullptr};
std::atomic<OnSwipeProcessFnV2> gOriginalOnSwipeProcessV2{nullptr};

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gHookInstalled{false};
std::atomic<uintptr_t> gHookedBase{0};
std::atomic<uintptr_t> gHookedTarget{0};
std::atomic<int> gCachedThresholdDp{kDefaultThresholdDp};
std::atomic<int> gCachedDensityDpi{-1};
std::atomic<int64_t> gLastThresholdReadMs{0};
std::atomic<int64_t> gLastSwipeLogMs{0};
std::atomic<int64_t> gLastHealthyLogMs{0};
std::atomic<int64_t> gLastRepairAttemptMs{0};
std::atomic<int64_t> gLastPatternScanMs{0};
std::atomic<int> gHookHealthState{0}; // 0 unknown, 1 healthy, 2 restored, 3 foreign/error, 4 repairing.
std::atomic<uint32_t> gActiveHookCalls{0};
std::atomic<uint64_t> gRepairCount{0};
std::atomic<uint64_t> gClampedCount{0};
std::atomic<uint64_t> gPassthroughCount{0};

std::mutex gHookMutex;
std::array<uint8_t, kHookProbeSize> gInstalledPatchHead{};
std::array<uint8_t, kHookProbeSize> gExpectedOriginalHead{};
bool gInstalledPatchHeadReady = false;
bool gExpectedOriginalHeadReady = false;
const char *gActivePatternName = "<none>";

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

// LSPosed initializes HyperOS native modules in the root hyos_spawner before
// the launcher child is specialized. At that stage /proc/self/cmdline is commonly
// `usap64`, not com.miui.home. Keep executable identity as the hard injection
// boundary and only require the launcher cmdline for child-only work such as the
// watchdog thread.
bool isLauncherProcess() {
    return readProcessName() == kTargetPackage;
}

bool isHyosSpawnerProcessFamily() {
    return readExecutable() == kSpawnerPath;
}

bool isTargetProcess() {
    return isHyosSpawnerProcessFamily() && isLauncherProcess();
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

struct ExecutableRange {
    uintptr_t start = 0;
    size_t size = 0;
};

struct LibraryInfo {
    uintptr_t base = 0;
    std::string path;
    std::array<ExecutableRange, kMaxExecutableRanges> executableRanges{};
    size_t executableRangeCount = 0;
};

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    const std::string path(info->dlpi_name);
    if (path.find(kTargetLibrary) == std::string::npos) return 0;

    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->path = path;
    result->executableRangeCount = 0;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0) {
            continue;
        }
        if (result->executableRangeCount >= result->executableRanges.size()) break;
        auto &range = result->executableRanges[result->executableRangeCount++];
        range.start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);
        range.size = static_cast<size_t>(phdr.p_memsz);
    }
    return 1;
}

LibraryInfo findLauncherLibrary() {
    LibraryInfo result;
    dl_iterate_phdr(libraryCallback, &result);
    return result;
}

bool readProbeHead(uintptr_t address, std::array<uint8_t, kHookProbeSize> &out) {
    if (address == 0) return false;
    std::memcpy(out.data(), reinterpret_cast<const void *>(address), out.size());
    return true;
}

bool probeEquals(const std::array<uint8_t, kHookProbeSize> &left,
                 const std::array<uint8_t, kHookProbeSize> &right) {
    return std::memcmp(left.data(), right.data(), left.size()) == 0;
}

bool probeEqualsExpectedOriginal(const std::array<uint8_t, kHookProbeSize> &head) {
    return gExpectedOriginalHeadReady && probeEquals(head, gExpectedOriginalHead);
}

std::string probeHex(const std::array<uint8_t, kHookProbeSize> &head) {
    char text[kHookProbeSize * 2 + 1]{};
    for (size_t i = 0; i < head.size(); ++i) {
        std::snprintf(text + i * 2, 3, "%02x", static_cast<unsigned int>(head[i]));
    }
    return std::string(text);
}

bool patternMatchesAt(uintptr_t address, const PatternSpec &pattern) {
    if (address == 0 || pattern.bytes == nullptr || pattern.mask == nullptr || pattern.size == 0) {
        return false;
    }
    const auto *data = reinterpret_cast<const uint8_t *>(address);
    for (size_t i = 0; i < pattern.size; ++i) {
        if (((data[i] ^ pattern.bytes[i]) & pattern.mask[i]) != 0) return false;
    }
    return true;
}

struct PatternMatch {
    uintptr_t address = 0;
    const PatternSpec *pattern = nullptr;
};

struct TargetResolution {
    PatternMatch match{};
    size_t uniqueCandidates = 0;
};

TargetResolution resolveOnSwipeProcessTarget(const LibraryInfo &library) {
    std::vector<PatternMatch> matches;
    matches.reserve(2);

    for (const PatternSpec &pattern : kOnSwipeProcessPatterns) {
        for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
            const ExecutableRange &range = library.executableRanges[rangeIndex];
            if (range.start == 0 || range.size < pattern.size) continue;

            const uintptr_t alignedStart = (range.start + 3U) & ~static_cast<uintptr_t>(3U);
            const uintptr_t last = range.start + range.size - pattern.size;
            for (uintptr_t cursor = alignedStart; cursor <= last; cursor += 4U) {
                if (!patternMatchesAt(cursor, pattern)) continue;

                const auto duplicate = std::find_if(matches.begin(), matches.end(),
                        [cursor](const PatternMatch &item) { return item.address == cursor; });
                if (duplicate == matches.end()) {
                    matches.push_back({cursor, &pattern});
                    if (matches.size() > 1) {
                        return {{}, matches.size()};
                    }
                }
            }
        }
    }

    if (matches.size() == 1) return {matches.front(), 1};
    return {{}, matches.size()};
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

struct ActiveHookCallGuard {
    ActiveHookCallGuard() {
        gActiveHookCalls.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ActiveHookCallGuard() {
        gActiveHookCalls.fetch_sub(1, std::memory_order_acq_rel);
    }
};

float gateHorizontalDistance(bool readyFinish, uint32_t side, float horizontalDistancePx) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp
            : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0 ? dpToPx(kStockBoundaryDp, densityDpi) : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

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
    if (now - last >= 1000 && gLastSwipeLogMs.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f guardPx=%.2f delayBeyondStock=%d gateReached=%d clamped=%d readyFinish=%d side=%u repairs=%llu",
                horizontalDistancePx, effectiveDistancePx,
                configuredDp, effectiveDp, densityDpi, userGatePx,
                stockBoundaryPx, stockGuardPx,
                delayBeyondStock ? 1 : 0, userGateReached ? 1 : 0,
                clamped ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    }

    return effectiveDistancePx;
}

void ensureWorkerStarted();

void onSwipeProcessHookV1(void *self, bool readyFinish, uint32_t side, const void *point, float horizontalDistancePx) {
    ActiveHookCallGuard activeGuard;
    // The inline hook can be inherited from the root spawner. Start the child-only
    // watchdog lazily on the first real launcher invocation if no loader callback
    // was delivered after specialization.
    if (isLauncherProcess()) ensureWorkerStarted();
    const float effectiveDistancePx = gateHorizontalDistance(readyFinish, side, horizontalDistancePx);
    const auto original = gOriginalOnSwipeProcessV1.load(std::memory_order_acquire);
    if (original != nullptr) original(self, readyFinish, side, point, effectiveDistancePx);
}

void onSwipeProcessHookV2(void *self, bool readyFinish, uint32_t side, float horizontalDistancePx, float pointX, float pointY) {
    ActiveHookCallGuard activeGuard;
    if (isLauncherProcess()) ensureWorkerStarted();
    const float effectiveDistancePx = gateHorizontalDistance(readyFinish, side, horizontalDistancePx);
    const auto original = gOriginalOnSwipeProcessV2.load(std::memory_order_acquire);
    if (original != nullptr) original(self, readyFinish, side, effectiveDistancePx, pointX, pointY);
}

bool waitForHookIdle() {
    const int64_t deadline = monotonicMs() + kHookIdleWaitMs;
    while (gActiveHookCalls.load(std::memory_order_acquire) != 0
            && monotonicMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return gActiveHookCalls.load(std::memory_order_acquire) == 0;
}

bool installFreshHookLocked(const LibraryInfo &library, uintptr_t target,
                            const PatternSpec &pattern, const char *source) {
    if (gHookFunction == nullptr || library.base == 0 || target == 0) return false;

    if (!patternMatchesAt(target, pattern)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_SCAN pattern changed before hook source=%s pattern=%s target=%p; refusing",
                source, pattern.name, reinterpret_cast<void *>(target));
        return false;
    }

    std::array<uint8_t, kHookProbeSize> before{};
    if (!readProbeHead(target, before)) return false;
    gExpectedOriginalHead = before;
    gExpectedOriginalHeadReady = true;
    gActivePatternName = pattern.name;

    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(target),
                                 (pattern.abi == SwipeProcessAbi::InlinePointFloatsV2 ? reinterpret_cast<void *>(onSwipeProcessHookV2) : reinterpret_cast<void *>(onSwipeProcessHookV1)), &backup);
    if (rc != 0 || backup == nullptr) {
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE hook_func failed source=%s rc=%d backup=%p target=%p pattern=%s",
                source, rc, backup, reinterpret_cast<void *>(target), pattern.name);
        return false;
    }

    std::array<uint8_t, kHookProbeSize> patchedHead{};
    if (!readProbeHead(target, patchedHead) || probeEqualsExpectedOriginal(patchedHead)) {
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH hook_func returned success but entry is not patched source=%s target=%p",
                source, reinterpret_cast<void *>(target));
        return false;
    }

    gOriginalOnSwipeProcessV1.store(nullptr, std::memory_order_release);
    gOriginalOnSwipeProcessV2.store(nullptr, std::memory_order_release);
    if (pattern.abi == SwipeProcessAbi::InlinePointFloatsV2) gOriginalOnSwipeProcessV2.store(reinterpret_cast<OnSwipeProcessFnV2>(backup), std::memory_order_release);
    else gOriginalOnSwipeProcessV1.store(reinterpret_cast<OnSwipeProcessFnV1>(backup), std::memory_order_release);
    gInstalledPatchHead = patchedHead;
    gInstalledPatchHeadReady = true;
    gHookedBase.store(library.base, std::memory_order_release);
    gHookedTarget.store(target, std::memory_order_release);
    gHookInstalled.store(true, std::memory_order_release);
    gHookHealthState.store(1, std::memory_order_release);
    gLastHealthyLogMs.store(monotonicMs(), std::memory_order_relaxed);

    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0
            ? kStockBoundaryDp
            : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const uintptr_t resolvedOffset = target - library.base;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE hook installed source=%s pattern=%s base=%p target=%p resolvedOffset=0x%zx referenceOffset=0x%zx configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f patchHead=%s repairs=%llu unhookRepair=1",
            source, pattern.name, reinterpret_cast<void *>(library.base),
            reinterpret_cast<void *>(target), static_cast<size_t>(resolvedOffset),
            static_cast<size_t>(kReferenceOnSwipeProcessOffset), configuredDp, effectiveDp, densityDpi,
            densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f,
            probeHex(patchedHead).c_str(),
            static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    return true;
}

bool repairRestoredHookLocked(const LibraryInfo &library, uintptr_t target, const char *source,
                              const std::array<uint8_t, kHookProbeSize> &currentHead) {
    const int64_t now = monotonicMs();
    const int64_t lastAttempt = gLastRepairAttemptMs.load(std::memory_order_relaxed);
    if (now - lastAttempt < kRepairCooldownMs) return false;
    gLastRepairAttemptMs.store(now, std::memory_order_relaxed);
    gHookHealthState.store(4, std::memory_order_release);
    gHookInstalled.store(false, std::memory_order_release);

    logLine(ANDROID_LOG_WARN,
            "HOOK_HEALTH original bytes restored source=%s base=%p target=%p pattern=%s currentHead=%s oldPatchHead=%s activeCalls=%u; starting unhook+rehook repair",
            source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(target),
            gActivePatternName, probeHex(currentHead).c_str(),
            gInstalledPatchHeadReady ? probeHex(gInstalledPatchHead).c_str() : "<none>",
            gActiveHookCalls.load(std::memory_order_acquire));

    if (gUnhookFunction == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH repair unavailable: LSPosed unhook_func is null");
        return false;
    }

    if (!waitForHookIdle()) {
        logLine(ANDROID_LOG_WARN,
                "HOOK_HEALTH repair deferred: hook still active after %lldms activeCalls=%u",
                static_cast<long long>(kHookIdleWaitMs),
                gActiveHookCalls.load(std::memory_order_acquire));
        return false;
    }

    gOriginalOnSwipeProcessV1.store(nullptr, std::memory_order_release);
    gOriginalOnSwipeProcessV2.store(nullptr, std::memory_order_release);
    const int unhookRc = gUnhookFunction(reinterpret_cast<void *>(target));

    std::array<uint8_t, kHookProbeSize> afterUnhook{};
    if (!readProbeHead(target, afterUnhook)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH repair failed to read target after unhook rc=%d target=%p",
                unhookRc, reinterpret_cast<void *>(target));
        return false;
    }

    logLine(unhookRc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
            "HOOK_HEALTH unhook result rc=%d target=%p headAfterUnhook=%s",
            unhookRc, reinterpret_cast<void *>(target), probeHex(afterUnhook).c_str());

    if (!probeEqualsExpectedOriginal(afterUnhook)) {
        gHookHealthState.store(3, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH repair aborted: entry became foreign after unhook head=%s expected=%s",
                probeHex(afterUnhook).c_str(), probeHex(gExpectedOriginalHead).c_str());
        return false;
    }

    const PatternSpec *activePattern = nullptr;
    for (const PatternSpec &pattern : kOnSwipeProcessPatterns) {
        if (std::strcmp(pattern.name, gActivePatternName) == 0) {
            activePattern = &pattern;
            break;
        }
    }
    if (activePattern == nullptr || !patternMatchesAt(target, *activePattern)) {
        gHookHealthState.store(3, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH repair aborted: active pattern no longer matches target=%p pattern=%s",
                reinterpret_cast<void *>(target), gActivePatternName);
        return false;
    }

    gInstalledPatchHeadReady = false;
    if (!installFreshHookLocked(library, target, *activePattern, "repair-after-unhook")) {
        gHookHealthState.store(2, std::memory_order_release);
        return false;
    }

    const uint64_t repairs = gRepairCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    logLine(ANDROID_LOG_INFO,
            "HOOK_HEALTH repaired successfully target=%p repairCount=%llu",
            reinterpret_cast<void *>(target), static_cast<unsigned long long>(repairs));
    return true;
}

void resetTrackedHookForRemapLocked(uintptr_t newBase) {
    const uintptr_t oldBase = gHookedBase.load(std::memory_order_acquire);
    const uintptr_t oldTarget = gHookedTarget.load(std::memory_order_acquire);
    if (oldTarget != 0) {
        logLine(ANDROID_LOG_WARN,
                "HOOK_HEALTH launcher mapping changed oldBase=%p oldTarget=%p newBase=%p; rescanning executable segments",
                reinterpret_cast<void *>(oldBase), reinterpret_cast<void *>(oldTarget),
                reinterpret_cast<void *>(newBase));
    }
    gOriginalOnSwipeProcessV1.store(nullptr, std::memory_order_release);
    gOriginalOnSwipeProcessV2.store(nullptr, std::memory_order_release);
    gHookedBase.store(0, std::memory_order_release);
    gHookedTarget.store(0, std::memory_order_release);
    gInstalledPatchHeadReady = false;
    gExpectedOriginalHeadReady = false;
    gActivePatternName = "<none>";
}

bool ensureHookLocked(const LibraryInfo &library, const char *source) {
    if (library.base == 0 || library.executableRangeCount == 0 || gHookFunction == nullptr) {
        return false;
    }

    const uintptr_t trackedBase = gHookedBase.load(std::memory_order_acquire);
    const uintptr_t trackedTarget = gHookedTarget.load(std::memory_order_acquire);

    if (trackedBase == library.base && trackedTarget != 0) {
        std::array<uint8_t, kHookProbeSize> currentHead{};
        if (!readProbeHead(trackedTarget, currentHead)) return false;

        if (gInstalledPatchHeadReady && probeEquals(currentHead, gInstalledPatchHead)) {
            gHookInstalled.store(true, std::memory_order_release);
            gHookHealthState.store(1, std::memory_order_release);
            const int64_t now = monotonicMs();
            int64_t last = gLastHealthyLogMs.load(std::memory_order_relaxed);
            if (now - last >= kHealthyLogIntervalMs
                    && gLastHealthyLogMs.compare_exchange_strong(
                            last, now, std::memory_order_relaxed)) {
                logLine(ANDROID_LOG_INFO,
                        "HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s configuredDp=%d repairs=%llu",
                        source, reinterpret_cast<void *>(library.base),
                        reinterpret_cast<void *>(trackedTarget), gActivePatternName,
                        readThresholdDp(),
                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
            }
            return true;
        }

        if (probeEqualsExpectedOriginal(currentHead)) {
            gHookHealthState.store(2, std::memory_order_release);
            return repairRestoredHookLocked(library, trackedTarget, source, currentHead);
        }

        const int previousState = gHookHealthState.exchange(3, std::memory_order_acq_rel);
        gHookInstalled.store(false, std::memory_order_release);
        if (previousState != 3) {
            logLine(ANDROID_LOG_ERROR,
                    "HOOK_HEALTH foreign patch detected source=%s base=%p target=%p pattern=%s head=%s; refusing unsafe repair",
                    source, reinterpret_cast<void *>(library.base),
                    reinterpret_cast<void *>(trackedTarget), gActivePatternName,
                    probeHex(currentHead).c_str());
        }
        return false;
    }

    if (trackedBase != 0 || trackedTarget != 0) {
        resetTrackedHookForRemapLocked(library.base);
    }

    const int64_t now = monotonicMs();
    const bool forceScan = std::strcmp(source, "loader-callback") == 0
            || std::strcmp(source, "native-init-backfill") == 0;
    const int64_t lastScan = gLastPatternScanMs.load(std::memory_order_relaxed);
    if (!forceScan && now - lastScan < kPatternRescanIntervalMs) return false;
    gLastPatternScanMs.store(now, std::memory_order_relaxed);

    const TargetResolution resolution = resolveOnSwipeProcessTarget(library);
    if (resolution.uniqueCandidates != 1 || resolution.match.address == 0
            || resolution.match.pattern == nullptr) {
        gHookInstalled.store(false, std::memory_order_release);
        gHookHealthState.store(3, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_SCAN install refused source=%s candidates=%zu base=%p execRanges=%zu referenceOffset=0x%zx; no unique validated target",
                source, resolution.uniqueCandidates, reinterpret_cast<void *>(library.base),
                library.executableRangeCount, static_cast<size_t>(kReferenceOnSwipeProcessOffset));
        return false;
    }

    const uintptr_t target = resolution.match.address;
    const uintptr_t resolvedOffset = target - library.base;
    const intptr_t delta = static_cast<intptr_t>(resolvedOffset)
            - static_cast<intptr_t>(kReferenceOnSwipeProcessOffset);
    logLine(ANDROID_LOG_INFO,
            "HOOK_SCAN resolved source=%s pattern=%s target=%p resolvedOffset=0x%zx referenceOffset=0x%zx delta=%lld execRanges=%zu",
            source, resolution.match.pattern->name, reinterpret_cast<void *>(target),
            static_cast<size_t>(resolvedOffset), static_cast<size_t>(kReferenceOnSwipeProcessOffset),
            static_cast<long long>(delta), library.executableRangeCount);

    return installFreshHookLocked(library, target, *resolution.match.pattern, source);
}

bool ensureHook(const LibraryInfo &library, const char *source) {
    std::lock_guard<std::mutex> lock(gHookMutex);
    return ensureHookLocked(library, source);
}

void hookWatchdogWorker() {
    int missingPolls = 0;
    while (isTargetProcess()) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            missingPolls = 0;
            ensureHook(library, "watchdog");
        } else {
            ++missingPolls;
            if (missingPolls == 12) {
                logLine(ANDROID_LOG_WARN,
                        "HOOK_HEALTH %s absent for ~%lldms; waiting for remap",
                        kTargetLibrary,
                        static_cast<long long>(missingPolls * kHookHealthIntervalMs));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kHookHealthIntervalMs));
    }
}

void ensureWorkerStarted() {
    if (!isTargetProcess()) return;
    bool expected = false;
    if (!gWorkerStarted.compare_exchange_strong(expected, true)) return;
    std::thread(hookWatchdogWorker).detach();
}

void onLibraryLoaded(const char *name, void *) {
    if (!isHyosSpawnerProcessFamily() || name == nullptr) return;
    if (std::strstr(name, kTargetLibrary) != nullptr) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            ensureHook(library, "loader-callback");
            if (isLauncherProcess()) ensureWorkerStarted();
        }
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    const std::string executable = readExecutable();
    const std::string processName = readProcessName();
    const bool entriesReady = entries != nullptr;
    const bool hookReady = entriesReady && entries->hook_func != nullptr;
    const bool hyosProcess = executable == kSpawnerPath;
    const bool launcherProcess = processName == kTargetPackage;

    // Always emit the preflight result before any rejection. This is intentionally
    // independent from the app-side log recording level so LSPosed exports can
    // distinguish lifecycle rejection from Pattern/ABI failures.
    __android_log_print(
            ANDROID_LOG_INFO, kTag,
            "DP_GATE native_init checks entries=%d hook=%d unhook=%d hyosExe=%d launcherCmdline=%d api=%u exe=%s process=%s",
            entriesReady ? 1 : 0, hookReady ? 1 : 0,
            entriesReady && entries->unhook_func != nullptr ? 1 : 0,
            hyosProcess ? 1 : 0, launcherProcess ? 1 : 0,
            entriesReady ? entries->version : 0, executable.c_str(), processName.c_str());

    if (!entriesReady || !hookReady) {
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE native_init rejected: LSPosed native hook backend unavailable entries=%p hook_func=%p",
                static_cast<const void *>(entries),
                entriesReady ? reinterpret_cast<void *>(entries->hook_func) : nullptr);
        return nullptr;
    }
    if (!hyosProcess) {
        logLine(ANDROID_LOG_WARN,
                "DP_GATE native_init rejected non-HYOS process exe=%s process=%s",
                executable.c_str(), processName.c_str());
        return nullptr;
    }

    gHookFunction = entries->hook_func;
    gUnhookFunction = entries->unhook_func;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE native_init accepted api=%u exe=%s process=%s launcherCmdline=%d hook_func=%p unhook_func=%p watchdog=%lldms resolver=masked-pattern-scan repair=unhook+rehook",
            entries->version, executable.c_str(), processName.c_str(), launcherProcess ? 1 : 0,
            reinterpret_cast<void *>(entries->hook_func),
            reinterpret_cast<void *>(entries->unhook_func),
            static_cast<long long>(kHookHealthIntervalMs));

    // HyperOS may map libapp_launcher.so before LSPosed calls native_init. Backfill
    // the already-loaded image so the first hook can be installed in the spawner
    // and inherited by the final launcher child instead of waiting for a callback
    // that may never arrive.
    const LibraryInfo library = findLauncherLibrary();
    if (library.base != 0) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE native_init backfill found %s base=%p process=%s",
                kTargetLibrary, reinterpret_cast<void *>(library.base), processName.c_str());
        ensureHook(library, "native-init-backfill");
    } else {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE native_init waiting for %s process=%s",
                kTargetLibrary, processName.c_str());
    }

    // Never start a worker thread in the root spawner: it would disappear across
    // fork while the atomic started flag remained inherited by the child.
    if (launcherProcess) ensureWorkerStarted();
    return onLibraryLoaded;
}
