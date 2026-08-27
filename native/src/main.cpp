#include "native_api.h"

#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateNative";
constexpr const char *kTargetPackage = "com.miui.home";
constexpr const char *kSpawnerPath = "/system_ext/bin/hyos_spawner";
constexpr const char *kTargetLibrary = "libapp_launcher.so";

// Exact target used only for this read-only reverse-engineering probe:
// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R.
// The address and prologue were recovered from this exact library's
// .gnu_debugdata local Rust symbols and already verified on-device.
constexpr uintptr_t kOnSwipeProcessOffset = 0x816fc4;
constexpr uint8_t kOnSwipeProcessSignature[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};

// Old 8.x builds show on_swipe_process as roughly 0x1544 bytes. Do not assume
// that size for 5459: dump a bounded 0x2000-byte RX window instead, then locate
// the actual function end from the exact 5459 instructions offline.
constexpr size_t kProbeBytes = 0x2000;
constexpr size_t kBytesPerLogLine = 256;

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gProbeDone{false};

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
    uintptr_t probeRxStart = 0;
    uintptr_t probeRxEnd = 0;
    std::string path;
};

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    const std::string path(info->dlpi_name);
    if (path.find(kTargetLibrary) == std::string::npos) return 0;

    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->path = path;

    const uintptr_t target = result->base + kOnSwipeProcessOffset;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0) continue;
        const uintptr_t start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);
        const uintptr_t end = start + static_cast<uintptr_t>(phdr.p_memsz);
        if (target >= start && target < end) {
            result->probeRxStart = start;
            result->probeRxEnd = end;
            break;
        }
    }
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

void dumpExact5459SwipeCode(const LibraryInfo &library) {
    if (gProbeDone.exchange(true, std::memory_order_acq_rel)) return;
    if (library.base == 0 || library.probeRxEnd == 0) {
        logLine(ANDROID_LOG_ERROR,
                "CODE_PROBE no executable segment for on_swipe_process base=%p",
                reinterpret_cast<void *>(library.base));
        return;
    }

    const uintptr_t target = library.base + kOnSwipeProcessOffset;
    if (!matchesSignature(target, kOnSwipeProcessSignature,
                          sizeof(kOnSwipeProcessSignature))) {
        logLine(ANDROID_LOG_ERROR,
                "CODE_PROBE exact-5459 signature mismatch base=%p target=%p; dump skipped",
                reinterpret_cast<void *>(library.base),
                reinterpret_cast<void *>(target));
        return;
    }

    const size_t rxRemaining = static_cast<size_t>(library.probeRxEnd - target);
    const size_t dumpBytes = std::min(kProbeBytes, rxRemaining);
    const auto *bytes = reinterpret_cast<const uint8_t *>(target);

    logLine(ANDROID_LOG_INFO,
            "CODE_PROBE_BEGIN version=RELEASE-8.01.02.5459 offset=0x%llx target=%p rxStart=%p rxEnd=%p bytes=%zu monotonicMs=%lld",
            static_cast<unsigned long long>(kOnSwipeProcessOffset),
            reinterpret_cast<void *>(target),
            reinterpret_cast<void *>(library.probeRxStart),
            reinterpret_cast<void *>(library.probeRxEnd), dumpBytes,
            static_cast<long long>(monotonicMs()));

    static constexpr char kHex[] = "0123456789abcdef";
    char hex[kBytesPerLogLine * 2 + 1]{};
    for (size_t offset = 0; offset < dumpBytes; offset += kBytesPerLogLine) {
        const size_t count = std::min(kBytesPerLogLine, dumpBytes - offset);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t value = bytes[offset + i];
            hex[i * 2] = kHex[value >> 4];
            hex[i * 2 + 1] = kHex[value & 0x0f];
        }
        hex[count * 2] = '\0';
        logLine(ANDROID_LOG_INFO,
                "CODE_PROBE off=0x%04zx len=%zu hex=%s", offset, count, hex);
    }

    logLine(ANDROID_LOG_INFO,
            "CODE_PROBE_END bytes=%zu lines=%zu",
            dumpBytes,
            (dumpBytes + kBytesPerLogLine - 1) / kBytesPerLogLine);
}

void inspectLibrary(const LibraryInfo &library) {
    if (library.base == 0) return;
    logLine(ANDROID_LOG_INFO, "CODE_PROBE found %s base=%p",
            library.path.c_str(), reinterpret_cast<void *>(library.base));
    dumpExact5459SwipeCode(library);
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) {
            logLine(ANDROID_LOG_WARN, "CODE_PROBE worker left target process; aborting");
            return;
        }
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            inspectLibrary(library);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "CODE_PROBE timed out waiting for %s", kTargetLibrary);
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
        if (library.base != 0) inspectLibrary(library);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) return nullptr;

    const std::string exe = readExecutable();
    const std::string process = readProcessName();
    logLine(ANDROID_LOG_INFO,
            "CODE_PROBE native_init candidate api=%u exe=%s process=%s",
            entries->version, exe.c_str(), process.c_str());
    if (exe != kSpawnerPath || process != kTargetPackage) return nullptr;

    logLine(ANDROID_LOG_INFO,
            "CODE_PROBE native_init accepted; read-only probe, no hook installed");
    ensureWorkerStarted();
    return onLibraryLoaded;
}
