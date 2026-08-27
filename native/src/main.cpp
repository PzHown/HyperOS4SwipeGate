#include "native_api.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cmath>
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
constexpr const char *kThresholdProperty = "persist.hyperos4swipegate.threshold";
constexpr int kDefaultThreshold = 55;

// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R
// Offsets recovered from libapp_launcher.so .gnu_debugdata local Rust symbols.
constexpr uintptr_t kOnSwipeProcessOffset = 0x816fc4;
constexpr uintptr_t kPauseDetectorResetOffset = 0x783aa8;
constexpr uintptr_t kGetScreenDimensionsOffset = 0x77ae6c;
constexpr uintptr_t kPauseDetectorOffsetInBackHelper = 0x178;

constexpr uint8_t kOnSwipeProcessSignature[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};
constexpr uint8_t kPauseResetSignature[] = {
        0xff, 0x83, 0x02, 0xd1, 0xfd, 0x7b, 0x08, 0xa9,
        0xf4, 0x4f, 0x09, 0xa9, 0xfd, 0x03, 0x02, 0x91,
};
constexpr uint8_t kGetScreenDimensionsSignature[] = {
        0xff, 0xc3, 0x03, 0xd1, 0xfd, 0x7b, 0x0b, 0xa9,
        0xf7, 0x63, 0x00, 0xf9, 0xf6, 0x57, 0x0d, 0xa9,
};

using OnSwipeProcessFn = void (*)(void *, bool, uint32_t, const void *, float);
using PauseDetectorResetFn = void (*)(void *);
using GetScreenDimensionsFn = int32_t (*)(void *);
using DisplayManagerGetInstanceFn = void *(*)();
using DisplayManagerGetDisplayFn = void *(*)(void *, int32_t);
using DisplayDropFn = void (*)(void *);

HookFunType gHookFunction = nullptr;
OnSwipeProcessFn gOriginalOnSwipeProcess = nullptr;
PauseDetectorResetFn gPauseDetectorReset = nullptr;
GetScreenDimensionsFn gGetScreenDimensions = nullptr;
DisplayManagerGetInstanceFn gDisplayManagerGetInstance = nullptr;
DisplayManagerGetDisplayFn gDisplayManagerGetDisplay = nullptr;
DisplayDropFn gDisplayDrop = nullptr;

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gHookInstalled{false};
std::atomic<int> gCachedThreshold{kDefaultThreshold};
std::atomic<int64_t> gLastThresholdReadMs{0};
std::atomic<int> gCachedScreenWidth{0};
std::atomic<int64_t> gLastScreenWidthReadMs{0};

int64_t monotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string readSmallFile(const char *path, size_t limit = 512) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string value(limit, '\0');
    ssize_t n = read(fd, value.data(), value.size() - 1);
    close(fd);
    if (n <= 0) return {};
    value.resize(static_cast<size_t>(n));
    size_t zero = value.find('\0');
    if (zero != std::string::npos) value.resize(zero);
    return value;
}

std::string readExecutable() {
    char buffer[256]{};
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
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
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
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
    std::string path(info->dlpi_name);
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
    return address != 0 && std::memcmp(reinterpret_cast<const void *>(address), signature, size) == 0;
}

int readThresholdPercent() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastThresholdReadMs.load(std::memory_order_relaxed);
    if (now - last < 250) return gCachedThreshold.load(std::memory_order_relaxed);

    char value[PROP_VALUE_MAX]{};
    int threshold = kDefaultThreshold;
    if (__system_property_get(kThresholdProperty, value) > 0) {
        char *end = nullptr;
        long parsed = strtol(value, &end, 10);
        if (end != value && *end == '\0') {
            if (parsed < 0) parsed = 0;
            if (parsed > 100) parsed = 100;
            threshold = static_cast<int>(parsed);
        }
    }

    const int previous = gCachedThreshold.exchange(threshold, std::memory_order_relaxed);
    gLastThresholdReadMs.store(now, std::memory_order_relaxed);
    if (previous != threshold) {
        logLine(ANDROID_LOG_INFO, "threshold changed: %d%%", threshold);
    }
    return threshold;
}

bool resolveDisplayApi() {
    if (gDisplayManagerGetInstance && gDisplayManagerGetDisplay && gDisplayDrop) return true;
    gDisplayManagerGetInstance = reinterpret_cast<DisplayManagerGetInstanceFn>(
            dlsym(RTLD_DEFAULT, "DisplayManager_get_instance"));
    gDisplayManagerGetDisplay = reinterpret_cast<DisplayManagerGetDisplayFn>(
            dlsym(RTLD_DEFAULT, "DisplayManager_get_display"));
    gDisplayDrop = reinterpret_cast<DisplayDropFn>(dlsym(RTLD_DEFAULT, "Display_drop"));
    return gDisplayManagerGetInstance && gDisplayManagerGetDisplay && gDisplayDrop;
}

int getScreenWidth() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastScreenWidthReadMs.load(std::memory_order_relaxed);
    int cached = gCachedScreenWidth.load(std::memory_order_relaxed);
    if (cached > 0 && now - last < 1000) return cached;
    if (!gGetScreenDimensions || !resolveDisplayApi()) return cached;

    void *manager = gDisplayManagerGetInstance();
    if (!manager) return cached;
    void *display = gDisplayManagerGetDisplay(manager, 0);
    if (!display) return cached;

    int width = gGetScreenDimensions(display);
    gDisplayDrop(display);
    if (width > 0) {
        gCachedScreenWidth.store(width, std::memory_order_relaxed);
        gLastScreenWidthReadMs.store(now, std::memory_order_relaxed);
        return width;
    }
    return cached;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    const int width = getScreenWidth();
    const int threshold = readThresholdPercent();
    if (self != nullptr && width > 0 && gPauseDetectorReset != nullptr) {
        const float progress = std::fabs(horizontalDistancePx) / static_cast<float>(width);
        if (progress < static_cast<float>(threshold) / 100.0f) {
            void *detector = reinterpret_cast<void *>(
                    reinterpret_cast<uintptr_t>(self) + kPauseDetectorOffsetInBackHelper);
            gPauseDetectorReset(detector);
        }
    }

    if (gOriginalOnSwipeProcess) {
        gOriginalOnSwipeProcess(self, readyFinish, side, point, horizontalDistancePx);
    }
}

bool installHook(uintptr_t base) {
    if (gHookInstalled.load(std::memory_order_acquire)) return true;
    if (base == 0 || gHookFunction == nullptr) return false;

    const uintptr_t onSwipe = base + kOnSwipeProcessOffset;
    const uintptr_t reset = base + kPauseDetectorResetOffset;
    const uintptr_t dimensions = base + kGetScreenDimensionsOffset;

    if (!matchesSignature(onSwipe, kOnSwipeProcessSignature, sizeof(kOnSwipeProcessSignature))
            || !matchesSignature(reset, kPauseResetSignature, sizeof(kPauseResetSignature))
            || !matchesSignature(dimensions, kGetScreenDimensionsSignature,
                                 sizeof(kGetScreenDimensionsSignature))) {
        logLine(ANDROID_LOG_ERROR,
                "launcher signature mismatch; unsupported libapp_launcher.so, hook skipped");
        return false;
    }

    gPauseDetectorReset = reinterpret_cast<PauseDetectorResetFn>(reset);
    gGetScreenDimensions = reinterpret_cast<GetScreenDimensionsFn>(dimensions);
    resolveDisplayApi();

    void *backup = nullptr;
    int rc = gHookFunction(reinterpret_cast<void *>(onSwipe),
                           reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR, "hook_func failed rc=%d backup=%p", rc, backup);
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gHookInstalled.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "hook installed launcher=8.01.02.5459 base=%p defaultThreshold=%d%%",
            reinterpret_cast<void *>(base), readThresholdPercent());
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) return;
        LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "found %s base=%p", library.path.c_str(),
                    reinterpret_cast<void *>(library.base));
            installHook(library.base);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    logLine(ANDROID_LOG_ERROR, "timed out waiting for %s", kTargetLibrary);
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
        LibraryInfo library = findLauncherLibrary();
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
    logLine(ANDROID_LOG_INFO, "native_init api=%u exe=%s process=%s",
            entries->version, readExecutable().c_str(), readProcessName().c_str());
    ensureWorkerStarted();
    return onLibraryLoaded;
}
