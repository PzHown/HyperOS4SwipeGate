#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kTargetLibrary = "libapp_launcher.so";
constexpr size_t kMaxExecutableRanges = 16;
constexpr int64_t kNewGestureGapMs = 450;

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

// 6174 reference fingerprints. They intentionally mask only PC-relative fields that
// are expected to move between nearby Launcher builds. Every pattern was verified to
// be unique inside the 6174 executable ranges. Unknown layouts fail closed.
constexpr uint32_t kMaskAdrp = 0x9f00001fU;
constexpr uint32_t kMaskAddImm12 = 0xffc003ffU;
constexpr uint32_t kMaskCompareBranchImm = 0xff00001fU;

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
        {0xd10303ffU, 0xffffffffU},
        {0xa9087bfdU, 0xffffffffU},
        {0xf9004bf7U, 0xffffffffU},
        {0xa90a57f6U, 0xffffffffU},
        {0xa90b4ff4U, 0xffffffffU},
        {0x910203fdU, 0xffffffffU},
        {0xf0ffd3a8U, kMaskAdrp},
        {0x9125e908U, kMaskAddImm12},
        {0xb0ffcfe9U, kMaskAdrp},
        {0x91376929U, kMaskAddImm12},
        {0x7100001fU, 0xffffffffU},
        {0x528000eaU, 0xffffffffU},
        {0x528000abU, 0xffffffffU},
        {0x9a881128U, 0xffffffffU},
        {0x90003436U, kMaskAdrp},
        {0x9a8a1169U, 0xffffffffU},
        {0x2a0003f3U, 0xffffffffU},
        {0xa90027e8U, 0xffffffffU},
};

constexpr PatternWord kHandleBackGesturePattern[] = {
        {0x6db82bebU, 0xffffffffU},
        {0x6d0123e9U, 0xffffffffU},
        {0xa9027bfdU, 0xffffffffU},
        {0xa9036ffcU, 0xffffffffU},
        {0xa90467faU, 0xffffffffU},
        {0xa9055ff8U, 0xffffffffU},
        {0xa90657f6U, 0xffffffffU},
        {0xa9074ff4U, 0xffffffffU},
        {0x910083fdU, 0xffffffffU},
        {0xd107c3ffU, 0xffffffffU},
};

std::atomic<bool> gResolverStarted{false};
std::atomic<bool> gResolverReady{false};
std::atomic<uintptr_t> gLauncherBase{0};
std::atomic<uintptr_t> gMergeSupportTarget{0};
std::atomic<uintptr_t> gCanUseBreakOpenTarget{0};
std::atomic<uintptr_t> gHandleBackGestureTarget{0};
std::atomic<size_t> gMergeSupportMatches{0};
std::atomic<size_t> gCanUseBreakOpenMatches{0};
std::atomic<size_t> gHandleBackGestureMatches{0};
std::atomic<int64_t> gLastSwipeAtMs{0};
std::atomic<uint64_t> gProbeSequence{0};

int64_t monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

bool isLauncherProcess() {
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
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

void probeLog(int priority, const char *format, ...) {
    char buffer[1024]{};
    va_list ap;
    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    __android_log_write(priority, kTag, buffer);
    fileLog(buffer);
}

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, kTargetLibrary) == nullptr) return 0;

    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->executableRangeCount = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0 || (phdr.p_flags & PF_X) == 0) continue;
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
            if (matches == 1) found = cursor;
            if (matches > 1) found = 0;
        }
    }
    if (matchCount != nullptr) *matchCount = matches;
    return matches == 1 ? found : 0;
}

void resolverWorker() {
    if (!isLauncherProcess()) return;

    LibraryInfo library;
    for (int attempt = 0; attempt < 60; ++attempt) {
        library = findLauncherLibrary();
        if (library.base != 0 && library.executableRangeCount != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (library.base == 0 || library.executableRangeCount == 0) return;

    size_t mergeMatches = 0;
    size_t canUseMatches = 0;
    size_t handleMatches = 0;
    const uintptr_t mergeTarget = resolveUnique(library, kMergeSupportPattern, &mergeMatches);
    const uintptr_t canUseTarget = resolveUnique(library, kCanUseBreakOpenPattern, &canUseMatches);
    const uintptr_t handleTarget = resolveUnique(library, kHandleBackGesturePattern, &handleMatches);

    gLauncherBase.store(library.base, std::memory_order_release);
    gMergeSupportMatches.store(mergeMatches, std::memory_order_release);
    gCanUseBreakOpenMatches.store(canUseMatches, std::memory_order_release);
    gHandleBackGestureMatches.store(handleMatches, std::memory_order_release);
    gMergeSupportTarget.store(mergeTarget, std::memory_order_release);
    gCanUseBreakOpenTarget.store(canUseTarget, std::memory_order_release);
    gHandleBackGestureTarget.store(handleTarget, std::memory_order_release);
    gResolverReady.store(true, std::memory_order_release);

    probeLog(ANDROID_LOG_INFO,
             "BREAK_OPEN_PROBE resolver ready base=%p mergeTarget=%p mergeMatches=%zu canUseTarget=%p canUseMatches=%zu handleTarget=%p handleMatches=%zu observeOnly=1",
             reinterpret_cast<void *>(library.base), reinterpret_cast<void *>(mergeTarget), mergeMatches,
             reinterpret_cast<void *>(canUseTarget), canUseMatches,
             reinterpret_cast<void *>(handleTarget), handleMatches);
}

void ensureResolverStarted() {
    bool expected = false;
    if (!gResolverStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    std::thread(resolverWorker).detach();
}

__attribute__((constructor)) void startBackBreakProbeResolver() {
    ensureResolverStarted();
}

}  // namespace

extern "C" __attribute__((visibility("hidden"))) void swipegate_back_break_probe_on_swipe(
        uint32_t readyFinish, uint32_t side, float horizontalDistancePx) {
    if (!isLauncherProcess()) return;
    ensureResolverStarted();

    const int64_t now = monotonicMs();
    const int64_t previous = gLastSwipeAtMs.exchange(now, std::memory_order_acq_rel);
    if (previous != 0 && now - previous < kNewGestureGapMs) return;

    const bool ready = gResolverReady.load(std::memory_order_acquire);
    const uintptr_t base = gLauncherBase.load(std::memory_order_acquire);
    const uintptr_t mergeTarget = gMergeSupportTarget.load(std::memory_order_acquire);
    const uintptr_t canUseTarget = gCanUseBreakOpenTarget.load(std::memory_order_acquire);
    const uintptr_t handleTarget = gHandleBackGestureTarget.load(std::memory_order_acquire);

    int mergeSupport = -1;
    if (ready && mergeTarget != 0) {
        using MergeSupportFn = uint32_t (*)();
        const auto fn = reinterpret_cast<MergeSupportFn>(mergeTarget);
        mergeSupport = static_cast<int>(fn() & 1U);
    }

    const uint64_t sequence = gProbeSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    probeLog(ANDROID_LOG_INFO,
             "BREAK_OPEN_PROBE swipe seq=%llu resolverReady=%d readyFinish=%u side=%u dx=%.2f mergeSupport=%d base=%p mergeOffset=0x%zx canUseOffset=0x%zx handleOffset=0x%zx matches=%zu/%zu/%zu observeOnly=1",
             static_cast<unsigned long long>(sequence), ready ? 1 : 0, readyFinish, side,
             horizontalDistancePx, mergeSupport, reinterpret_cast<void *>(base),
             mergeTarget != 0 && base != 0 ? static_cast<size_t>(mergeTarget - base) : 0U,
             canUseTarget != 0 && base != 0 ? static_cast<size_t>(canUseTarget - base) : 0U,
             handleTarget != 0 && base != 0 ? static_cast<size_t>(handleTarget - base) : 0U,
             gMergeSupportMatches.load(std::memory_order_acquire),
             gCanUseBreakOpenMatches.load(std::memory_order_acquire),
             gHandleBackGestureMatches.load(std::memory_order_acquire));
}
