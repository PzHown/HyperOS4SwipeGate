#include "native_api.h"

#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
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
constexpr const char *kThresholdProperty = "persist.hyperos4swipegate.threshold";
constexpr int kDefaultThreshold = 55;

// HyperOS 4 System Launcher RELEASE-8.01.02.5459-260807-08242024-R
// Offsets recovered from libapp_launcher.so .gnu_debugdata local Rust symbols.
constexpr uintptr_t kOnSwipeProcessOffset = 0x816fc4;
constexpr uintptr_t kPauseDetectorResetOffset = 0x783aa8;
constexpr uintptr_t kPauseDetectorOffsetInBackHelper = 0x178;

constexpr uint8_t kOnSwipeProcessSignature[] = {
        0xff, 0x83, 0x05, 0xd1, 0xea, 0x7b, 0x00, 0xfd,
        0xe9, 0xa3, 0x0f, 0x6d, 0xfd, 0xfb, 0x10, 0xa9,
};
constexpr uint8_t kPauseResetSignature[] = {
        0xff, 0x83, 0x02, 0xd1, 0xfd, 0x7b, 0x08, 0xa9,
        0xf4, 0x4f, 0x09, 0xa9, 0xfd, 0x03, 0x02, 0x91,
};

using OnSwipeProcessFn = void (*)(void *, bool, uint32_t, const void *, float);
using PauseDetectorResetFn = void (*)(void *);
using MotionEventIntFn = int32_t (*)(void *);
using MotionEventFloatFn = float (*)(void *);

HookFunType gHookFunction = nullptr;
OnSwipeProcessFn gOriginalOnSwipeProcess = nullptr;
PauseDetectorResetFn gPauseDetectorReset = nullptr;
MotionEventIntFn gOriginalMotionGetAction = nullptr;
MotionEventIntFn gOriginalMotionGetActionMasked = nullptr;
MotionEventFloatFn gMotionGetRawX = nullptr;

std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gSwipeHookInstalled{false};
std::atomic<bool> gMotionHooksInstalled{false};
std::atomic<int> gCachedThreshold{kDefaultThreshold};
std::atomic<int64_t> gLastThresholdReadMs{0};
std::atomic<int> gCachedScreenWidth{0};
std::atomic<int64_t> gLastScreenWidthProbeMs{0};
std::atomic<int64_t> gLastSwipeLogMs{0};
std::atomic<uintptr_t> gLauncherBase{0};
std::atomic<uintptr_t> gLauncherEnd{0};

struct MotionState {
    bool valid = false;
    float downRawX = 0.0f;
    float currentRawX = 0.0f;
    int32_t lastAction = -1;
};

thread_local MotionState gMotionState;

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
    const size_t zero = value.find('\0');
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
    uintptr_t end = 0;
    std::string path;
};

int libraryCallback(dl_phdr_info *info, size_t, void *data) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    std::string path(info->dlpi_name);
    if (path.find(kTargetLibrary) == std::string::npos) return 0;

    uintptr_t end = static_cast<uintptr_t>(info->dlpi_addr);
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) continue;
        const uintptr_t segmentEnd = static_cast<uintptr_t>(info->dlpi_addr)
                + static_cast<uintptr_t>(phdr.p_vaddr)
                + static_cast<uintptr_t>(phdr.p_memsz);
        if (segmentEnd > end) end = segmentEnd;
    }

    auto *result = static_cast<LibraryInfo *>(data);
    result->base = static_cast<uintptr_t>(info->dlpi_addr);
    result->end = end;
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

int readIntegerProperty(const char *name, int minimum, int maximum, int fallback) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) <= 0) return fallback;

    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed < minimum || parsed > maximum) return fallback;
    return static_cast<int>(parsed);
}

int readThresholdPercent() {
    const int64_t now = monotonicMs();
    const int64_t last = gLastThresholdReadMs.load(std::memory_order_relaxed);
    if (now - last < 250) return gCachedThreshold.load(std::memory_order_relaxed);

    const int threshold = readIntegerProperty(
            kThresholdProperty, 0, 100, kDefaultThreshold);
    const int previous = gCachedThreshold.exchange(threshold, std::memory_order_relaxed);
    gLastThresholdReadMs.store(now, std::memory_order_relaxed);
    if (previous != threshold) {
        logLine(ANDROID_LOG_INFO, "threshold changed: %d%%", threshold);
    }
    return threshold;
}

int parseWidth(const std::string &text) {
    const char *cursor = text.c_str();
    while (*cursor != '\0' && (*cursor < '0' || *cursor > '9')) ++cursor;
    if (*cursor == '\0') return 0;
    char *end = nullptr;
    long value = strtol(cursor, &end, 10);
    if (end == cursor || value < 200 || value > 10000) return 0;
    return static_cast<int>(value);
}

int readWidthProperty(const char *name) {
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get(name, value) <= 0) return 0;
    return parseWidth(value);
}

int probeWidthFromProperties() {
    static constexpr const char *properties[] = {
            "persist.sys.miui_resolution",
            "persist.sys.display-size",
            "vendor.display-size",
            "ro.boot.display_resolution",
    };
    for (const char *property : properties) {
        const int width = readWidthProperty(property);
        if (width > 0) {
            logLine(ANDROID_LOG_INFO, "screen width=%d source=property:%s", width, property);
            return width;
        }
    }
    return 0;
}

int probeWidthFromSysfs() {
    static constexpr const char *directPaths[] = {
            "/sys/class/graphics/fb0/virtual_size",
            "/sys/class/drm/card0-DSI-1/modes",
            "/sys/class/drm/card0-DSI-0/modes",
            "/sys/class/drm/card0-DSI-2/modes",
    };
    for (const char *path : directPaths) {
        const int width = parseWidth(readSmallFile(path, 256));
        if (width > 0) {
            logLine(ANDROID_LOG_INFO, "screen width=%d source=sysfs:%s", width, path);
            return width;
        }
    }

    DIR *directory = opendir("/sys/class/drm");
    if (directory == nullptr) return 0;
    int width = 0;
    while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string name(entry->d_name);
        if (name.find("card") != 0 || name.find("DSI") == std::string::npos) continue;
        const std::string path = std::string("/sys/class/drm/") + name + "/modes";
        width = parseWidth(readSmallFile(path.c_str(), 256));
        if (width > 0) {
            logLine(ANDROID_LOG_INFO, "screen width=%d source=sysfs:%s", width, path.c_str());
            break;
        }
    }
    closedir(directory);
    return width;
}

int roundUpTo8(float value) {
    if (!(value >= 200.0f && value <= 10000.0f)) return 0;
    const int integer = static_cast<int>(std::ceil(value));
    return (integer + 7) & ~7;
}

int getScreenWidth() {
    const int cached = gCachedScreenWidth.load(std::memory_order_relaxed);
    if (cached > 0) return cached;

    const int64_t now = monotonicMs();
    const int64_t last = gLastScreenWidthProbeMs.load(std::memory_order_relaxed);
    if (now - last < 1000) return 0;
    gLastScreenWidthProbeMs.store(now, std::memory_order_relaxed);

    int width = probeWidthFromProperties();
    if (width <= 0) width = probeWidthFromSysfs();
    if (width > 0) gCachedScreenWidth.store(width, std::memory_order_relaxed);
    return width;
}

bool callerIsLauncher(uintptr_t caller) {
    const uintptr_t base = gLauncherBase.load(std::memory_order_relaxed);
    const uintptr_t end = gLauncherEnd.load(std::memory_order_relaxed);
    return base != 0 && end > base && caller >= base && caller < end;
}

void captureMotion(void *event, int32_t action, uintptr_t caller) {
    if (event == nullptr || gMotionGetRawX == nullptr || !callerIsLauncher(caller)) return;
    const float rawX = gMotionGetRawX(event);
    if (!(rawX >= 0.0f && rawX <= 10000.0f)) return;

    const int32_t masked = action & 0xff;
    if (masked == 0) {
        gMotionState.valid = true;
        gMotionState.downRawX = rawX;
        gMotionState.currentRawX = rawX;
    } else if (gMotionState.valid) {
        gMotionState.currentRawX = rawX;
    }
    gMotionState.lastAction = masked;
}

int32_t motionGetActionHook(void *event) {
    if (gOriginalMotionGetAction == nullptr) return -1;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(
            __builtin_extract_return_addr(__builtin_return_address(0)));
    const int32_t action = gOriginalMotionGetAction(event);
    captureMotion(event, action, caller);
    return action;
}

int32_t motionGetActionMaskedHook(void *event) {
    if (gOriginalMotionGetActionMasked == nullptr) return -1;
    const uintptr_t caller = reinterpret_cast<uintptr_t>(
            __builtin_extract_return_addr(__builtin_return_address(0)));
    const int32_t action = gOriginalMotionGetActionMasked(event);
    captureMotion(event, action, caller);
    return action;
}

void *resolveLauncherSymbol(void *launcherHandle, const char *name) {
    void *symbol = launcherHandle == nullptr ? nullptr : dlsym(launcherHandle, name);
    if (symbol == nullptr) symbol = dlsym(RTLD_DEFAULT, name);
    return symbol;
}

bool installMotionHooks(void *launcherHandle) {
    if (gMotionHooksInstalled.load(std::memory_order_acquire)) return true;
    if (gHookFunction == nullptr) return false;

    auto *getAction = reinterpret_cast<MotionEventIntFn>(
            resolveLauncherSymbol(launcherHandle, "input_MotionEvent_getAction"));
    auto *getActionMasked = reinterpret_cast<MotionEventIntFn>(
            resolveLauncherSymbol(launcherHandle, "input_MotionEvent_getActionMasked"));
    gMotionGetRawX = reinterpret_cast<MotionEventFloatFn>(
            resolveLauncherSymbol(launcherHandle, "input_MotionEvent_getRawX"));

    logLine(ANDROID_LOG_INFO,
            "motion symbols handle=%p action=%p masked=%p rawX=%p",
            launcherHandle, reinterpret_cast<void *>(getAction),
            reinterpret_cast<void *>(getActionMasked),
            reinterpret_cast<void *>(gMotionGetRawX));

    if (gMotionGetRawX == nullptr || (getAction == nullptr && getActionMasked == nullptr)) {
        logLine(ANDROID_LOG_ERROR,
                "MotionEvent symbols unavailable; pause gate will fail closed");
        return false;
    }

    bool installedAny = false;
    if (getAction != nullptr) {
        void *backup = nullptr;
        const int rc = gHookFunction(reinterpret_cast<void *>(getAction),
                                     reinterpret_cast<void *>(motionGetActionHook), &backup);
        if (rc == 0 && backup != nullptr) {
            gOriginalMotionGetAction = reinterpret_cast<MotionEventIntFn>(backup);
            installedAny = true;
        } else {
            logLine(ANDROID_LOG_WARN, "getAction hook failed rc=%d backup=%p", rc, backup);
        }
    }

    if (getActionMasked != nullptr && getActionMasked != getAction) {
        void *backup = nullptr;
        const int rc = gHookFunction(reinterpret_cast<void *>(getActionMasked),
                                     reinterpret_cast<void *>(motionGetActionMaskedHook), &backup);
        if (rc == 0 && backup != nullptr) {
            gOriginalMotionGetActionMasked = reinterpret_cast<MotionEventIntFn>(backup);
            installedAny = true;
        } else {
            logLine(ANDROID_LOG_WARN,
                    "getActionMasked hook failed rc=%d backup=%p", rc, backup);
        }
    }

    if (getActionMasked == getAction && gOriginalMotionGetAction != nullptr) {
        gOriginalMotionGetActionMasked = gOriginalMotionGetAction;
    }

    gMotionHooksInstalled.store(installedAny, std::memory_order_release);
    logLine(installedAny ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
            "motion hooks installed=%d", installedAny ? 1 : 0);
    return installedAny;
}

void onSwipeProcessHook(void *self, bool readyFinish, uint32_t side,
                        const void *point, float horizontalDistancePx) {
    int width = getScreenWidth();
    const int threshold = readThresholdPercent();

    const bool rawValid = gMotionState.valid;
    const float downRawX = rawValid ? gMotionState.downRawX : -1.0f;
    const float currentRawX = rawValid ? gMotionState.currentRawX : -1.0f;
    const float rawDistance = rawValid
            ? std::fabs(currentRawX - downRawX) : -1.0f;

    if (width <= 0 && rawValid && downRawX > 256.0f) {
        const int learned = roundUpTo8(downRawX + 1.0f);
        if (learned > 0) {
            width = learned;
            gCachedScreenWidth.store(learned, std::memory_order_relaxed);
            logLine(ANDROID_LOG_INFO,
                    "screen width=%d source=right-edge-motion downRawX=%.2f",
                    learned, downRawX);
        }
    }

    float progress = -1.0f;
    bool pauseReset = false;
    const bool canEvaluate = self != nullptr && gPauseDetectorReset != nullptr
            && rawValid && rawDistance >= 0.0f && width > 0;

    if (canEvaluate) {
        progress = rawDistance / static_cast<float>(width);
        if (progress < static_cast<float>(threshold) / 100.0f) {
            void *detector = reinterpret_cast<void *>(
                    reinterpret_cast<uintptr_t>(self) + kPauseDetectorOffsetInBackHelper);
            gPauseDetectorReset(detector);
            pauseReset = true;
        }
    } else if (self != nullptr && gPauseDetectorReset != nullptr) {
        void *detector = reinterpret_cast<void *>(
                reinterpret_cast<uintptr_t>(self) + kPauseDetectorOffsetInBackHelper);
        gPauseDetectorReset(detector);
        pauseReset = true;
    }

    const int64_t now = monotonicMs();
    int64_t last = gLastSwipeLogMs.load(std::memory_order_relaxed);
    if (now - last >= 150 && gLastSwipeLogMs.compare_exchange_strong(
            last, now, std::memory_order_relaxed)) {
        logLine(ANDROID_LOG_INFO,
                "swipe rawValid=%d downX=%.2f currentX=%.2f rawDx=%.2f width=%d progress=%.4f threshold=%d%% pauseReset=%d internalDx=%.2f action=%d readyFinish=%d side=%u",
                rawValid ? 1 : 0, downRawX, currentRawX, rawDistance, width,
                progress, threshold, pauseReset ? 1 : 0, horizontalDistancePx,
                gMotionState.lastAction, readyFinish ? 1 : 0, side);
    }

    if (gOriginalOnSwipeProcess != nullptr) {
        gOriginalOnSwipeProcess(self, readyFinish, side, point, horizontalDistancePx);
    }
}

bool installSwipeHook(const LibraryInfo &library, void *launcherHandle) {
    if (library.base == 0 || gHookFunction == nullptr) return false;

    gLauncherBase.store(library.base, std::memory_order_relaxed);
    gLauncherEnd.store(library.end, std::memory_order_relaxed);
    installMotionHooks(launcherHandle);

    if (gSwipeHookInstalled.load(std::memory_order_acquire)) return true;

    const uintptr_t onSwipe = library.base + kOnSwipeProcessOffset;
    const uintptr_t reset = library.base + kPauseDetectorResetOffset;
    const bool swipeSignatureOk = matchesSignature(
            onSwipe, kOnSwipeProcessSignature, sizeof(kOnSwipeProcessSignature));
    const bool resetSignatureOk = matchesSignature(
            reset, kPauseResetSignature, sizeof(kPauseResetSignature));
    if (!swipeSignatureOk || !resetSignatureOk) {
        logLine(ANDROID_LOG_ERROR,
                "launcher signature mismatch base=%p swipe=%d reset=%d; hook skipped",
                reinterpret_cast<void *>(library.base), swipeSignatureOk ? 1 : 0,
                resetSignatureOk ? 1 : 0);
        return false;
    }

    gPauseDetectorReset = reinterpret_cast<PauseDetectorResetFn>(reset);

    void *backup = nullptr;
    const int rc = gHookFunction(reinterpret_cast<void *>(onSwipe),
                                 reinterpret_cast<void *>(onSwipeProcessHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR, "onSwipe hook failed rc=%d backup=%p", rc, backup);
        return false;
    }

    gOriginalOnSwipeProcess = reinterpret_cast<OnSwipeProcessFn>(backup);
    gSwipeHookInstalled.store(true, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "hook installed launcher=8.01.02.5459 base=%p threshold=%d%% screenWidth=%d motionHooks=%d",
            reinterpret_cast<void *>(library.base), readThresholdPercent(), getScreenWidth(),
            gMotionHooksInstalled.load(std::memory_order_relaxed) ? 1 : 0);
    return true;
}

void hookWorker() {
    for (int attempt = 1; attempt <= 200; ++attempt) {
        if (!isTargetProcess()) {
            logLine(ANDROID_LOG_WARN, "hook worker left target process; aborting");
            return;
        }
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) {
            logLine(ANDROID_LOG_INFO, "found %s base=%p end=%p",
                    library.path.c_str(), reinterpret_cast<void *>(library.base),
                    reinterpret_cast<void *>(library.end));
            void *handle = dlopen(library.path.c_str(), RTLD_NOW | RTLD_NOLOAD);
            installSwipeHook(library, handle);
            if (handle != nullptr) dlclose(handle);
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

void onLibraryLoaded(const char *name, void *handle) {
    if (!isTargetProcess() || name == nullptr) return;
    if (std::strstr(name, kTargetLibrary) != nullptr) {
        logLine(ANDROID_LOG_INFO, "LSPosed library callback: %s handle=%p", name, handle);
        const LibraryInfo library = findLauncherLibrary();
        if (library.base != 0) installSwipeHook(library, handle);
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used))
NativeOnModuleLoaded native_init(const NativeAPIEntries *entries) {
    if (entries == nullptr) return nullptr;

    const std::string exe = readExecutable();
    const std::string process = readProcessName();
    logLine(ANDROID_LOG_INFO,
            "native_init candidate api=%u exe=%s process=%s hook_func=%p",
            entries->version, exe.c_str(), process.c_str(),
            reinterpret_cast<void *>(entries->hook_func));

    if (entries->hook_func == nullptr) {
        logLine(ANDROID_LOG_ERROR, "native_init rejected: hook_func is null");
        return nullptr;
    }
    if (exe != kSpawnerPath || process != kTargetPackage) {
        logLine(ANDROID_LOG_WARN,
                "native_init rejected: expected exe=%s process=%s",
                kSpawnerPath, kTargetPackage);
        return nullptr;
    }

    gHookFunction = entries->hook_func;
    logLine(ANDROID_LOG_INFO, "native_init accepted api=%u", entries->version);
    ensureWorkerStarted();
    return onLibraryLoaded;
}
