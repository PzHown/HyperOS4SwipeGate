#include "native_api.h"
#include "control_channel.h"

#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr size_t kMaxExecutableRanges = 16;
constexpr size_t kProbeSize = 16;
constexpr int64_t kHealthyLogIntervalMs = 30000;

struct ExecutableRange {
    uintptr_t start = 0;
    size_t size = 0;
};

struct LibraryInfo {
    uintptr_t base = 0;
    std::array<ExecutableRange, kMaxExecutableRanges> executableRanges{};
    size_t executableRangeCount = 0;
};

struct PatternWord {
    uint32_t value;
    uint32_t mask;
};

constexpr uint32_t kMaskAdrp = 0x9f00001fU;
constexpr uint32_t kMaskAddImm12 = 0xffc003ffU;
constexpr uint32_t kMaskCompareBranchImm = 0xff00001fU;

// These three fingerprints describe Xiaomi's complete merge-back-break-open chain.
// The support getter is only overridden after all three targets resolve uniquely.
// No Xiaomi backing flag is mutated: that flag is shared with other WindowTransition
// code and proved unsafe when set globally during Launcher startup.
constexpr PatternWord kMergeSupportPattern[] = {
        {0x90004288U, kMaskAdrp},
        {0x9133a108U, 0xffffffffU},
        {0x88dffd08U, 0xffffffffU},
        {0x350000c8U, kMaskCompareBranchImm},
        {0x90004288U, kMaskAdrp},
        {0x39738108U, 0xffffffffU},
        {0x7100011fU, 0xffffffffU},
        {0x1a9f07e0U, 0xffffffffU},
        {0xd65f03c0U, 0xffffffffU},
};

constexpr PatternWord kCanUseBreakOpenPattern[] = {
        {0xd10303ffU, 0xffffffffU}, {0xa9087bfdU, 0xffffffffU},
        {0xf9004bf7U, 0xffffffffU}, {0xa90a57f6U, 0xffffffffU},
        {0xa90b4ff4U, 0xffffffffU}, {0x910203fdU, 0xffffffffU},
        {0xf0ffd3a8U, kMaskAdrp}, {0x9125e908U, kMaskAddImm12},
        {0xb0ffcfe9U, kMaskAdrp}, {0x91376929U, kMaskAddImm12},
        {0x7100001fU, 0xffffffffU}, {0x528000eaU, 0xffffffffU},
        {0x528000abU, 0xffffffffU}, {0x9a881128U, 0xffffffffU},
        {0x90003436U, kMaskAdrp}, {0x9a8a1169U, 0xffffffffU},
        {0x2a0003f3U, 0xffffffffU}, {0xa90027e8U, 0xffffffffU},
};

constexpr PatternWord kHandleBackGesturePattern[] = {
        {0x6db82bebU, 0xffffffffU}, {0x6d0123e9U, 0xffffffffU},
        {0xa9027bfdU, 0xffffffffU}, {0xa9036ffcU, 0xffffffffU},
        {0xa90467faU, 0xffffffffU}, {0xa9055ff8U, 0xffffffffU},
        {0xa90657f6U, 0xffffffffU}, {0xa9074ff4U, 0xffffffffU},
        {0x910083fdU, 0xffffffffU}, {0xd107c3ffU, 0xffffffffU},
};

using MergeSupportFn = uint32_t (*)();

std::atomic<bool> gResolverStarted{false};
std::atomic<bool> gResolverReady{false};
std::atomic<uintptr_t> gLauncherBase{0};
std::atomic<uintptr_t> gMergeTarget{0};
std::atomic<size_t> gMergeMatches{0};
std::atomic<size_t> gCanUseMatches{0};
std::atomic<size_t> gHandleMatches{0};
std::atomic<HookFunType> gHookFunction{nullptr};
std::atomic<UnhookFunType> gUnhookFunction{nullptr};
std::atomic<void *> gOriginalMergeSupport{nullptr};
std::atomic<bool> gHookInstalled{false};
std::atomic<int64_t> gLastHealthyLogMs{0};
std::array<uint8_t, kProbeSize> gOriginalHead{};
std::array<uint8_t, kProbeSize> gPatchedHead{};
bool gOriginalHeadReady = false;
bool gPatchedHeadReady = false;
std::mutex gHookMutex;

int64_t monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

bool isLauncherProcess() {
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[256]{};
    const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) return false;
    buffer[sizeof(buffer) - 1] = '\0';
    return std::strcmp(buffer, kTargetPackage) == 0;
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
    char buffer[1024]{};
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    __android_log_write(priority, kTag, buffer);
    fileLog(buffer);
}

int libraryCallback(dl_phdr_info *info, size_t, void *opaque) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, kTargetLibrary) == nullptr) return 0;
    auto *result = static_cast<LibraryInfo *>(opaque);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->executableRangeCount = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0 || (phdr.p_flags & PF_X) == 0) continue;
        if (result->executableRangeCount >= result->executableRanges.size()) break;
        result->executableRanges[result->executableRangeCount++] = {
                result->base + static_cast<uintptr_t>(phdr.p_vaddr),
                static_cast<size_t>(phdr.p_memsz)};
    }
    return 1;
}

LibraryInfo findLauncherLibrary() {
    LibraryInfo result;
    dl_iterate_phdr(libraryCallback, &result);
    return result;
}

template <size_t N>
bool patternMatches(uintptr_t address, const PatternWord (&pattern)[N]) {
    for (size_t i = 0; i < N; ++i) {
        uint32_t word = 0;
        std::memcpy(&word, reinterpret_cast<const void *>(address + i * sizeof(uint32_t)), sizeof(word));
        if ((word & pattern[i].mask) != (pattern[i].value & pattern[i].mask)) return false;
    }
    return true;
}

template <size_t N>
uintptr_t resolveUnique(const LibraryInfo &library, const PatternWord (&pattern)[N], size_t *matchCount) {
    uintptr_t found = 0;
    size_t matches = 0;
    const size_t patternBytes = N * sizeof(uint32_t);
    for (size_t rangeIndex = 0; rangeIndex < library.executableRangeCount; ++rangeIndex) {
        const ExecutableRange &range = library.executableRanges[rangeIndex];
        if (range.start == 0 || range.size < patternBytes) continue;
        const uintptr_t first = (range.start + 3U) & ~static_cast<uintptr_t>(3U);
        const uintptr_t last = range.start + range.size - patternBytes;
        for (uintptr_t cursor = first; cursor <= last; cursor += sizeof(uint32_t)) {
            if (!patternMatches(cursor, pattern)) continue;
            ++matches;
            found = matches == 1 ? cursor : 0;
        }
    }
    if (matchCount != nullptr) *matchCount = matches;
    return matches == 1 ? found : 0;
}

bool readHead(uintptr_t target, std::array<uint8_t, kProbeSize> *out) {
    if (target == 0 || out == nullptr) return false;
    std::memcpy(out->data(), reinterpret_cast<const void *>(target), out->size());
    return true;
}

bool headsEqual(const std::array<uint8_t, kProbeSize> &a,
                const std::array<uint8_t, kProbeSize> &b) {
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

bool betaEnabled() {
    swipegate_control_sync_if_due();
    return swipegate_control_break_open_enabled() == 1;
}

uint32_t mergeSupportHook() {
    const auto original = reinterpret_cast<MergeSupportFn>(
            gOriginalMergeSupport.load(std::memory_order_acquire));
    const uint32_t stock = original == nullptr ? 0U : (original() & 1U);
    return betaEnabled() ? 1U : stock;
}

bool installHookLocked(const char *source) {
    const HookFunType hookFunction = gHookFunction.load(std::memory_order_acquire);
    const uintptr_t target = gMergeTarget.load(std::memory_order_acquire);
    if (hookFunction == nullptr || target == 0 || !gResolverReady.load(std::memory_order_acquire)) return false;
    if (gMergeMatches.load(std::memory_order_acquire) != 1
            || gCanUseMatches.load(std::memory_order_acquire) != 1
            || gHandleMatches.load(std::memory_order_acquire) != 1) {
        return false;
    }

    std::array<uint8_t, kProbeSize> current{};
    if (!readHead(target, &current)) return false;
    if (!gOriginalHeadReady) {
        gOriginalHead = current;
        gOriginalHeadReady = true;
    } else if (!headsEqual(current, gOriginalHead)) {
        logLine(ANDROID_LOG_ERROR,
                "BREAK_OPEN_HEALTH install refused source=%s reason=foreign-entry target=%p failClosed=1",
                source, reinterpret_cast<void *>(target));
        return false;
    }

    void *backup = nullptr;
    const int rc = hookFunction(reinterpret_cast<void *>(target),
                                reinterpret_cast<void *>(mergeSupportHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "BREAK_OPEN_HEALTH hook failed source=%s rc=%d target=%p backup=%p",
                source, rc, reinterpret_cast<void *>(target), backup);
        return false;
    }

    gOriginalMergeSupport.store(backup, std::memory_order_release);
    std::array<uint8_t, kProbeSize> patched{};
    if (!readHead(target, &patched) || headsEqual(patched, gOriginalHead)) {
        gHookInstalled.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_ERROR,
                "BREAK_OPEN_HEALTH hook backend returned success but entry stayed stock source=%s target=%p",
                source, reinterpret_cast<void *>(target));
        return false;
    }
    gPatchedHead = patched;
    gPatchedHeadReady = true;
    gHookInstalled.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "BREAK_OPEN_HEALTH hook installed source=%s target=%p beta=%d functionScoped=1 backingFlagUntouched=1",
            source, reinterpret_cast<void *>(target), betaEnabled() ? 1 : 0);
    return true;
}

void maintainHook(const char *source) {
    if (!isLauncherProcess() || !gResolverReady.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(gHookMutex);

    const uintptr_t target = gMergeTarget.load(std::memory_order_acquire);
    if (target == 0) return;

    std::array<uint8_t, kProbeSize> current{};
    if (!readHead(target, &current)) return;

    if (!gHookInstalled.load(std::memory_order_acquire)) {
        if (!betaEnabled()) return;
        installHookLocked(source);
        return;
    }

    if (gPatchedHeadReady && headsEqual(current, gPatchedHead)) {
        const int64_t now = monotonicMs();
        int64_t last = gLastHealthyLogMs.load(std::memory_order_relaxed);
        if (now - last >= kHealthyLogIntervalMs
                && gLastHealthyLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            logLine(ANDROID_LOG_INFO,
                    "BREAK_OPEN_HEALTH healthy beta=%d target=%p functionScoped=1 backingFlagUntouched=1",
                    betaEnabled() ? 1 : 0, reinterpret_cast<void *>(target));
        }
        return;
    }

    if (gOriginalHeadReady && headsEqual(current, gOriginalHead)) {
        gHookInstalled.store(false, std::memory_order_release);
        gPatchedHeadReady = false;
        gOriginalMergeSupport.store(nullptr, std::memory_order_release);
        logLine(ANDROID_LOG_WARN,
                "BREAK_OPEN_HEALTH original bytes restored source=%s target=%p; rehooking",
                source, reinterpret_cast<void *>(target));
        if (!betaEnabled()) return;

        const UnhookFunType unhookFunction = gUnhookFunction.load(std::memory_order_acquire);
        if (unhookFunction != nullptr) {
            const int rc = unhookFunction(reinterpret_cast<void *>(target));
            logLine(rc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN,
                    "BREAK_OPEN_HEALTH stale-backend cleanup rc=%d target=%p",
                    rc, reinterpret_cast<void *>(target));
        }
        if (installHookLocked("repair-after-restore")) {
            logLine(ANDROID_LOG_INFO,
                    "BREAK_OPEN_HEALTH rehook succeeded target=%p",
                    reinterpret_cast<void *>(target));
        }
        return;
    }

    gHookInstalled.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_ERROR,
            "BREAK_OPEN_HEALTH foreign patch detected source=%s target=%p; refusing repair failClosed=1",
            source, reinterpret_cast<void *>(target));
}

void resolverWorker() {
    if (!isLauncherProcess()) return;

    LibraryInfo library;
    for (int attempt = 0; attempt < 120; ++attempt) {
        library = findLauncherLibrary();
        if (library.base != 0 && library.executableRangeCount != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (library.base == 0 || library.executableRangeCount == 0) return;

    size_t mergeMatches = 0;
    size_t canUseMatches = 0;
    size_t handleMatches = 0;
    const uintptr_t mergeTarget = resolveUnique(library, kMergeSupportPattern, &mergeMatches);
    const uintptr_t canUseTarget = resolveUnique(library, kCanUseBreakOpenPattern, &canUseMatches);
    const uintptr_t handleTarget = resolveUnique(library, kHandleBackGesturePattern, &handleMatches);

    gLauncherBase.store(library.base, std::memory_order_release);
    gMergeTarget.store(mergeTarget, std::memory_order_release);
    gMergeMatches.store(mergeMatches, std::memory_order_release);
    gCanUseMatches.store(canUseMatches, std::memory_order_release);
    gHandleMatches.store(handleMatches, std::memory_order_release);

    if (mergeTarget == 0 || canUseTarget == 0 || handleTarget == 0
            || mergeMatches != 1 || canUseMatches != 1 || handleMatches != 1) {
        logLine(ANDROID_LOG_WARN,
                "BREAK_OPEN_HEALTH resolver refused matches=%zu/%zu/%zu failClosed=1",
                mergeMatches, canUseMatches, handleMatches);
        return;
    }

    std::array<uint8_t, kProbeSize> original{};
    if (!readHead(mergeTarget, &original)) return;
    {
        std::lock_guard<std::mutex> lock(gHookMutex);
        gOriginalHead = original;
        gOriginalHeadReady = true;
    }
    gResolverReady.store(true, std::memory_order_release);

    logLine(ANDROID_LOG_INFO,
            "BREAK_OPEN_HEALTH resolver ready mergeOffset=0x%zx canUseOffset=0x%zx handleOffset=0x%zx matches=1/1/1 functionScoped=1 backingFlagUntouched=1",
            static_cast<size_t>(mergeTarget - library.base),
            static_cast<size_t>(canUseTarget - library.base),
            static_cast<size_t>(handleTarget - library.base));
    maintainHook("resolver-ready");
}

void ensureResolverStarted() {
    bool expected = false;
    if (!gResolverStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    std::thread(resolverWorker).detach();
}

}  // namespace

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_enable(
        HookFunType hookFunction, UnhookFunType unhookFunction) {
    if (!isLauncherProcess() || hookFunction == nullptr) return;
    gHookFunction.store(hookFunction, std::memory_order_release);
    gUnhookFunction.store(unhookFunction, std::memory_order_release);
    ensureResolverStarted();
}

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_maintain() {
    ensureResolverStarted();
    maintainHook("main-watchdog");
}

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_probe_on_swipe(
        uint32_t, uint32_t, float) {
    ensureResolverStarted();
    maintainHook("swipe");
}
