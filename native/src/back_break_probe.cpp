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
#include <thread>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr const char *kPersistedBreakOpenFile = "hyperos4swipegate_break_open";
constexpr size_t kMaxRanges = 16;
constexpr int kAndroidUserOffset = 100000;
constexpr int64_t kFlagGuardIntervalMs = 100;
constexpr int64_t kStatusLogIntervalMs = 30000;

struct Range { uintptr_t start = 0; size_t size = 0; };
struct LibraryInfo {
    uintptr_t base = 0;
    std::array<Range, kMaxRanges> executableRanges{};
    size_t executableRangeCount = 0;
    std::array<Range, kMaxRanges> writableRanges{};
    size_t writableRangeCount = 0;
};
struct PatternWord { uint32_t value; uint32_t mask; };

constexpr uint32_t kMaskAdrp = 0x9f00001fU;
constexpr uint32_t kMaskAddImm12 = 0xffc003ffU;
constexpr uint32_t kMaskCompareBranchImm = 0xff00001fU;

constexpr PatternWord kMergeSupportPattern[] = {
        {0x90004288U, kMaskAdrp}, {0x9133a108U, 0xffffffffU},
        {0x88dffd08U, 0xffffffffU}, {0x350000c8U, kMaskCompareBranchImm},
        {0x90004288U, kMaskAdrp}, {0x39738108U, 0xffffffffU},
        {0x7100011fU, 0xffffffffU}, {0x1a9f07e0U, 0xffffffffU},
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

enum class FlagKind : int { None = 0, FeatureByte = 1, RuntimeAtomic = 2 };

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gResolverReady{false};
std::atomic<uintptr_t> gLauncherBase{0};
std::atomic<uintptr_t> gMergeTarget{0};
std::atomic<uintptr_t> gSelectedFlag{0};
std::atomic<int> gFlagKind{static_cast<int>(FlagKind::None)};
std::atomic<uint32_t> gStockFlagValue{0};
std::atomic<bool> gFlagApplied{false};
std::atomic<int64_t> gLastStatusLogMs{0};

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
    return std::strcmp(buffer, kTargetPackage) == 0;
}

void fileLog(const char *message) {
    const int userId = static_cast<int>(getuid()) / kAndroidUserOffset;
    char path[256]{};
    const char *formats[] = {
            "/data/user_de/%d/com.miui.home/cache/hyperos4swipegate_native.log",
            "/data/user/%d/com.miui.home/cache/hyperos4swipegate_native.log",
            "/data/data/com.miui.home/cache/hyperos4swipegate_native.log",
    };
    for (size_t i = 0; i < 3; ++i) {
        if (i == 2 && userId != 0) break;
        if (i < 2) std::snprintf(path, sizeof(path), formats[i], userId);
        else std::snprintf(path, sizeof(path), "%s", formats[i]);
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
    auto *out = static_cast<LibraryInfo *>(opaque);
    out->base = static_cast<uintptr_t>(info->dlpi_addr);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;
        const Range range{out->base + static_cast<uintptr_t>(phdr.p_vaddr),
                          static_cast<size_t>(phdr.p_memsz)};
        if ((phdr.p_flags & PF_X) != 0 && out->executableRangeCount < kMaxRanges)
            out->executableRanges[out->executableRangeCount++] = range;
        if ((phdr.p_flags & PF_W) != 0 && out->writableRangeCount < kMaxRanges)
            out->writableRanges[out->writableRangeCount++] = range;
    }
    return 1;
}

LibraryInfo findLauncherLibrary() {
    LibraryInfo result;
    dl_iterate_phdr(libraryCallback, &result);
    return result;
}

bool contains(const Range &range, uintptr_t address, size_t size) {
    if (range.start == 0 || range.size == 0 || address < range.start) return false;
    const uintptr_t end = address + size;
    return end >= address && end <= range.start + range.size;
}

bool isWritable(const LibraryInfo &library, uintptr_t address, size_t size) {
    for (size_t i = 0; i < library.writableRangeCount; ++i)
        if (contains(library.writableRanges[i], address, size)) return true;
    return false;
}

template <size_t N>
bool patternMatches(uintptr_t address, const PatternWord (&pattern)[N]) {
    for (size_t i = 0; i < N; ++i) {
        uint32_t word = 0;
        std::memcpy(&word, reinterpret_cast<const void *>(address + i * 4U), 4);
        if ((word & pattern[i].mask) != (pattern[i].value & pattern[i].mask)) return false;
    }
    return true;
}

template <size_t N>
uintptr_t resolveUnique(const LibraryInfo &library, const PatternWord (&pattern)[N], size_t *countOut) {
    uintptr_t found = 0;
    size_t count = 0;
    const size_t bytes = N * 4U;
    for (size_t r = 0; r < library.executableRangeCount; ++r) {
        const Range &range = library.executableRanges[r];
        if (range.size < bytes) continue;
        const uintptr_t first = (range.start + 3U) & ~static_cast<uintptr_t>(3U);
        const uintptr_t last = range.start + range.size - bytes;
        for (uintptr_t p = first; p <= last; p += 4U) {
            if (!patternMatches(p, pattern)) continue;
            ++count;
            found = count == 1 ? p : 0;
        }
    }
    if (countOut != nullptr) *countOut = count;
    return count == 1 ? found : 0;
}

bool decodeAdrpPage(uintptr_t pc, uint32_t insn, uintptr_t *pageOut) {
    if (pageOut == nullptr || (insn & 0x9f00001fU) != 0x90000008U) return false;
    int64_t imm21 = static_cast<int64_t>(((insn >> 5) & 0x7ffffU) << 2)
            | static_cast<int64_t>((insn >> 29) & 0x3U);
    if ((imm21 & (1LL << 20)) != 0) imm21 |= ~((1LL << 21) - 1LL);
    const int64_t page = static_cast<int64_t>(pc & ~static_cast<uintptr_t>(0xfffU)) + (imm21 << 12);
    if (page <= 0) return false;
    *pageOut = static_cast<uintptr_t>(page);
    return true;
}

bool decodeRuntimeAtomicFlag(uintptr_t target, uintptr_t *out) {
    uint32_t adrp = 0, add = 0;
    std::memcpy(&adrp, reinterpret_cast<const void *>(target), 4);
    std::memcpy(&add, reinterpret_cast<const void *>(target + 4U), 4);
    uintptr_t page = 0;
    if (out == nullptr || !decodeAdrpPage(target, adrp, &page)) return false;
    if ((add & 0xff0003ffU) != 0x91000108U) return false;
    const uintptr_t imm12 = (add >> 10) & 0xfffU;
    const uintptr_t shift = ((add >> 22) & 1U) != 0 ? 12U : 0U;
    *out = page + (imm12 << shift);
    return true;
}

bool decodeFeatureByteFlag(uintptr_t target, uintptr_t *out) {
    uint32_t adrp = 0, ldrb = 0;
    std::memcpy(&adrp, reinterpret_cast<const void *>(target + 16U), 4);
    std::memcpy(&ldrb, reinterpret_cast<const void *>(target + 20U), 4);
    uintptr_t page = 0;
    if (out == nullptr || !decodeAdrpPage(target + 16U, adrp, &page)) return false;
    if ((ldrb & 0xffc003ffU) != 0x39400108U) return false;
    *out = page + static_cast<uintptr_t>((ldrb >> 10) & 0xfffU);
    return true;
}

uint32_t readFlag(uintptr_t address, FlagKind kind) {
    if (kind == FlagKind::FeatureByte)
        return __atomic_load_n(reinterpret_cast<uint8_t *>(address), __ATOMIC_ACQUIRE);
    if (kind == FlagKind::RuntimeAtomic)
        return __atomic_load_n(reinterpret_cast<uint32_t *>(address), __ATOMIC_ACQUIRE);
    return 0;
}

void writeFlag(uintptr_t address, FlagKind kind, uint32_t value) {
    if (kind == FlagKind::FeatureByte)
        __atomic_store_n(reinterpret_cast<uint8_t *>(address), static_cast<uint8_t>(value), __ATOMIC_RELEASE);
    else if (kind == FlagKind::RuntimeAtomic)
        __atomic_store_n(reinterpret_cast<uint32_t *>(address), value, __ATOMIC_RELEASE);
}

int readPersistedBreakOpen() {
    const int userId = static_cast<int>(getuid()) / kAndroidUserOffset;
    char path[256]{};
    const char *formats[] = {
            "/data/user_de/%d/com.miui.home/cache/%s",
            "/data/user/%d/com.miui.home/cache/%s",
            "/data/data/com.miui.home/cache/%s",
    };
    for (size_t i = 0; i < 3; ++i) {
        if (i == 2 && userId != 0) break;
        if (i < 2) std::snprintf(path, sizeof(path), formats[i], userId, kPersistedBreakOpenFile);
        else std::snprintf(path, sizeof(path), formats[i], kPersistedBreakOpenFile);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        char buffer[16]{};
        const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (n > 0) return buffer[0] == '1' ? 1 : 0;
    }
    return 0;
}

bool desiredEnabled() {
    swipegate_control_sync_if_due();
    const int live = swipegate_control_break_open_enabled();
    return live >= 0 ? live == 1 : readPersistedBreakOpen() == 1;
}

void applyFlagPolicy(bool fromSwipe) {
    if (!gResolverReady.load(std::memory_order_acquire)) return;
    const uintptr_t address = gSelectedFlag.load(std::memory_order_acquire);
    const FlagKind kind = static_cast<FlagKind>(gFlagKind.load(std::memory_order_acquire));
    if (address == 0 || kind == FlagKind::None) return;
    const bool enabled = desiredEnabled();
    const uint32_t stock = gStockFlagValue.load(std::memory_order_acquire);
    const uint32_t current = readFlag(address, kind);
    const bool applied = gFlagApplied.load(std::memory_order_acquire);

    if (enabled) {
        if (current != 1U) {
            writeFlag(address, kind, 1U);
            logLine(applied ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
                    applied ? "BREAK_OPEN_FLAG restored by Launcher observed=%u reasserted=1 kind=%s source=%s"
                            : "BREAK_OPEN_FLAG enabled old=%u new=1 kind=%s source=%s",
                    current, kind == FlagKind::FeatureByte ? "feature-byte" : "runtime-atomic",
                    fromSwipe ? "swipe" : "guard");
        }
        gFlagApplied.store(true, std::memory_order_release);
    } else if (applied) {
        if (current != stock) writeFlag(address, kind, stock);
        gFlagApplied.store(false, std::memory_order_release);
        logLine(ANDROID_LOG_INFO, "BREAK_OPEN_FLAG disabled restored=%u kind=%s", stock,
                kind == FlagKind::FeatureByte ? "feature-byte" : "runtime-atomic");
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastStatusLogMs.load(std::memory_order_relaxed);
    if (now - last >= kStatusLogIntervalMs
            && gLastStatusLogMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        const uintptr_t base = gLauncherBase.load(std::memory_order_acquire);
        logLine(ANDROID_LOG_INFO,
                "BREAK_OPEN_FLAG healthy enabled=%d value=%u kind=%s targetOffset=0x%zx flagOffset=0x%zx",
                enabled ? 1 : 0, readFlag(address, kind),
                kind == FlagKind::FeatureByte ? "feature-byte" : "runtime-atomic",
                static_cast<size_t>(gMergeTarget.load(std::memory_order_acquire) - base),
                static_cast<size_t>(address - base));
    }
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

    size_t mergeCount = 0, canUseCount = 0, handleCount = 0;
    const uintptr_t merge = resolveUnique(library, kMergeSupportPattern, &mergeCount);
    const uintptr_t canUse = resolveUnique(library, kCanUseBreakOpenPattern, &canUseCount);
    const uintptr_t handle = resolveUnique(library, kHandleBackGesturePattern, &handleCount);
    if (merge == 0 || canUse == 0 || handle == 0 || mergeCount != 1 || canUseCount != 1 || handleCount != 1) {
        logLine(ANDROID_LOG_WARN, "BREAK_OPEN_FLAG resolver refused matches=%zu/%zu/%zu failClosed=1",
                mergeCount, canUseCount, handleCount);
        return;
    }

    uintptr_t runtimeFlag = 0, featureFlag = 0;
    const bool runtimeDecoded = decodeRuntimeAtomicFlag(merge, &runtimeFlag);
    const bool featureDecoded = decodeFeatureByteFlag(merge, &featureFlag);
    const bool runtimeWritable = runtimeDecoded && isWritable(library, runtimeFlag, sizeof(uint32_t));
    const bool featureWritable = featureDecoded && isWritable(library, featureFlag, sizeof(uint8_t));

    uintptr_t selected = 0;
    FlagKind kind = FlagKind::None;
    if (featureWritable) { selected = featureFlag; kind = FlagKind::FeatureByte; }
    else if (runtimeWritable) { selected = runtimeFlag; kind = FlagKind::RuntimeAtomic; }
    if (selected == 0) {
        logLine(ANDROID_LOG_WARN,
                "BREAK_OPEN_FLAG no writable backing flag runtime=%p writable=%d feature=%p writable=%d failClosed=1",
                reinterpret_cast<void *>(runtimeFlag), runtimeWritable ? 1 : 0,
                reinterpret_cast<void *>(featureFlag), featureWritable ? 1 : 0);
        return;
    }

    const uint32_t stock = readFlag(selected, kind);
    gLauncherBase.store(library.base, std::memory_order_release);
    gMergeTarget.store(merge, std::memory_order_release);
    gSelectedFlag.store(selected, std::memory_order_release);
    gFlagKind.store(static_cast<int>(kind), std::memory_order_release);
    gStockFlagValue.store(stock, std::memory_order_release);
    gResolverReady.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "BREAK_OPEN_FLAG resolver ready mergeOffset=0x%zx canUseOffset=0x%zx handleOffset=0x%zx runtimeFlagOffset=0x%zx featureFlagOffset=0x%zx selected=%s stock=%u noInlineHook=1",
            static_cast<size_t>(merge - library.base), static_cast<size_t>(canUse - library.base),
            static_cast<size_t>(handle - library.base),
            runtimeDecoded ? static_cast<size_t>(runtimeFlag - library.base) : 0U,
            featureDecoded ? static_cast<size_t>(featureFlag - library.base) : 0U,
            kind == FlagKind::FeatureByte ? "feature-byte" : "runtime-atomic", stock);

    while (isLauncherProcess()) {
        applyFlagPolicy(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(kFlagGuardIntervalMs));
    }
}

void ensureWorkerStarted() {
    bool expected = false;
    if (!gWorkerStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    std::thread(resolverWorker).detach();
}

}  // namespace

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_enable(HookFunType) {
    if (isLauncherProcess()) ensureWorkerStarted();
}

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_probe_on_swipe(
        uint32_t, uint32_t, float) {
    if (!isLauncherProcess()) return;
    ensureWorkerStarted();
    applyFlagPolicy(true);
}
