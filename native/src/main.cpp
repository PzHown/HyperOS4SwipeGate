#include "control_channel.h"
#include "native_api.h"
#include "swipe_semantic_resolver.h"

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <dlfcn.h>
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

extern "C" {
__attribute__((visibility("hidden"))) extern void *gSwipeGateOriginalOnSwipeProcess;
__attribute__((visibility("hidden"))) void swipegate_on_swipe_process_hook();
__attribute__((visibility("hidden"))) float swipegate_hook_enter_and_gate(
        uint32_t readyFinish, uint32_t side, float horizontalDistancePx);
__attribute__((visibility("hidden"))) void swipegate_hook_exit();
__attribute__((visibility("hidden"))) extern void *gSwipeGateOriginalOnSwipeStart;
__attribute__((visibility("hidden"))) extern void *gSwipeGateOriginalOnBackInvoke;
__attribute__((visibility("hidden"))) extern void *gSwipeGateOriginalOnBackCancelled;
__attribute__((visibility("hidden"))) void swipegate_on_swipe_start_hook();
__attribute__((visibility("hidden"))) void swipegate_on_back_invoke_hook();
__attribute__((visibility("hidden"))) void swipegate_on_back_cancelled_hook();
__attribute__((visibility("hidden"))) void swipegate_haptic_on_swipe_start();
__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke();
__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_cancelled();
}

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kSpawnerPath = "/system_ext/bin/hyos_spawner";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr const char *kHapticLibrary = "libhyper_os_background_tasks_public.so";
constexpr const char *kHapticSymbol = "HapticFeedback_perform_ext_haptic_feedback";
constexpr const char *kStandardHapticSymbol = "HapticFeedback_perform_haptic_feedback";
constexpr int32_t kReadyHapticConstant = 27;  // Android SEGMENT_FREQUENT_TICK: deliberately very light
constexpr int32_t kCommitHapticConstant = 0;  // validated HyperOS ext feedback
constexpr uintptr_t kPointerTagMask = 0x00ffffffffffffffull;
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

constexpr uintptr_t kReferenceOnSwipeProcessOffset = 0x816fc4;

// Exact patterns remain authoritative compatibility profiles for builds we have
// already validated. The semantic resolver is allowed to corroborate them, but
// an experimental semantic disagreement must never regress a known-good build.
// On an unknown build with no exact profile, semantic resolution may take over
// only when it proves exactly one candidate.
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

struct PatternSpec {
    const char *name;
    const uint8_t *bytes;
    const uint8_t *mask;
    size_t size;
};

constexpr PatternSpec kOnSwipeProcessPatterns[] = {
        {"8.01.02.5459-v1", kOnSwipeProcessPatternV1, kOnSwipeProcessMaskV1,
         sizeof(kOnSwipeProcessPatternV1)},
        {"8.01.02.6174-v2", kOnSwipeProcessPatternV2, kOnSwipeProcessMaskV2,
         sizeof(kOnSwipeProcessPatternV2)},
};

constexpr PatternSpec kSemanticOnSwipeProcessPattern = {
        "semantic-motion-graph", nullptr, nullptr, 0,
};

constexpr uint8_t kOnSwipeStartPatternV1[] = {
        0xff,0x03,0x05,0xd1,0xe8,0x6b,0x00,0xfd,0xfd,0x7b,0x0e,0xa9,0xfc,0x6f,0x0f,0xa9,
        0xfa,0x67,0x10,0xa9,0xf8,0x5f,0x11,0xa9,0xf6,0x57,0x12,0xa9,0xf4,0x4f,0x13,0xa9,
        0xfd,0x83,0x03,0x91,0x37,0x59,0x00,0xd0,
};
constexpr uint8_t kOnSwipeStartPatternV2[] = {
        0xff,0x43,0x03,0xd1,0xe8,0x43,0x00,0xfd,0xfd,0x7b,0x09,0xa9,0xf8,0x5f,0x0a,0xa9,
        0xf6,0x57,0x0b,0xa9,0xf4,0x4f,0x0c,0xa9,0xfd,0x43,0x02,0x91,0x77,0x41,0x00,0x90,
        0xf3,0x03,0x02,0x2a,0xf5,0x03,0x01,0xaa,
};
constexpr uint8_t kOnBackInvokePatternV1[] = {
        0xff,0x83,0x05,0xd1,0xfd,0x7b,0x10,0xa9,0xfc,0x6f,0x11,0xa9,0xfa,0x67,0x12,0xa9,
        0xf8,0x5f,0x13,0xa9,0xf6,0x57,0x14,0xa9,0xf4,0x4f,0x15,0xa9,0xfd,0x03,0x04,0x91,
        0xf9,0x03,0x00,0xaa,0x20,0x00,0x80,0x52,
};
constexpr uint8_t kOnBackInvokePatternV2[] = {
        0xff,0x83,0x04,0xd1,0xfd,0x7b,0x0c,0xa9,0xfc,0x6f,0x0d,0xa9,0xfa,0x67,0x0e,0xa9,
        0xf8,0x5f,0x0f,0xa9,0xf6,0x57,0x10,0xa9,0xf4,0x4f,0x11,0xa9,0xfd,0x03,0x03,0x91,
        0xfa,0x03,0x00,0xaa,0x20,0x00,0x80,0x52,
};
constexpr uint8_t kOnBackCancelledPatternV1[] = {
        0xff,0x03,0x04,0xd1,0xfd,0x7b,0x0c,0xa9,0xf8,0x5f,0x0d,0xa9,0xf6,0x57,0x0e,0xa9,
        0xf4,0x4f,0x0f,0xa9,0xfd,0x03,0x03,0x91,0x14,0x04,0x40,0xf9,0xf3,0x03,0x00,0xaa,
        0x74,0x1a,0x00,0xb4,0xe8,0xe3,0x00,0x91,
};
constexpr uint8_t kOnBackCancelledPatternV2[] = {
        0xff,0xc3,0x02,0xd1,0xfd,0x7b,0x08,0xa9,0xf6,0x57,0x09,0xa9,0xf4,0x4f,0x0a,0xa9,
        0xfd,0x03,0x02,0x91,0x14,0x04,0x40,0xf9,0xf3,0x03,0x00,0xaa,0xe0,0x03,0x14,0xaa,
        0x6a,0x00,0x00,0x94,0xa0,0x05,0x00,0xb4,
};

static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));
static_assert(sizeof(kOnSwipeProcessPatternV1) >= kHookProbeSize);

HookFunType gHookFunction = nullptr;
UnhookFunType gUnhookFunction = nullptr;

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
std::atomic<int> gHookHealthState{0};
std::atomic<uint32_t> gActiveHookCalls{0};
std::atomic<uint64_t> gRepairCount{0};
std::atomic<uint64_t> gClampedCount{0};
std::atomic<uint64_t> gPassthroughCount{0};
std::atomic<void *> gOriginalHapticFeedback{nullptr};
std::atomic<void *> gStandardHapticFeedback{nullptr};
std::atomic<uintptr_t> gCapturedHapticArc{0};
std::atomic<bool> gHapticCaptureHookInstalled{false};
std::atomic<uintptr_t> gAuxGestureScannedBase{0};
std::atomic<bool> gGestureActive{false};
std::atomic<bool> gReadyHapticLatched{false};
std::atomic<bool> gHapticUnavailableLogged{false};
std::atomic<uint64_t> gHapticHookRepairCount{0};
std::atomic<uint64_t> gHapticCaptureHookRepairCount{0};

std::mutex gHookMutex;
std::array<uint8_t, kHookProbeSize> gInstalledPatchHead{};
std::array<uint8_t, kHookProbeSize> gExpectedOriginalHead{};
bool gInstalledPatchHeadReady = false;
bool gExpectedOriginalHeadReady = false;
const char *gActivePatternName = "<none>";
const char *gActiveResolverDetail = "<none>";

struct AuxHookHealth {
    uintptr_t target = 0;
    std::array<uint8_t, kHookProbeSize> originalHead{};
    std::array<uint8_t, kHookProbeSize> patchHead{};
    bool originalReady = false;
    bool patchReady = false;
};

AuxHookHealth gBackInvokeHapticHookHealth{};
AuxHookHealth gHapticCaptureHookHealth{};

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
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0) continue;
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
    if (address == 0 || pattern.bytes == nullptr || pattern.mask == nullptr || pattern.size == 0) return false;
    const auto *data = reinterpret_cast<const uint8_t *>(address);
    for (size_t i = 0; i < pattern.size; ++i) {
        if (((data[i] ^ pattern.bytes[i]) & pattern.mask[i]) != 0) return false;
    }
    return true;
}

bool isSemanticPattern(const PatternSpec &pattern) {
    return pattern.bytes == nullptr && pattern.mask == nullptr && pattern.size == 0;
}

bool validateResolvedTarget(const LibraryInfo &library, uintptr_t target,
                            const PatternSpec &pattern) {
    if (isSemanticPattern(pattern)) return swipe_semantic::ValidateTarget(library.base, target);
    return patternMatchesAt(target, pattern);
}

struct PatternMatch {
    uintptr_t address = 0;
    const PatternSpec *pattern = nullptr;
    const char *detail = nullptr;
};

struct TargetResolution {
    PatternMatch match{};
    size_t uniqueCandidates = 0;
};

TargetResolution resolveExactOnSwipeProcessTarget(const LibraryInfo &library) {
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
                    matches.push_back({cursor, &pattern, nullptr});
                    if (matches.size() > 1) return {{}, matches.size()};
                }
            }
        }
    }

    if (matches.size() == 1) return {matches.front(), 1};
    return {{}, matches.size()};
}

TargetResolution resolveOnSwipeProcessTarget(const LibraryInfo &library) {
    const swipe_semantic::Resolution semantic = swipe_semantic::Resolve(library.base);
    const TargetResolution exact = resolveExactOnSwipeProcessTarget(library);

    // A unique exact profile is an already validated compatibility contract.
    // Semantic resolution may corroborate it, but cannot disable or replace it.
    // This keeps 5459/6174 safe while the semantic matcher evolves on real devices.
    if (exact.uniqueCandidates == 1 && exact.match.address != 0 && exact.match.pattern != nullptr) {
        if (semantic.candidate_count == 1 && semantic.target != 0) {
            const bool agrees = semantic.target == exact.match.address;
            return {{exact.match.address, exact.match.pattern,
                     agrees ? "semantic-corroborated" : "semantic-conflict-exact-authoritative"}, 1};
        }
        return {{exact.match.address, exact.match.pattern, "exact-authoritative"}, 1};
    }

    // Multiple exact hits are unsafe even if semantic happens to choose one.
    if (exact.uniqueCandidates > 1) return {{}, exact.uniqueCandidates};

    // Unknown launcher build: semantic resolution is allowed to take over only
    // after proving exactly one candidate. Otherwise fail closed.
    if (semantic.candidate_count == 1 && semantic.target != 0) {
        return {{semantic.target, &kSemanticOnSwipeProcessPattern,
                 swipe_semantic::FrameShapeName(semantic.shape)}, 1};
    }
    return {{}, semantic.candidate_count};
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
        logLine(ANDROID_LOG_ERROR, "DP_GATE density unavailable; custom delay disabled, stock passthrough");
    } else {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE density resolved: %ddpi source=%s pxPerDp=%.3f stock88dp=%.2fpx",
                densityDpi, source, static_cast<float>(densityDpi) / 160.0f,
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


using HapticFeedbackFn = void (*)(void *, int32_t);

void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    if (storage != nullptr) {
        uintptr_t arc = 0;
        std::memcpy(&arc, storage, sizeof(arc));
        arc &= kPointerTagMask;
        if (arc >= 0x10000u) {
            gCapturedHapticArc.store(arc, std::memory_order_release);
            gHapticUnavailableLogged.store(false, std::memory_order_release);
        }
    }
    const auto original = reinterpret_cast<HapticFeedbackFn>(gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original != nullptr) original(storage, constant);
}

void *resolveHapticSymbol(const char *symbol) {
    if (symbol == nullptr) return nullptr;
    void *target = dlsym(RTLD_DEFAULT, symbol);
    if (target != nullptr) return target;
    void *handle = dlopen(kHapticLibrary, RTLD_NOW | RTLD_NOLOAD);
    return handle == nullptr ? nullptr : dlsym(handle, symbol);
}

bool ensureHapticCaptureHook() {
    void *standard = resolveHapticSymbol(kStandardHapticSymbol);
    if (standard != nullptr) gStandardHapticFeedback.store(standard, std::memory_order_release);
    if (gHookFunction == nullptr) return false;

    void *targetPointer = resolveHapticSymbol(kHapticSymbol);
    if (targetPointer == nullptr) return false;
    const uintptr_t target = reinterpret_cast<uintptr_t>(targetPointer);
    AuxHookHealth &health = gHapticCaptureHookHealth;

    if (health.target != 0 && health.target != target) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC capture target changed old=%p new=%p; resetting cached runtime",
                reinterpret_cast<void *>(health.target), targetPointer);
        health = AuxHookHealth{};
        gHapticCaptureHookInstalled.store(false, std::memory_order_release);
        gOriginalHapticFeedback.store(nullptr, std::memory_order_release);
        gCapturedHapticArc.store(0, std::memory_order_release);
    }

    if (health.target == target && health.patchReady) {
        std::array<uint8_t, kHookProbeSize> current{};
        if (!readProbeHead(target, current)) return false;
        if (probeEquals(current, health.patchHead)) {
            gHapticCaptureHookInstalled.store(true, std::memory_order_release);
            return true;
        }
        if (!health.originalReady || !probeEquals(current, health.originalHead)) {
            gHapticCaptureHookInstalled.store(false, std::memory_order_release);
            gCapturedHapticArc.store(0, std::memory_order_release);
            logLine(ANDROID_LOG_ERROR,
                    "HAPTIC capture hook foreign patch target=%p current=%s expectedPatch=%s",
                    targetPointer, probeHex(current).c_str(), probeHex(health.patchHead).c_str());
            return false;
        }
        if (gUnhookFunction == nullptr) return false;

        gHapticCaptureHookInstalled.store(false, std::memory_order_release);
        gOriginalHapticFeedback.store(nullptr, std::memory_order_release);
        gCapturedHapticArc.store(0, std::memory_order_release);
        const int unhookRc = gUnhookFunction(targetPointer);
        std::array<uint8_t, kHookProbeSize> afterUnhook{};
        if (!readProbeHead(target, afterUnhook)
                || !probeEquals(afterUnhook, health.originalHead)) {
            logLine(ANDROID_LOG_ERROR,
                    "HAPTIC capture repair unhook failed rc=%d target=%p",
                    unhookRc, targetPointer);
            return false;
        }
        health.patchReady = false;
        const uint64_t repairs = gHapticCaptureHookRepairCount.fetch_add(
                1, std::memory_order_acq_rel) + 1;
        logLine(ANDROID_LOG_WARN,
                "HAPTIC capture hook restored by runtime; rehooking repair=%llu",
                static_cast<unsigned long long>(repairs));
    }

    std::array<uint8_t, kHookProbeSize> original{};
    if (!readProbeHead(target, original)) return false;
    if (!health.originalReady) {
        health.target = target;
        health.originalHead = original;
        health.originalReady = true;
    } else if (!probeEquals(original, health.originalHead)) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC capture hook install refused target=%p current=%s expectedOriginal=%s",
                targetPointer, probeHex(original).c_str(), probeHex(health.originalHead).c_str());
        return false;
    }

    void *backup = nullptr;
    const int rc = gHookFunction(
            targetPointer, reinterpret_cast<void *>(hapticFeedbackCaptureHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC capture hook install failed rc=%d target=%p", rc, targetPointer);
        return false;
    }

    std::array<uint8_t, kHookProbeSize> patched{};
    if (!readProbeHead(target, patched) || probeEquals(patched, health.originalHead)) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC capture hook did not patch target=%p", targetPointer);
        return false;
    }
    health.patchHead = patched;
    health.patchReady = true;
    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC capture hook ready target=%p lightTarget=%p repairs=%llu",
            targetPointer, standard,
            static_cast<unsigned long long>(
                    gHapticCaptureHookRepairCount.load(std::memory_order_relaxed)));
    return true;
}

bool performReturnHaptic(const char *stage, bool light) {
    if (swipegate_control_haptic_enabled() != 1) return false;
    const auto original = reinterpret_cast<HapticFeedbackFn>(gOriginalHapticFeedback.load(std::memory_order_acquire));
    const uintptr_t arc = gCapturedHapticArc.load(std::memory_order_acquire);
    if (original == nullptr || arc < 0x10000u) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            logLine(ANDROID_LOG_WARN, "HAPTIC skipped stage=%s reason=runtime-not-captured", stage == nullptr ? "unknown" : stage);
        }
        return false;
    }
    void *storage = reinterpret_cast<void *>(arc);
    if (light) {
        const auto standard = reinterpret_cast<HapticFeedbackFn>(gStandardHapticFeedback.load(std::memory_order_acquire));
        if (standard != nullptr) {
            standard(&storage, kReadyHapticConstant);
            logLine(ANDROID_LOG_INFO, "HAPTIC feedback stage=%s kind=light constant=%d", stage == nullptr ? "unknown" : stage, kReadyHapticConstant);
            return true;
        }
        logLine(ANDROID_LOG_WARN, "HAPTIC light feedback unavailable; falling back to ext feedback");
    }
    original(&storage, kCommitHapticConstant);
    logLine(ANDROID_LOG_INFO, "HAPTIC feedback stage=%s kind=%s constant=%d", stage == nullptr ? "unknown" : stage, light ? "fallback" : "commit", kCommitHapticConstant);
    return true;
}

uintptr_t resolveUniqueAuxPattern(const LibraryInfo &library, const uint8_t *bytes, size_t size) {
    if (bytes == nullptr || size == 0) return 0;
    uintptr_t found = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < size) continue;
        const uintptr_t start = (range.start + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - size;
        for (uintptr_t cursor = start; cursor <= last; cursor += 4u) {
            if (std::memcmp(reinterpret_cast<const void *>(cursor), bytes, size) != 0) continue;
            if (found != 0 && found != cursor) return 0;
            found = cursor;
        }
    }
    return found;
}

uintptr_t resolveAuxForActiveProfile(const LibraryInfo &library, const uint8_t *v1, size_t v1Size, const uint8_t *v2, size_t v2Size) {
    if (std::strcmp(gActivePatternName, "8.01.02.5459-v1") == 0) return resolveUniqueAuxPattern(library, v1, v1Size);
    if (std::strcmp(gActivePatternName, "8.01.02.6174-v2") == 0) return resolveUniqueAuxPattern(library, v2, v2Size);
    return 0;
}

bool installAuxGestureHapticHooks(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;
    AuxHookHealth &health = gBackInvokeHapticHookHealth;
    const uintptr_t trackedBase = gAuxGestureScannedBase.load(std::memory_order_acquire);
    if (trackedBase != 0 && trackedBase != library.base) {
        health = AuxHookHealth{};
        gAuxGestureScannedBase.store(0, std::memory_order_release);
        __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);
    }

    uintptr_t invokeTarget = health.target;
    if (invokeTarget == 0) {
        invokeTarget = resolveAuxForActiveProfile(
                library, kOnBackInvokePatternV1, sizeof(kOnBackInvokePatternV1),
                kOnBackInvokePatternV2, sizeof(kOnBackInvokePatternV2));
        if (invokeTarget == 0) {
            logLine(ANDROID_LOG_WARN, "HAPTIC commit hook unresolved profile=%s", gActivePatternName);
            return false;
        }
    }

    if (health.target == invokeTarget && health.patchReady) {
        std::array<uint8_t, kHookProbeSize> current{};
        if (!readProbeHead(invokeTarget, current)) return false;
        if (probeEquals(current, health.patchHead)) return true;
        if (!health.originalReady || !probeEquals(current, health.originalHead)) {
            logLine(ANDROID_LOG_ERROR,
                    "HAPTIC commit hook foreign patch target=%p current=%s expectedPatch=%s",
                    reinterpret_cast<void *>(invokeTarget), probeHex(current).c_str(), probeHex(health.patchHead).c_str());
            return false;
        }
        if (gUnhookFunction == nullptr) return false;
        __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);
        const int unhookRc = gUnhookFunction(reinterpret_cast<void *>(invokeTarget));
        std::array<uint8_t, kHookProbeSize> afterUnhook{};
        if (!readProbeHead(invokeTarget, afterUnhook) || !probeEquals(afterUnhook, health.originalHead)) {
            logLine(ANDROID_LOG_ERROR, "HAPTIC commit repair unhook failed rc=%d target=%p", unhookRc, reinterpret_cast<void *>(invokeTarget));
            return false;
        }
        health.patchReady = false;
        const uint64_t repairs = gHapticHookRepairCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        logLine(ANDROID_LOG_WARN, "HAPTIC commit hook restored by runtime; rehooking repair=%llu",
                static_cast<unsigned long long>(repairs));
    }

    std::array<uint8_t, kHookProbeSize> original{};
    if (!readProbeHead(invokeTarget, original)) return false;
    if (!health.originalReady) {
        health.target = invokeTarget;
        health.originalHead = original;
        health.originalReady = true;
    }
    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(invokeTarget),
                                 reinterpret_cast<void *>(swipegate_on_back_invoke_hook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR, "HAPTIC commit hook install failed rc=%d target=%p", rc, reinterpret_cast<void *>(invokeTarget));
        return false;
    }
    __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, backup, __ATOMIC_RELEASE);
    std::array<uint8_t, kHookProbeSize> patched{};
    if (!readProbeHead(invokeTarget, patched) || probeEquals(patched, health.originalHead)) {
        __atomic_store_n(&gSwipeGateOriginalOnBackInvoke, nullptr, __ATOMIC_RELEASE);
        logLine(ANDROID_LOG_ERROR, "HAPTIC commit hook did not patch target=%p", reinterpret_cast<void *>(invokeTarget));
        return false;
    }
    health.patchHead = patched;
    health.patchReady = true;
    gAuxGestureScannedBase.store(library.base, std::memory_order_release);
    logLine(ANDROID_LOG_INFO, "HAPTIC commit hook ready profile=%s target=%p repairs=%llu",
            gActivePatternName, reinterpret_cast<void *>(invokeTarget),
            static_cast<unsigned long long>(gHapticHookRepairCount.load(std::memory_order_relaxed)));
    return true;
}

float gateHorizontalDistance(bool readyFinish, uint32_t side, float horizontalDistancePx) {
    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0 ? kStockBoundaryDp : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const float stockBoundaryPx = densityDpi > 0 ? dpToPx(kStockBoundaryDp, densityDpi) : 0.0f;
    const float stockGuardPx = stockBoundaryPx > 1.0f ? stockBoundaryPx - 1.0f : 0.0f;
    const float userGatePx = densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f;
    const float absDx = std::fabs(horizontalDistancePx);

    const bool delayBeyondStock = effectiveDp > kStockBoundaryDp && densityDpi > 0
            && stockGuardPx > 0.0f && userGatePx > stockBoundaryPx;
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
    if (now - last >= 1000 && gLastSwipeLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "DP_GATE rawDx=%.2f effectiveDx=%.2f configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f stockBoundaryPx=%.2f guardPx=%.2f delayBeyondStock=%d gateReached=%d clamped=%d readyFinish=%d side=%u repairs=%llu",
                horizontalDistancePx, effectiveDistancePx, configuredDp, effectiveDp, densityDpi,
                userGatePx, stockBoundaryPx, stockGuardPx, delayBeyondStock ? 1 : 0,
                userGateReached ? 1 : 0, clamped ? 1 : 0, readyFinish ? 1 : 0, side,
                static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    }
    return effectiveDistancePx;
}

void ensureWorkerStarted();

bool waitForHookIdle() {
    const int64_t deadline = monotonicMs() + kHookIdleWaitMs;
    while (gActiveHookCalls.load(std::memory_order_acquire) != 0 && monotonicMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return gActiveHookCalls.load(std::memory_order_acquire) == 0;
}

bool installFreshHookLocked(const LibraryInfo &library, uintptr_t target,
                            const PatternSpec &pattern, const char *source,
                            const char *resolverDetail) {
    if (gHookFunction == nullptr || library.base == 0 || target == 0) return false;

    if (!validateResolvedTarget(library, target, pattern)) {
        logLine(ANDROID_LOG_ERROR,
                "HOOK_SCAN target changed before hook source=%s resolver=%s detail=%s target=%p; refusing",
                source, pattern.name, resolverDetail == nullptr ? "<none>" : resolverDetail,
                reinterpret_cast<void *>(target));
        return false;
    }

    std::array<uint8_t, kHookProbeSize> before{};
    if (!readProbeHead(target, before)) return false;
    gExpectedOriginalHead = before;
    gExpectedOriginalHeadReady = true;
    gActivePatternName = pattern.name;
    gActiveResolverDetail = resolverDetail == nullptr ? "<none>" : resolverDetail;

    __atomic_store_n(&gSwipeGateOriginalOnSwipeProcess, nullptr, __ATOMIC_RELEASE);
    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(target),
                                 reinterpret_cast<void *>(swipegate_on_swipe_process_hook), &backup);
    if (rc != 0 || backup == nullptr) {
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "DP_GATE hook_func failed source=%s rc=%d backup=%p target=%p pattern=%s resolver=%s detail=%s",
                source, rc, backup, reinterpret_cast<void *>(target), pattern.name, pattern.name,
                gActiveResolverDetail);
        return false;
    }
    __atomic_store_n(&gSwipeGateOriginalOnSwipeProcess, backup, __ATOMIC_RELEASE);

    std::array<uint8_t, kHookProbeSize> patchedHead{};
    if (!readProbeHead(target, patchedHead) || probeEqualsExpectedOriginal(patchedHead)) {
        __atomic_store_n(&gSwipeGateOriginalOnSwipeProcess, nullptr, __ATOMIC_RELEASE);
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH hook_func returned success but entry is not patched source=%s target=%p",
                source, reinterpret_cast<void *>(target));
        return false;
    }

    gInstalledPatchHead = patchedHead;
    gInstalledPatchHeadReady = true;
    gHookedBase.store(library.base, std::memory_order_release);
    gHookedTarget.store(target, std::memory_order_release);
    gHookInstalled.store(true, std::memory_order_release);
    gHookHealthState.store(1, std::memory_order_release);
    gLastHealthyLogMs.store(monotonicMs(), std::memory_order_relaxed);

    const int configuredDp = readThresholdDp();
    const int effectiveDp = configuredDp == 0 ? kStockBoundaryDp : std::max(configuredDp, kStockBoundaryDp);
    const int densityDpi = effectiveDp > kStockBoundaryDp ? readDensityDpi() : 0;
    const uintptr_t resolvedOffset = target - library.base;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE hook installed source=%s pattern=%s resolver=%s detail=%s base=%p target=%p resolvedOffset=0x%zx referenceOffset=0x%zx configuredDp=%d effectiveDp=%d densityDpi=%d userGatePx=%.2f patchHead=%s repairs=%llu unhookRepair=1 abi=transparent-s0",
            source, pattern.name, pattern.name, gActiveResolverDetail,
            reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(target),
            static_cast<size_t>(resolvedOffset), static_cast<size_t>(kReferenceOnSwipeProcessOffset),
            configuredDp, effectiveDp, densityDpi,
            densityDpi > 0 ? dpToPx(effectiveDp, densityDpi) : 0.0f,
            probeHex(patchedHead).c_str(),
            static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)));
    return true;
}

const PatternSpec *findActivePattern() {
    if (std::strcmp(gActivePatternName, kSemanticOnSwipeProcessPattern.name) == 0) return &kSemanticOnSwipeProcessPattern;
    for (const PatternSpec &pattern : kOnSwipeProcessPatterns) {
        if (std::strcmp(pattern.name, gActivePatternName) == 0) return &pattern;
    }
    return nullptr;
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
            "HOOK_HEALTH original bytes restored source=%s base=%p target=%p pattern=%s resolver=%s detail=%s currentHead=%s oldPatchHead=%s activeCalls=%u; starting unhook+rehook repair",
            source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(target),
            gActivePatternName, gActivePatternName, gActiveResolverDetail, probeHex(currentHead).c_str(),
            gInstalledPatchHeadReady ? probeHex(gInstalledPatchHead).c_str() : "<none>",
            gActiveHookCalls.load(std::memory_order_acquire));

    if (gUnhookFunction == nullptr) {
        logLine(ANDROID_LOG_ERROR, "HOOK_HEALTH repair unavailable: LSPosed unhook_func is null");
        return false;
    }
    if (!waitForHookIdle()) {
        logLine(ANDROID_LOG_WARN,
                "HOOK_HEALTH repair deferred: hook still active after %lldms activeCalls=%u",
                static_cast<long long>(kHookIdleWaitMs), gActiveHookCalls.load(std::memory_order_acquire));
        return false;
    }

    __atomic_store_n(&gSwipeGateOriginalOnSwipeProcess, nullptr, __ATOMIC_RELEASE);
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

    const PatternSpec *activePattern = findActivePattern();
    if (activePattern == nullptr || !validateResolvedTarget(library, target, *activePattern)) {
        gHookHealthState.store(3, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_HEALTH repair aborted: active resolver no longer validates target=%p pattern=%s resolver=%s detail=%s",
                reinterpret_cast<void *>(target), gActivePatternName, gActivePatternName, gActiveResolverDetail);
        return false;
    }

    gInstalledPatchHeadReady = false;
    if (!installFreshHookLocked(library, target, *activePattern, "repair-after-unhook", gActiveResolverDetail)) {
        gHookHealthState.store(2, std::memory_order_release);
        return false;
    }

    const uint64_t repairs = gRepairCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    logLine(ANDROID_LOG_INFO, "HOOK_HEALTH repaired successfully target=%p repairCount=%llu",
            reinterpret_cast<void *>(target), static_cast<unsigned long long>(repairs));
    return true;
}

void resetTrackedHookForRemapLocked(uintptr_t newBase) {
    const uintptr_t oldBase = gHookedBase.load(std::memory_order_acquire);
    const uintptr_t oldTarget = gHookedTarget.load(std::memory_order_acquire);
    if (oldTarget != 0) {
        logLine(ANDROID_LOG_WARN,
                "HOOK_HEALTH launcher mapping changed oldBase=%p oldTarget=%p newBase=%p; rescanning executable segments",
                reinterpret_cast<void *>(oldBase), reinterpret_cast<void *>(oldTarget), reinterpret_cast<void *>(newBase));
    }
    __atomic_store_n(&gSwipeGateOriginalOnSwipeProcess, nullptr, __ATOMIC_RELEASE);
    gHookedBase.store(0, std::memory_order_release);
    gHookedTarget.store(0, std::memory_order_release);
    gInstalledPatchHeadReady = false;
    gExpectedOriginalHeadReady = false;
    gActivePatternName = "<none>";
    gActiveResolverDetail = "<none>";
}

bool ensureHookLocked(const LibraryInfo &library, const char *source) {
    if (library.base == 0 || library.executableRangeCount == 0 || gHookFunction == nullptr) return false;

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
                    && gLastHealthyLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
                logLine(ANDROID_LOG_INFO,
                        "HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu",
                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),
                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),
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
                    "HOOK_HEALTH foreign patch detected source=%s base=%p target=%p pattern=%s resolver=%s detail=%s head=%s; refusing unsafe repair",
                    source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),
                    gActivePatternName, gActivePatternName, gActiveResolverDetail, probeHex(currentHead).c_str());
        }
        return false;
    }

    if (trackedBase != 0 || trackedTarget != 0) resetTrackedHookForRemapLocked(library.base);

    const int64_t now = monotonicMs();
    const bool forceScan = std::strcmp(source, "loader-callback") == 0
            || std::strcmp(source, "native-init-backfill") == 0;
    const int64_t lastScan = gLastPatternScanMs.load(std::memory_order_relaxed);
    if (!forceScan && now - lastScan < kPatternRescanIntervalMs) return false;
    gLastPatternScanMs.store(now, std::memory_order_relaxed);

    const TargetResolution resolution = resolveOnSwipeProcessTarget(library);
    if (resolution.uniqueCandidates != 1 || resolution.match.address == 0 || resolution.match.pattern == nullptr) {
        gHookInstalled.store(false, std::memory_order_release);
        gHookHealthState.store(3, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "HOOK_SCAN install refused source=%s candidates=%zu base=%p execRanges=%zu referenceOffset=0x%zx; semantic/exact resolvers found no unique validated target",
                source, resolution.uniqueCandidates, reinterpret_cast<void *>(library.base),
                library.executableRangeCount, static_cast<size_t>(kReferenceOnSwipeProcessOffset));
        return false;
    }

    const uintptr_t target = resolution.match.address;
    const uintptr_t resolvedOffset = target - library.base;
    const intptr_t delta = static_cast<intptr_t>(resolvedOffset) - static_cast<intptr_t>(kReferenceOnSwipeProcessOffset);
    logLine(ANDROID_LOG_INFO,
            "HOOK_SCAN resolved source=%s pattern=%s resolver=%s detail=%s target=%p resolvedOffset=0x%zx referenceOffset=0x%zx delta=%lld execRanges=%zu",
            source, resolution.match.pattern->name, resolution.match.pattern->name,
            resolution.match.detail == nullptr ? "<none>" : resolution.match.detail,
            reinterpret_cast<void *>(target), static_cast<size_t>(resolvedOffset),
            static_cast<size_t>(kReferenceOnSwipeProcessOffset), static_cast<long long>(delta),
            library.executableRangeCount);

    return installFreshHookLocked(library, target, *resolution.match.pattern, source, resolution.match.detail);
}

bool ensureHook(const LibraryInfo &library, const char *source) {
    std::lock_guard<std::mutex> lock(gHookMutex);
    const bool ready = ensureHookLocked(library, source);
    if (ready) {
        installAuxGestureHapticHooks(library);
        ensureHapticCaptureHook();
    }
    return ready;
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
                logLine(ANDROID_LOG_WARN, "HOOK_HEALTH %s absent for ~%lldms; waiting for remap",
                        kTargetLibrary, static_cast<long long>(missingPolls * kHookHealthIntervalMs));
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

extern "C" {
__attribute__((visibility("hidden"))) void *gSwipeGateOriginalOnSwipeProcess = nullptr;
__attribute__((visibility("hidden"))) void *gSwipeGateOriginalOnSwipeStart = nullptr;
__attribute__((visibility("hidden"))) void *gSwipeGateOriginalOnBackInvoke = nullptr;
__attribute__((visibility("hidden"))) void *gSwipeGateOriginalOnBackCancelled = nullptr;

__attribute__((visibility("hidden"))) float swipegate_hook_enter_and_gate(
        uint32_t readyFinish, uint32_t side, float horizontalDistancePx) {
    gActiveHookCalls.fetch_add(1, std::memory_order_acq_rel);
    if (isLauncherProcess()) ensureWorkerStarted();
    const bool readyNow = readyFinish != 0;
    const bool readyBefore = gReadyHapticLatched.exchange(readyNow, std::memory_order_acq_rel);
    if (readyNow && !readyBefore) performReturnHaptic("ready", true);
    return gateHorizontalDistance(readyFinish != 0, side, horizontalDistancePx);
}

__attribute__((visibility("hidden"))) void swipegate_haptic_on_swipe_start() {
    gGestureActive.store(true, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
}

__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_invoke() {
    gGestureActive.store(false, std::memory_order_release);
    performReturnHaptic("commit", false);
    gReadyHapticLatched.store(false, std::memory_order_release);
}

__attribute__((visibility("hidden"))) void swipegate_haptic_on_back_cancelled() {
    gGestureActive.store(false, std::memory_order_release);
    gReadyHapticLatched.store(false, std::memory_order_release);
}

__attribute__((visibility("hidden"))) void swipegate_hook_exit() {
    gActiveHookCalls.fetch_sub(1, std::memory_order_acq_rel);
}
}

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    const std::string executable = readExecutable();
    const std::string processName = readProcessName();
    const bool entriesReady = entries != nullptr;
    const bool hookReady = entriesReady && entries->hook_func != nullptr;
    const bool hyosProcess = executable == kSpawnerPath;
    const bool launcherProcess = processName == kTargetPackage;

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
                static_cast<const void *>(entries), entriesReady ? reinterpret_cast<void *>(entries->hook_func) : nullptr);
        return nullptr;
    }
    if (!hyosProcess) {
        logLine(ANDROID_LOG_WARN, "DP_GATE native_init rejected non-HYOS process exe=%s process=%s",
                executable.c_str(), processName.c_str());
        return nullptr;
    }

    gHookFunction = entries->hook_func;
    gUnhookFunction = entries->unhook_func;
    logLine(ANDROID_LOG_INFO,
            "DP_GATE native_init accepted api=%u exe=%s process=%s launcherCmdline=%d hook_func=%p unhook_func=%p watchdog=%lldms resolver=exact-profile-first+semantic-unknown-build abi=transparent-s0 repair=unhook+rehook",
            entries->version, executable.c_str(), processName.c_str(), launcherProcess ? 1 : 0,
            reinterpret_cast<void *>(entries->hook_func), reinterpret_cast<void *>(entries->unhook_func),
            static_cast<long long>(kHookHealthIntervalMs));

    const LibraryInfo library = findLauncherLibrary();
    if (library.base != 0) {
        logLine(ANDROID_LOG_INFO, "DP_GATE native_init backfill found %s base=%p process=%s",
                kTargetLibrary, reinterpret_cast<void *>(library.base), processName.c_str());
        ensureHook(library, "native-init-backfill");
    } else {
        logLine(ANDROID_LOG_INFO, "DP_GATE native_init waiting for %s process=%s",
                kTargetLibrary, processName.c_str());
    }

    if (launcherProcess) ensureWorkerStarted();
    return onLibraryLoaded;
}
