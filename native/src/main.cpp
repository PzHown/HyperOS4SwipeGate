#include "native_api.h"
#include "swipe_semantic_resolver.h"
#include "control_channel.h"
#include "hook_page_guard.h"

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/system_properties.h>
#include <sys/syscall.h>
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
__attribute__((visibility("hidden"))) void swipegate_back_break_enable(
        HookFunType hookFunction, UnhookFunType unhookFunction);
__attribute__((visibility("hidden"))) void swipegate_back_break_maintain();
}

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kSpawnerPath = "/system_ext/bin/hyos_spawner";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr const char *kHapticProviderLibrary = "libhyper_os_background_tasks_public.so";
constexpr const char *kHapticSymbol = "HapticFeedback_perform_ext_haptic_feedback";
constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;
constexpr int64_t kHapticInitialResolveRetryMs = 500;
constexpr uint32_t kHapticInitialResolveFastAttempts = 10;
constexpr int64_t kReadyReleaseDedupMs = 750;
constexpr int32_t kHapticConstant = 0;
constexpr uintptr_t kPointerAddressMask = 0x00ffffffffffffffull;
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
constexpr size_t kMaxLoadRanges = 16;

constexpr uintptr_t kReferenceOnSwipeProcessOffset = 0x816fc4;

// These byte patterns are feature fingerprints, not version gates. A newer Launcher may
// reuse the same gesture implementation and is compatible when the fingerprint still matches.
// Semantic resolution may corroborate a known fingerprint; on a changed implementation it may
// take over only when it proves exactly one candidate.
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
        {"gesture-frame-v1", kOnSwipeProcessPatternV1, kOnSwipeProcessMaskV1,
         sizeof(kOnSwipeProcessPatternV1)},
        {"gesture-frame-v2", kOnSwipeProcessPatternV2, kOnSwipeProcessMaskV2,
         sizeof(kOnSwipeProcessPatternV2)},
};

constexpr PatternSpec kSemanticOnSwipeProcessPattern = {
        "semantic-motion-graph", nullptr, nullptr, 0,
};

// Launcher 8.0.x GestureBackArrowView::check_and_perform_haptic_feedback entry.
// 6174 disassembly starts with:
//   ldr w8,[x0,#0x18] / ldrb w9,[x0,#0x20] / cbnz feedback_done /
//   cmp w8,#4 / b.hi / and w8,w8,#0xff / cmp w8,#3 / b.ne.
// Branch immediates are masked; register/opcode/state constants remain exact. A candidate
// is accepted only when the same function also contains the imported HyperRT haptic call
// followed by feedback_done=true, so this is a semantic 8.x feature fingerprint rather
// than a hard-coded RVA.
constexpr uint8_t kReleaseFeedbackPatternV1[] = {
        0x08,0x18,0x40,0xb9, 0x09,0x80,0x40,0x39, 0x09,0x00,0x00,0x35,
        0x1f,0x11,0x00,0x71, 0x08,0x00,0x00,0x54, 0x08,0x1d,0x00,0x12,
        0x1f,0x0d,0x00,0x71, 0x01,0x00,0x00,0x54,
};
constexpr uint8_t kReleaseFeedbackMaskV1[] = {
        0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff,
        0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff, 0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff, 0x1f,0x00,0x00,0xff,
};
constexpr PatternSpec kReleaseFeedbackPattern = {
        "release-ready-state-back-v1", kReleaseFeedbackPatternV1,
        kReleaseFeedbackMaskV1, sizeof(kReleaseFeedbackPatternV1),
};

static_assert(sizeof(kOnSwipeProcessPatternV1) == sizeof(kOnSwipeProcessMaskV1));
static_assert(sizeof(kReleaseFeedbackPatternV1) == sizeof(kReleaseFeedbackMaskV1));
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

// Haptic V2 is deliberately isolated from gHookMutex. Primary discovery follows the
// production Android hook-library pattern used by xHook/ByteHook/xDL: inspect the already
// loaded Launcher's PT_DYNAMIC metadata in memory. No APK path open and no haptic dlopen.
std::atomic<void *> gOriginalHapticFeedback{nullptr};
std::atomic<uintptr_t> gCapturedHapticArc{0};
std::atomic<int64_t> gCapturedHapticArcAtMs{0};
std::atomic<bool> gHapticInstallInProgress{false};
std::atomic<bool> gHapticCaptureHookInstalled{false};
std::atomic<bool> gHapticUnavailableLogged{false};
std::atomic<int64_t> gLastHapticFeatureResolveMs{0};
std::atomic<uint32_t> gHapticResolveFailures{0};
std::atomic<int64_t> gNativeHapticSuppressUntilMs{0};
std::atomic<int64_t> gLastInjectedHapticAtMs{0};
std::atomic<uintptr_t> gGetGlobalRuntime{0};
std::atomic<void *> gRuntimeDecStrong{nullptr};
std::atomic<bool> gHapticRuntimeBridgeResolved{false};
std::atomic<uint64_t> gHapticTraceSequence{0};
std::atomic<int64_t> gLastReadyHapticAtMs{0};
std::atomic<bool> gReadyReleaseDedupEligible{false};
// Launcher 8.0.x emits the stock hand-up vibration synchronously from
// GestureBackArrowView::check_and_perform_haptic_feedback during on_swipe_stop. The
// boundary hook keeps Xiaomi's state bookkeeping intact and only scopes the nested
// constant=0 HyperRT call for one-shot suppression.
std::atomic<void *> gOriginalReleaseFeedbackHelper{nullptr};
std::atomic<uintptr_t> gReleaseFeedbackTarget{0};
std::atomic<bool> gReleaseFeedbackHookInstalled{false};
thread_local bool gReleaseFeedbackScopeArmed = false;
thread_local int64_t gReleaseFeedbackScopeReadyAtMs = 0;
// Haptic segment state is independent from the Launcher hook health state.
// 0 = outside/idle, 1 = first segment (Back, below custom threshold),
// 2 = second segment (custom threshold reached). Only entering segment 1 is replayed.
std::atomic<int> gHapticGestureSegment{0};

std::mutex gHookMutex;
std::array<uint8_t, kHookProbeSize> gInstalledPatchHead{};
std::array<uint8_t, kHookProbeSize> gExpectedOriginalHead{};
bool gInstalledPatchHeadReady = false;
bool gExpectedOriginalHeadReady = false;
const char *gActivePatternName = "<none>";
const char *gActiveResolverDetail = "<none>";

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
    const ElfW(Phdr) *programHeaders = nullptr;
    ElfW(Half) programHeaderCount = 0;
    std::array<ExecutableRange, kMaxExecutableRanges> executableRanges{};
    size_t executableRangeCount = 0;
    std::array<ExecutableRange, kMaxLoadRanges> loadRanges{};
    size_t loadRangeCount = 0;
};

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    const std::string path(info->dlpi_name);
    if (path.find(kTargetLibrary) == std::string::npos) return 0;

    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->path = path;
    result->programHeaders = info->dlpi_phdr;
    result->programHeaderCount = info->dlpi_phnum;
    result->executableRangeCount = 0;
    result->loadRangeCount = 0;

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        if (result->loadRangeCount < result->loadRanges.size()) {
            auto &load = result->loadRanges[result->loadRangeCount++];
            load.start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);
            load.size = static_cast<size_t>(phdr.p_memsz);
        }
        if ((phdr.p_flags & PF_X) == 0
                || result->executableRangeCount >= result->executableRanges.size()) continue;
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

bool rangeContains(const ExecutableRange &range, uintptr_t address, size_t size) {
    if (range.start == 0 || range.size == 0 || address == 0 || size == 0) return false;
    if (address < range.start) return false;
    const uintptr_t end = address + size;
    return end >= address && end <= range.start + range.size;
}

bool libraryContainsRange(const LibraryInfo &library, uintptr_t address, size_t size) {
    for (size_t i = 0; i < library.loadRangeCount; ++i) {
        if (rangeContains(library.loadRanges[i], address, size)) return true;
    }
    return false;
}

uintptr_t resolveLoadedElfPointer(const LibraryInfo &library, uintptr_t value, size_t size) {
    if (value == 0 || size == 0) return 0;
    if (libraryContainsRange(library, value, size)) return value;
    const uintptr_t rebased = library.base + value;
    if (rebased >= library.base && libraryContainsRange(library, rebased, size)) return rebased;
    return 0;
}

bool dynStringEquals(const LibraryInfo &library, uintptr_t strtab, size_t strsz,
                     size_t offset, const char *expected) {
    if (expected == nullptr || offset >= strsz) return false;
    const uintptr_t address = strtab + offset;
    if (address < strtab) return false;
    const size_t remaining = strsz - offset;
    if (!libraryContainsRange(library, address, remaining)) return false;
    const size_t expectedLength = std::strlen(expected);
    if (expectedLength >= remaining) return false;
    return std::memcmp(reinterpret_cast<const void *>(address), expected, expectedLength) == 0
            && *reinterpret_cast<const char *>(address + expectedLength) == '\0';
}

struct ImportedFunctionResolution {
    void *target = nullptr;
    uintptr_t slot = 0;
    size_t matches = 0;
    const char *relocationKind = nullptr;
};

ImportedFunctionResolution resolveImportedFunction(const LibraryInfo &library, const char *symbolName) {
    ImportedFunctionResolution result;
    if (library.base == 0 || library.programHeaders == nullptr || library.programHeaderCount == 0
            || symbolName == nullptr || *symbolName == '\0') return result;

    uintptr_t dynamicAddress = 0;
    size_t dynamicSize = 0;
    for (ElfW(Half) i = 0; i < library.programHeaderCount; ++i) {
        const ElfW(Phdr) &phdr = library.programHeaders[i];
        if (phdr.p_type != PT_DYNAMIC || phdr.p_memsz < sizeof(ElfW(Dyn))) continue;
        dynamicAddress = library.base + static_cast<uintptr_t>(phdr.p_vaddr);
        dynamicSize = static_cast<size_t>(phdr.p_memsz);
        break;
    }
    if (dynamicAddress == 0 || !libraryContainsRange(library, dynamicAddress, dynamicSize)) return result;

    uintptr_t strtabValue = 0;
    size_t strsz = 0;
    uintptr_t symtabValue = 0;
    size_t syment = sizeof(ElfW(Sym));
    uintptr_t jmprelValue = 0;
    size_t pltrelsz = 0;
    uintptr_t relaValue = 0;
    size_t relasz = 0;
    ElfW(Sxword) pltrelType = 0;

    const size_t maxDynamicEntries = dynamicSize / sizeof(ElfW(Dyn));
    for (size_t i = 0; i < maxDynamicEntries; ++i) {
        ElfW(Dyn) entry{};
        std::memcpy(&entry,
                    reinterpret_cast<const void *>(dynamicAddress + i * sizeof(ElfW(Dyn))),
                    sizeof(entry));
        if (entry.d_tag == DT_NULL) break;
        switch (entry.d_tag) {
            case DT_STRTAB: strtabValue = static_cast<uintptr_t>(entry.d_un.d_ptr); break;
            case DT_STRSZ: strsz = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_SYMTAB: symtabValue = static_cast<uintptr_t>(entry.d_un.d_ptr); break;
            case DT_SYMENT: syment = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_JMPREL: jmprelValue = static_cast<uintptr_t>(entry.d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_PLTREL: pltrelType = static_cast<ElfW(Sxword)>(entry.d_un.d_val); break;
            case DT_RELA: relaValue = static_cast<uintptr_t>(entry.d_un.d_ptr); break;
            case DT_RELASZ: relasz = static_cast<size_t>(entry.d_un.d_val); break;
            default: break;
        }
    }
    if (strtabValue == 0 || strsz == 0 || symtabValue == 0 || syment != sizeof(ElfW(Sym))) {
        return result;
    }

    const uintptr_t strtab = resolveLoadedElfPointer(library, strtabValue, strsz);
    const uintptr_t symtab = resolveLoadedElfPointer(library, symtabValue, sizeof(ElfW(Sym)));
    if (strtab == 0 || symtab == 0) return result;

    const auto scanRela = [&](uintptr_t table, size_t bytes, const char *kind) {
        if (table == 0 || bytes < sizeof(ElfW(Rela)) || bytes % sizeof(ElfW(Rela)) != 0) return;
        if (!libraryContainsRange(library, table, bytes)) return;
        const size_t count = bytes / sizeof(ElfW(Rela));
        for (size_t index = 0; index < count; ++index) {
            ElfW(Rela) relocation{};
            std::memcpy(&relocation,
                        reinterpret_cast<const void *>(table + index * sizeof(ElfW(Rela))),
                        sizeof(relocation));
            const size_t symbolIndex = static_cast<size_t>(ELF64_R_SYM(relocation.r_info));
            if (symbolIndex > (SIZE_MAX - symtab) / sizeof(ElfW(Sym))) continue;
            const uintptr_t symbolAddress = symtab + symbolIndex * sizeof(ElfW(Sym));
            if (symbolAddress < symtab
                    || !libraryContainsRange(library, symbolAddress, sizeof(ElfW(Sym)))) continue;
            const auto &symbol = *reinterpret_cast<const ElfW(Sym) *>(symbolAddress);
            if (!dynStringEquals(library, strtab, strsz,
                                 static_cast<size_t>(symbol.st_name), symbolName)) continue;
            const uintptr_t slot = resolveLoadedElfPointer(
                    library, relocation.r_offset, sizeof(void *));
            if (slot == 0) continue;
            void *target = nullptr;
            std::memcpy(&target, reinterpret_cast<const void *>(slot), sizeof(target));
            if (reinterpret_cast<uintptr_t>(target) < 0x10000u) continue;
            if (result.matches == 0) {
                result.slot = slot;
                result.target = target;
                result.matches = 1;
                result.relocationKind = kind;
            } else if (result.target != target) {
                result.matches = 2;
                result.relocationKind = "ambiguous";
                return;
            }
        }
    };

    if (jmprelValue != 0 && pltrelsz != 0 && pltrelType == DT_RELA) {
        const uintptr_t jmprel = resolveLoadedElfPointer(library, jmprelValue, pltrelsz);
        scanRela(jmprel, pltrelsz, "DT_JMPREL");
    }
    if (result.matches <= 1 && relaValue != 0 && relasz != 0) {
        const uintptr_t rela = resolveLoadedElfPointer(library, relaValue, relasz);
        scanRela(rela, relasz, "DT_RELA");
    }
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

const char *traceModuleBasename(const char *path) {
    if (path == nullptr || *path == '\0') return "<unresolved>";
    const char *slash = std::strrchr(path, '/');
    return slash == nullptr ? path : slash + 1;
}

void traceStockHapticCall(int32_t constant, int64_t now, uintptr_t callerReturnAddress) {
    const uintptr_t callsite = callerReturnAddress >= 4u ? callerReturnAddress - 4u : 0u;
    Dl_info callerInfo{};
    const bool callerResolved = callsite != 0
            && dladdr(reinterpret_cast<void *>(callsite), &callerInfo) != 0
            && callerInfo.dli_fbase != nullptr;
    const uintptr_t moduleBase = callerResolved
            ? reinterpret_cast<uintptr_t>(callerInfo.dli_fbase) : 0u;
    const uintptr_t lrRva = moduleBase != 0 && callerReturnAddress >= moduleBase
            ? callerReturnAddress - moduleBase : 0u;
    const uintptr_t callsiteRva = moduleBase != 0 && callsite >= moduleBase
            ? callsite - moduleBase : 0u;
    const bool launcherCaller = callerResolved && callerInfo.dli_fname != nullptr
            && std::strstr(callerInfo.dli_fname, kTargetLibrary) != nullptr;

    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
    const int64_t deltaReadyMs = readyAt > 0 && now >= readyAt ? now - readyAt : -1;
    const int segment = gHapticGestureSegment.load(std::memory_order_acquire);
    const bool eligible = gReadyReleaseDedupEligible.load(std::memory_order_acquire);
    const bool releaseHook = gReleaseFeedbackHookInstalled.load(std::memory_order_acquire);
    const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);
    const uintptr_t launcherBase = gHookedBase.load(std::memory_order_acquire);
    const uintptr_t releaseTargetRva = launcherBase != 0 && releaseTarget >= launcherBase
            ? releaseTarget - launcherBase : 0u;
    const uint64_t sequence = gHapticTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const long tid = static_cast<long>(syscall(SYS_gettid));

    logLine(ANDROID_LOG_INFO,
            "HAPTIC_TRACE stock-call seq=%llu tid=%ld constant=%d caller=%p callsite=%p module=%s moduleBase=%p lrRva=0x%zx callsiteRva=0x%zx launcherCaller=%d launcherBase=%p deltaReadyMs=%lld segment=%d eligible=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx scopeArmed=%d",
            static_cast<unsigned long long>(sequence), tid, constant,
            reinterpret_cast<void *>(callerReturnAddress), reinterpret_cast<void *>(callsite),
            callerResolved ? traceModuleBasename(callerInfo.dli_fname) : "<unresolved>",
            reinterpret_cast<void *>(moduleBase), static_cast<size_t>(lrRva),
            static_cast<size_t>(callsiteRva), launcherCaller ? 1 : 0,
            reinterpret_cast<void *>(launcherBase), static_cast<long long>(deltaReadyMs),
            segment, eligible ? 1 : 0, releaseHook ? 1 : 0,
            reinterpret_cast<void *>(releaseTarget), static_cast<size_t>(releaseTargetRva),
            gReleaseFeedbackScopeArmed ? 1 : 0);
}

__attribute__((noinline)) void hapticFeedbackCaptureHook(void *storage, int32_t constant) {
    // The provider target is inline-hooked. Xiaomi's PLT uses BR, not BL, so LR still points
    // at the instruction immediately after the real callsite in libapp_launcher.so. Capture
    // it before doing any other work. Synthetic Ready feedback uses the original trampoline
    // and therefore never enters this function.
    void *rawReturnAddress = __builtin_return_address(0);
    const uintptr_t callerReturnAddress = reinterpret_cast<uintptr_t>(
            __builtin_extract_return_addr(rawReturnAddress)) & kPointerAddressMask;
    const int64_t now = monotonicMs();
    traceStockHapticCall(constant, now, callerReturnAddress);
    if (storage != nullptr) {
        uintptr_t rawArc = 0;
        std::memcpy(&rawArc, storage, sizeof(rawArc));
        const uintptr_t addressBits = rawArc & kPointerAddressMask;
        if (addressBits >= 0x10000u) {
            // Preserve the original tagged pointer exactly as Xiaomi supplied it.  The
            // untagged value is used only for sanity checking, never for replay.
            gCapturedHapticArc.store(rawArc, std::memory_order_release);
            gCapturedHapticArcAtMs.store(now, std::memory_order_release);
            gHapticUnavailableLogged.store(false, std::memory_order_release);
        }
    }

    const auto original = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    if (original == nullptr) return;

    // The stock 8.0.x hand-up effect is synchronous inside the resolved
    // GestureBackArrowView release helper. Suppress only the nested constant=0 call while
    // that exact helper is active. The helper itself still runs and sets feedback_done,
    // preserving Xiaomi's release state machine.
    if (constant == kHapticConstant && gReleaseFeedbackScopeArmed) {
        gReleaseFeedbackScopeArmed = false;
        const int64_t readyAt = gReleaseFeedbackScopeReadyAtMs;
        gReleaseFeedbackScopeReadyAtMs = 0;
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release suppressed reason=ready-release-boundary constant=%d deltaMs=%lld windowMs=%lld source=GestureBackArrowView::check_and_perform_haptic_feedback",
                constant,
                readyAt > 0 && now >= readyAt
                        ? static_cast<long long>(now - readyAt) : -1LL,
                static_cast<long long>(kReadyReleaseDedupMs));
        return;
    }

    original(storage, constant);
}

bool installHapticCaptureHookTarget(void *target, const char *source) {
    if (target == nullptr || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    bool expected = false;
    if (!gHapticInstallInProgress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return gHapticCaptureHookInstalled.load(std::memory_order_acquire);
    }

    void *backup = nullptr;
    const int rc = swipegate_install_protected_inline_hook(
            gHookFunction, target, reinterpret_cast<void *>(hapticFeedbackCaptureHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC_V2 capture hook failed source=%s rc=%d target=%p backup=%p",
                source == nullptr ? "unknown" : source, rc, target, backup);
        gHapticInstallInProgress.store(false, std::memory_order_release);
        return false;
    }

    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gHapticResolveFailures.store(0, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticInstallInProgress.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 capture hook ready source=%s target=%p feature=%s workerOnly=1 no-dlsym=1 no-dlopen=1",
            source == nullptr ? "unknown" : source, target, kHapticSymbol);
    return true;
}

bool installHapticCaptureHookFromLauncherImport(const LibraryInfo &library, const char *source) {
    if (library.base == 0 || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    const ImportedFunctionResolution resolution = resolveImportedFunction(library, kHapticSymbol);
    if (resolution.matches != 1 || resolution.target == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 loaded-elf import unresolved source=%s symbol=%s matches=%zu launcher=%s",
                source == nullptr ? "unknown" : source, kHapticSymbol,
                resolution.matches, library.path.c_str());
        return false;
    }
    Dl_info providerInfo{};
    if (dladdr(resolution.target, &providerInfo) == 0
            || providerInfo.dli_fname == nullptr
            || std::strstr(providerInfo.dli_fname, kHapticProviderLibrary) == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 loaded-elf target rejected source=%s target=%p provider=%s",
                source == nullptr ? "unknown" : source, resolution.target,
                providerInfo.dli_fname == nullptr ? "<unknown>" : providerInfo.dli_fname);
        return false;
    }
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 loaded-elf import resolved source=%s symbol=%s reloc=%s got=%p target=%p provider=%s launcher=%s",
            source == nullptr ? "unknown" : source, kHapticSymbol,
            resolution.relocationKind == nullptr ? "unknown" : resolution.relocationKind,
            reinterpret_cast<void *>(resolution.slot), resolution.target, providerInfo.dli_fname,
            library.path.c_str());
    return installHapticCaptureHookTarget(resolution.target, "launcher-import-feature");
}

bool decodeBlTarget(uintptr_t pc, uint32_t instruction, uintptr_t *target) {
    if (target == nullptr || (instruction & 0xfc000000u) != 0x94000000u) return false;
    int64_t imm26 = static_cast<int64_t>(instruction & 0x03ffffffu);
    if ((imm26 & 0x02000000LL) != 0) imm26 |= ~0x03ffffffLL;
    const int64_t delta = imm26 * 4LL;
    *target = static_cast<uintptr_t>(static_cast<int64_t>(pc) + delta);
    return true;
}

bool pltReferencesSlot(const LibraryInfo &library, uintptr_t plt, uintptr_t expectedSlot) {
    if (plt == 0 || expectedSlot == 0 || !libraryContainsRange(library, plt, 16)) return false;
    uint32_t insn[4]{};
    std::memcpy(insn, reinterpret_cast<const void *>(plt), sizeof(insn));
    if ((insn[0] & 0x9f00001fu) != 0x90000010u) return false;
    if ((insn[2] & 0xffc003ffu) != 0x91000210u) return false;
    if (insn[3] != 0xd61f0220u) return false;

    int64_t immhi = static_cast<int64_t>((insn[0] >> 5) & 0x7ffffu);
    const int64_t immlo = static_cast<int64_t>((insn[0] >> 29) & 0x3u);
    int64_t imm21 = (immhi << 2) | immlo;
    if ((imm21 & (1LL << 20)) != 0) imm21 |= ~((1LL << 21) - 1LL);
    const uintptr_t page = static_cast<uintptr_t>(
            static_cast<int64_t>(plt & ~static_cast<uintptr_t>(0xfff)) + imm21 * 4096LL);
    const uint32_t shift = (insn[2] >> 22) & 0x1u;
    const uintptr_t addImm = static_cast<uintptr_t>((insn[2] >> 10) & 0xfffu)
            << (shift != 0 ? 12 : 0);
    return page + addImm == expectedSlot;
}

bool resolveHyperRtRuntimeBridge(const LibraryInfo &library) {
    if (gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) return true;
    if (library.base == 0) return false;

    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    const ImportedFunctionResolution decStrong = resolveImportedFunction(library, "Runtime_dec_strong");
    if (haptic.matches != 1 || haptic.slot == 0
            || decStrong.matches != 1 || decStrong.slot == 0 || decStrong.target == nullptr) {
        return false;
    }

    uintptr_t runtimeTarget = 0;
    size_t corroboratedCallsites = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < 36) continue;
        const uintptr_t start = (range.start + 20u + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - 12u;
        for (uintptr_t pc = start; pc <= last; pc += 4u) {
            uint32_t callInsn = 0;
            std::memcpy(&callInsn, reinterpret_cast<const void *>(pc), sizeof(callInsn));
            uintptr_t callTarget = 0;
            if (!decodeBlTarget(pc, callInsn, &callTarget)
                    || !pltReferencesSlot(library, callTarget, haptic.slot)) continue;

            uint32_t movW1 = 0;
            uint32_t saveRuntime = 0;
            uint32_t restoreRuntime = 0;
            uint32_t runtimeCallInsn = 0;
            uint32_t decCallInsn = 0;
            std::memcpy(&movW1, reinterpret_cast<const void *>(pc - 4u), sizeof(movW1));
            std::memcpy(&saveRuntime, reinterpret_cast<const void *>(pc - 16u), sizeof(saveRuntime));
            std::memcpy(&restoreRuntime, reinterpret_cast<const void *>(pc + 4u), sizeof(restoreRuntime));
            std::memcpy(&runtimeCallInsn, reinterpret_cast<const void *>(pc - 20u), sizeof(runtimeCallInsn));
            std::memcpy(&decCallInsn, reinterpret_cast<const void *>(pc + 8u), sizeof(decCallInsn));

            if (movW1 != 0x2a1f03e1u) continue;
            if ((saveRuntime & 0xffffffe0u) != 0xaa0003e0u) continue;
            const uint32_t runtimeReg = saveRuntime & 0x1fu;
            if (restoreRuntime != (0xaa0003e0u | (runtimeReg << 16))) continue;

            uintptr_t candidateRuntime = 0;
            uintptr_t decPlt = 0;
            if (!decodeBlTarget(pc - 20u, runtimeCallInsn, &candidateRuntime)
                    || !decodeBlTarget(pc + 8u, decCallInsn, &decPlt)
                    || !pltReferencesSlot(library, decPlt, decStrong.slot)) continue;
            if (!libraryContainsRange(library, candidateRuntime, 4)) continue;

            if (runtimeTarget == 0) {
                runtimeTarget = candidateRuntime;
            } else if (runtimeTarget != candidateRuntime) {
                logLine(ANDROID_LOG_WARN,
                        "HAPTIC_V2 HyperRT runtime resolver ambiguous first=%p next=%p",
                        reinterpret_cast<void *>(runtimeTarget),
                        reinterpret_cast<void *>(candidateRuntime));
                return false;
            }
            ++corroboratedCallsites;
        }
    }

    if (runtimeTarget == 0 || corroboratedCallsites == 0) return false;
    gGetGlobalRuntime.store(runtimeTarget, std::memory_order_release);
    gRuntimeDecStrong.store(decStrong.target, std::memory_order_release);
    gHapticRuntimeBridgeResolved.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 HyperRT runtime bridge resolved getRuntime=%p decStrong=%p stockCallsites=%zu",
            reinterpret_cast<void *>(runtimeTarget), decStrong.target, corroboratedCallsites);
    return true;
}

bool performNativeHaptic(const char *stage) {
    swipegate_control_sync_if_due();
    if (swipegate_control_haptic_enabled() != 1) return false;

    const auto haptic = reinterpret_cast<HapticFeedbackFn>(
            gOriginalHapticFeedback.load(std::memory_order_acquire));
    const auto getRuntime = reinterpret_cast<void *(*)()>(
            gGetGlobalRuntime.load(std::memory_order_acquire));
    const auto decStrong = reinterpret_cast<void (*)(void *)>(
            gRuntimeDecStrong.load(std::memory_order_acquire));
    if (haptic == nullptr || getRuntime == nullptr || decStrong == nullptr
            || !gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) {
        bool expected = false;
        if (gHapticUnavailableLogged.compare_exchange_strong(expected, true)) {
            logLine(ANDROID_LOG_WARN,
                    "HAPTIC_V2 skipped stage=%s reason=hyperrt-runtime-bridge-not-ready hook=%d bridge=%d getRuntime=%p decStrong=%p",
                    stage == nullptr ? "unknown" : stage,
                    haptic == nullptr ? 0 : 1,
                    gHapticRuntimeBridgeResolved.load(std::memory_order_acquire) ? 1 : 0,
                    reinterpret_cast<void *>(getRuntime), reinterpret_cast<void *>(decStrong));
        }
        return false;
    }

    void *runtime = getRuntime();
    if (runtime == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 skipped stage=%s reason=get-global-runtime-null",
                stage == nullptr ? "unknown" : stage);
        return false;
    }
    // Synthetic Ready uses the original HyperRT trampoline, outside the stock
    // release-helper scope, so it can never suppress itself.

    void *storage = runtime;
    haptic(&storage, kHapticConstant);
    decStrong(runtime);

    const int64_t now = monotonicMs();
    gLastReadyHapticAtMs.store(now, std::memory_order_release);
    gReadyReleaseDedupEligible.store(true, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feedback stage=%s kind=hyperrt-stock constant=%d readyReleaseDedupMs=%lld",
            stage == nullptr ? "unknown" : stage, kHapticConstant,
            static_cast<long long>(kReadyReleaseDedupMs));
    return true;
}

using ReleaseFeedbackHelperFn = void (*)(void *);

bool releaseFeedbackCandidateHasHapticCall(
        const LibraryInfo &library, uintptr_t candidate, uintptr_t hapticSlot) {
    if (candidate == 0 || hapticSlot == 0 || !libraryContainsRange(library, candidate, 0xc0)) {
        return false;
    }
    for (uintptr_t pc = candidate + 0x20; pc <= candidate + 0xb0; pc += 4) {
        uint32_t callInsn = 0;
        std::memcpy(&callInsn, reinterpret_cast<const void *>(pc), sizeof(callInsn));
        uintptr_t callTarget = 0;
        if (!decodeBlTarget(pc, callInsn, &callTarget)
                || !pltReferencesSlot(library, callTarget, hapticSlot)) {
            continue;
        }
        if (!libraryContainsRange(library, pc + 16, 4)) return false;
        uint32_t movFeedbackDone = 0;
        uint32_t storeFeedbackDone = 0;
        std::memcpy(&movFeedbackDone,
                    reinterpret_cast<const void *>(pc + 12), sizeof(movFeedbackDone));
        std::memcpy(&storeFeedbackDone,
                    reinterpret_cast<const void *>(pc + 16), sizeof(storeFeedbackDone));
        // 6174: mov w8,#1 ; strb w8,[x19,#0x20]. This corroborates that the call is
        // check_and_perform_haptic_feedback rather than an unrelated constant=0 site.
        if (movFeedbackDone == 0x52800028u && storeFeedbackDone == 0x39008268u) {
            return true;
        }
    }
    return false;
}

uintptr_t resolveReleaseFeedbackTarget(const LibraryInfo &library, const char **featureName) {
    if (featureName != nullptr) *featureName = nullptr;
    const ImportedFunctionResolution haptic = resolveImportedFunction(library, kHapticSymbol);
    if (haptic.matches != 1 || haptic.slot == 0) {
        if (featureName != nullptr) *featureName = "haptic-import-unresolved";
        return 0;
    }

    uintptr_t found = 0;
    size_t matches = 0;
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < kReleaseFeedbackPattern.size) continue;
        const uintptr_t first = (range.start + 3u) & ~static_cast<uintptr_t>(3u);
        const uintptr_t last = range.start + range.size - kReleaseFeedbackPattern.size;
        for (uintptr_t cursor = first; cursor <= last; cursor += 4u) {
            if (!patternMatchesAt(cursor, kReleaseFeedbackPattern)) continue;
            if (!releaseFeedbackCandidateHasHapticCall(library, cursor, haptic.slot)) continue;
            found = cursor;
            if (++matches > 1) {
                if (featureName != nullptr) *featureName = "ambiguous";
                return 0;
            }
        }
    }
    if (matches == 1 && featureName != nullptr) {
        *featureName = kReleaseFeedbackPattern.name;
    }
    return matches == 1 ? found : 0;
}

void releaseFeedbackHelperHook(void *arrowState) {
    const auto original = reinterpret_cast<ReleaseFeedbackHelperFn>(
            gOriginalReleaseFeedbackHelper.load(std::memory_order_acquire));
    if (original == nullptr) return;

    const int64_t now = monotonicMs();
    const bool eligible = gReadyReleaseDedupEligible.exchange(false, std::memory_order_acq_rel);
    const int64_t readyAt = gLastReadyHapticAtMs.load(std::memory_order_acquire);
    const int64_t delta = readyAt > 0 && now >= readyAt ? now - readyAt : -1;
    const bool suppress = eligible && delta >= 0 && delta < kReadyReleaseDedupMs
            && gHapticCaptureHookInstalled.load(std::memory_order_acquire);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_TRACE release-boundary-enter tid=%ld eligible=%d deltaReadyMs=%lld segment=%d suppress=%d target=%p",
            static_cast<long>(syscall(SYS_gettid)), eligible ? 1 : 0,
            static_cast<long long>(delta),
            gHapticGestureSegment.load(std::memory_order_acquire), suppress ? 1 : 0,
            reinterpret_cast<void *>(gReleaseFeedbackTarget.load(std::memory_order_acquire)));

    gReleaseFeedbackScopeReadyAtMs = suppress ? readyAt : 0;
    gReleaseFeedbackScopeArmed = suppress;
    if (suppress) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 release boundary armed deltaMs=%lld windowMs=%lld semantic=READY_STATE_BACK",
                static_cast<long long>(delta), static_cast<long long>(kReadyReleaseDedupMs));
    }

    // Do not skip Xiaomi's helper. The nested HyperRT call is intercepted instead, so
    // feedback_done and all release-state bookkeeping remain stock.
    original(arrowState);

    gReleaseFeedbackScopeArmed = false;
    gReleaseFeedbackScopeReadyAtMs = 0;
    gHapticGestureSegment.store(0, std::memory_order_release);
}

bool installReleaseFeedbackHapticHook(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;
    const char *featureName = nullptr;
    const uintptr_t target = resolveReleaseFeedbackTarget(library, &featureName);
    if (target == 0) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 release boundary unresolved feature=%s activeProfile=%s; stock release remains untouched",
                featureName == nullptr ? "none" : featureName, gActivePatternName);
        return false;
    }
    if (gReleaseFeedbackHookInstalled.load(std::memory_order_acquire)
            && gReleaseFeedbackTarget.load(std::memory_order_acquire) == target) {
        return true;
    }

    void *backup = nullptr;
    const int rc = swipegate_install_protected_inline_hook(
            gHookFunction, reinterpret_cast<void *>(target),
            reinterpret_cast<void *>(releaseFeedbackHelperHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC_V2 release boundary hook failed rc=%d target=%p backup=%p",
                rc, reinterpret_cast<void *>(target), backup);
        return false;
    }

    gOriginalReleaseFeedbackHelper.store(backup, std::memory_order_release);
    gReleaseFeedbackTarget.store(target, std::memory_order_release);
    gReleaseFeedbackHookInstalled.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 release boundary hook ready feature=%s activeProfile=%s target=%p semantic=READY_STATE_BACK synchronous=1",
            featureName == nullptr ? "unknown" : featureName, gActivePatternName,
            reinterpret_cast<void *>(target));
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

    // The module only fills the missing first-segment feedback. Xiaomi owns the
    // second-segment/commit feedback, so crossing into >= userGatePx must not replay
    // another vibration here. readyFinish identifies that the Back (first) segment is
    // actually armed; userGateReached separates the custom second segment.
    int hapticSegment = 0;
    if (delayBeyondStock && userGateReached) {
        hapticSegment = 2;
    } else if (readyFinish) {
        hapticSegment = 1;
    }
    if (hapticSegment == 2) {
        // Threshold / Three-hold remains 100% Xiaomi-owned. It invalidates Ready->Release
        // dedup until the gesture explicitly re-enters Ready, so threshold release is never
        // suppressed by the release-helper scope.
        gReadyReleaseDedupEligible.store(false, std::memory_order_release);
    }
    const int previousHapticSegment = gHapticGestureSegment.exchange(
            hapticSegment, std::memory_order_acq_rel);
    if (hapticSegment == 1 && previousHapticSegment != 1) {
        const char *stage = previousHapticSegment == 2 ? "return-first" : "first";
        if (!performNativeHaptic(stage)) {
            // Do not consume the segment transition when the feedback could not actually be
            // emitted.  Restoring the previous state lets a later frame retry as soon as the
            // capture hook/Arc becomes available instead of losing the first haptic forever.
            int expectedSegment = 1;
            if (gHapticGestureSegment.compare_exchange_strong(
                    expectedSegment, previousHapticSegment, std::memory_order_acq_rel)) {
                logLine(ANDROID_LOG_INFO,
                        "HAPTIC_V2 retry armed stage=%s previousSegment=%d reason=feedback-not-emitted",
                        stage, previousHapticSegment);
            }
        }
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
    const int rc = swipegate_install_protected_inline_hook(
            gHookFunction, reinterpret_cast<void *>(target),
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
    // 8.0.x stock hand-up candidate from 6174. Keep it installed while the provider-level
    // caller trace determines the actual 6179 release callsite.
    const bool releaseBoundaryReady = installReleaseFeedbackHapticHook(library);
    const uintptr_t releaseBoundaryTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_TRACE release-boundary-status installed=%d target=%p targetRva=0x%zx launcherBase=%p",
            releaseBoundaryReady ? 1 : 0, reinterpret_cast<void *>(releaseBoundaryTarget),
            library.base != 0 && releaseBoundaryTarget >= library.base
                    ? static_cast<size_t>(releaseBoundaryTarget - library.base) : 0u,
            reinterpret_cast<void *>(library.base));
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
    gReleaseFeedbackHookInstalled.store(false, std::memory_order_release);
    gReleaseFeedbackTarget.store(0, std::memory_order_release);
    gOriginalReleaseFeedbackHelper.store(nullptr, std::memory_order_release);
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
                const uintptr_t releaseTarget = gReleaseFeedbackTarget.load(std::memory_order_acquire);
                logLine(ANDROID_LOG_INFO,
                        "HOOK_HEALTH healthy source=%s base=%p target=%p pattern=%s resolver=%s detail=%s configuredDp=%d repairs=%llu hapticCapture=%d releaseHook=%d releaseTarget=%p releaseTargetRva=0x%zx",
                        source, reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(trackedTarget),
                        gActivePatternName, gActivePatternName, gActiveResolverDetail, readThresholdDp(),
                        static_cast<unsigned long long>(gRepairCount.load(std::memory_order_relaxed)),
                        gHapticCaptureHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
                        gReleaseFeedbackHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
                        reinterpret_cast<void *>(releaseTarget),
                        library.base != 0 && releaseTarget >= library.base
                                ? static_cast<size_t>(releaseTarget - library.base) : 0u);
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
    return ensureHookLocked(library, source);
}

void hookWatchdogWorker() {
    int missingPolls = 0;
    while (isTargetProcess()) {
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            missingPolls = 0;
            ensureHook(library, "watchdog");
            if (!gReleaseFeedbackHookInstalled.load(std::memory_order_acquire)) {
                installReleaseFeedbackHapticHook(library);
            }
            swipegate_back_break_maintain();
            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                    || !gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
                const uint32_t failures = gHapticResolveFailures.load(std::memory_order_relaxed);
                const int64_t retryInterval = failures < kHapticInitialResolveFastAttempts
                        ? kHapticInitialResolveRetryMs : kHapticFeatureResolveIntervalMs;
                int64_t last = gLastHapticFeatureResolveMs.load(std::memory_order_relaxed);
                if (now - last >= retryInterval
                        && gLastHapticFeatureResolveMs.compare_exchange_strong(
                                last, now, std::memory_order_relaxed)) {
                    const bool hookReady = gHapticCaptureHookInstalled.load(std::memory_order_acquire)
                            || installHapticCaptureHookFromLauncherImport(
                                    library, "watchdog-feature-probe");
                    const bool runtimeReady = gHapticRuntimeBridgeResolved.load(std::memory_order_acquire)
                            || resolveHyperRtRuntimeBridge(library);
                    if (!hookReady || !runtimeReady) {
                        gHapticResolveFailures.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        gHapticResolveFailures.store(0, std::memory_order_release);
                    }
                }
            }
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

void onLibraryLoaded(const char *name, void *handle) {
    if (!isHyosSpawnerProcessFamily() || name == nullptr) return;

    // Keep linker callbacks lightweight. Haptic symbol resolution and hook installation are
    // worker-only to avoid linker-lock re-entry/deadlock hazards.
    if (isLauncherProcess()
            && (std::strstr(name, "haptic") != nullptr
                    || std::strstr(name, "background_tasks") != nullptr)) {
        logLine(ANDROID_LOG_INFO,
                "HAPTIC_V2 loader observed name=%s handle=%p workerOnly=1",
                name, handle);
        ensureWorkerStarted();
    }

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

__attribute__((visibility("hidden"))) float swipegate_hook_enter_and_gate(
        uint32_t readyFinish, uint32_t side, float horizontalDistancePx) {
    gActiveHookCalls.fetch_add(1, std::memory_order_acq_rel);
    if (isLauncherProcess()) ensureWorkerStarted();
    return gateHorizontalDistance(readyFinish != 0, side, horizontalDistancePx);
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

    logLine(
            ANDROID_LOG_INFO,
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
    if (launcherProcess) swipegate_back_break_enable(entries->hook_func, entries->unhook_func);
    logLine(ANDROID_LOG_INFO,
            "DP_GATE native_init accepted api=%u exe=%s process=%s launcherCmdline=%d hook_func=%p unhook_func=%p watchdog=%lldms resolver=exact-profile-first+semantic-unknown-build abi=transparent-s0 repair=unhook+rehook",
            entries->version, executable.c_str(), processName.c_str(), launcherProcess ? 1 : 0,
            reinterpret_cast<void *>(entries->hook_func), reinterpret_cast<void *>(entries->unhook_func),
            static_cast<long long>(kHookHealthIntervalMs));
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 enabled policy=hyperrt-stock-runtime ready-added=1 threshold-stock=1 release-stock=1 ready-release-dedup-ms=%lld threshold-never-dedup=1 release-boundary=GestureBackArrowView constant=0",
            static_cast<long long>(kReadyReleaseDedupMs));

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