#include "control_channel.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr uint32_t kProtocolMagic = 0x53474331U;  // SGC1
constexpr uint32_t kProtocolVersion = 1U;
constexpr uint16_t kControlPort = 39173;
constexpr int kMinThresholdDp = 88;
constexpr int kMaxThresholdDp = 300;
constexpr int kMinLogLevel = 0;
constexpr int kMaxLogLevel = 2;
constexpr int64_t kSyncIntervalMs = 500;
constexpr int kConnectTimeoutMs = 250;
constexpr int kChildProbeIntervalMs = 100;
constexpr int kChildProbeAttempts = 150;  // 15s for HYOS specialization/name change.
constexpr size_t kMaxPatternBytes = 128;
constexpr size_t kMaxDetailBytes = 768;
constexpr size_t kMaxAppLogBytes = 24 * 1024;
constexpr const char *kLauncherPackage = "com.miui.home";
constexpr const char *kConfigFileName = "hyperos4swipegate_config";
constexpr const char *kLogLevelFileName = "hyperos4swipegate_log_level";
constexpr int kAndroidUserOffset = 100000;

enum HookState : uint32_t {
    kHookUnknown = 0,
    kHookWaiting = 1,
    kHookHealthy = 2,
    kHookRepairing = 3,
    kHookFailed = 4,
};

std::atomic_flag gStateLock = ATOMIC_FLAG_INIT;
std::atomic<int64_t> gLastSyncAttemptMs{0};
std::atomic<bool> gWorkerStarted{false};
std::atomic<bool> gChildProbeStarted{false};
int gThresholdDp = -1;
int gLogLevel = -1;
HookState gHookState = kHookWaiting;
std::string gPattern;
std::string gDetail = "Native 模块已加载，等待 native_init / Hook";
std::string gAppLog;

extern "C" int __real_clock_gettime(clockid_t clockId, struct timespec *tp);

class SpinGuard {
public:
    SpinGuard() {
        while (gStateLock.test_and_set(std::memory_order_acquire)) {
            sched_yield();
        }
    }
    ~SpinGuard() { gStateLock.clear(std::memory_order_release); }
};

int64_t monotonicMsRaw() {
    timespec ts{};
    if (__real_clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

std::string readProcessName() {
    const int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    char buffer[256]{};
    const ssize_t size = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (size <= 0) return {};
    buffer[std::min<ssize_t>(size, sizeof(buffer) - 1)] = '\0';
    return std::string(buffer);
}

bool isLauncherProcess() {
    return readProcessName() == kLauncherPackage;
}

bool isDetailedOnlyLine(const char *text) {
    if (text == nullptr) return false;
    return std::strncmp(text, "DP_GATE rawDx=", 14) == 0
            || std::strncmp(text, "HOOK_HEALTH healthy ", 20) == 0;
}

std::string extractPattern(const char *text) {
    if (text == nullptr) return {};
    const char *found = std::strstr(text, "pattern=");
    if (found == nullptr) return {};
    found += 8;
    const char *end = found;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
    if (end <= found) return {};
    return std::string(found, static_cast<size_t>(end - found)).substr(0, kMaxPatternBytes);
}

void updateHookStateLocked(const char *text) {
    if (text == nullptr || *text == '\0') return;

    const std::string pattern = extractPattern(text);
    if (!pattern.empty() && pattern != "<none>") gPattern = pattern;

    if (std::strstr(text, "DP_GATE native_init checks") != nullptr
            || std::strstr(text, "DP_GATE native_init accepted") != nullptr
            || std::strstr(text, "native_init waiting for") != nullptr
            || std::strstr(text, "HOOK_HEALTH launcher mapping changed") != nullptr
            || std::strstr(text, "HOOK_HEALTH libapp_launcher.so absent") != nullptr
            || std::strstr(text, "HOOK_SCAN resolved") != nullptr) {
        gHookState = kHookWaiting;
    } else if (std::strstr(text, "HOOK_HEALTH original bytes restored") != nullptr
            || std::strstr(text, "starting unhook+rehook repair") != nullptr
            || std::strstr(text, "HOOK_HEALTH repair deferred") != nullptr) {
        gHookState = kHookRepairing;
    } else if (std::strstr(text, "DP_GATE hook installed") != nullptr
            || std::strstr(text, "HOOK_HEALTH healthy ") != nullptr
            || std::strstr(text, "HOOK_HEALTH repaired successfully") != nullptr
            || std::strstr(text, "DP_GATE rawDx=") != nullptr) {
        gHookState = kHookHealthy;
    } else if (std::strstr(text, "HOOK_SCAN install refused") != nullptr
            || std::strstr(text, "HOOK_SCAN pattern changed before hook") != nullptr
            || std::strstr(text, "DP_GATE hook_func failed") != nullptr
            || std::strstr(text, "hook_func returned success but entry is not patched") != nullptr
            || std::strstr(text, "HOOK_HEALTH foreign patch detected") != nullptr
            || std::strstr(text, "HOOK_HEALTH repair unavailable") != nullptr
            || std::strstr(text, "HOOK_HEALTH repair failed") != nullptr
            || std::strstr(text, "HOOK_HEALTH repair aborted") != nullptr
            || std::strstr(text, "DP_GATE native_init rejected") != nullptr) {
        gHookState = kHookFailed;
    } else {
        return;
    }

    gDetail.assign(text, std::min(std::strlen(text), kMaxDetailBytes));
}

void appendAppLogLocked(const char *text) {
    if (text == nullptr || *text == '\0') return;
    if (gLogLevel <= 0) return;
    if (gLogLevel == 1 && isDetailedOnlyLine(text)) return;

    gAppLog.append(text);
    gAppLog.push_back('\n');
    if (gAppLog.size() > kMaxAppLogBytes) {
        const size_t over = gAppLog.size() - kMaxAppLogBytes;
        size_t cut = gAppLog.find('\n', over);
        if (cut == std::string::npos) cut = over;
        else ++cut;
        gAppLog.erase(0, cut);
    }
}

void persistValue(const char *fileName, int value) {
    if (fileName == nullptr) return;
    const int userId = static_cast<int>(getuid()) / kAndroidUserOffset;
    char path[256]{};
    const char *formats[] = {
            "/data/user_de/%d/com.miui.home/cache/%s",
            "/data/user/%d/com.miui.home/cache/%s",
            "/data/data/com.miui.home/cache/%s",
    };

    char valueText[24]{};
    const int valueLength = std::snprintf(valueText, sizeof(valueText), "%d\n", value);
    if (valueLength <= 0) return;

    for (size_t i = 0; i < 3; ++i) {
        if (i == 2 && userId != 0) break;
        if (i < 2) {
            std::snprintf(path, sizeof(path), formats[i], userId, fileName);
        } else {
            std::snprintf(path, sizeof(path), formats[i], fileName);
        }
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        const ssize_t written = write(fd, valueText, static_cast<size_t>(valueLength));
        close(fd);
        if (written == valueLength) return;
    }
}

bool sendAll(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t sent = send(fd, bytes + offset, size - offset, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        offset += static_cast<size_t>(sent);
    }
    return true;
}

bool recvAll(int fd, void *data, size_t size) {
    auto *bytes = static_cast<uint8_t *>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t received = recv(fd, bytes + offset, size - offset, 0);
        if (received <= 0) return false;
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool sendU32(int fd, uint32_t value) {
    const uint32_t network = htonl(value);
    return sendAll(fd, &network, sizeof(network));
}

bool sendString(int fd, const std::string &value, size_t maxBytes) {
    const size_t length = std::min(value.size(), maxBytes);
    if (!sendU32(fd, static_cast<uint32_t>(length))) return false;
    return length == 0 || sendAll(fd, value.data(), length);
}

int connectToApp() {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    const int originalFlags = fcntl(fd, F_GETFL, 0);
    if (originalFlags < 0 || fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kControlPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (result != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (result != 0) {
        pollfd descriptor{fd, POLLOUT, 0};
        if (poll(&descriptor, 1, kConnectTimeoutMs) <= 0) {
            close(fd);
            return -1;
        }
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) != 0 || socketError != 0) {
            close(fd);
            return -1;
        }
    }

    if (fcntl(fd, F_SETFL, originalFlags) != 0) {
        close(fd);
        return -1;
    }
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = kConnectTimeoutMs * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

void exchangeWithApp() {
    if (!isLauncherProcess()) return;

    HookState state;
    std::string pattern;
    std::string detail;
    std::string appLog;
    {
        SpinGuard guard;
        state = gHookState;
        pattern = gPattern;
        detail = gDetail;
        appLog = gAppLog;
    }

    const int fd = connectToApp();
    if (fd < 0) return;

    bool ok = sendU32(fd, kProtocolMagic)
            && sendU32(fd, kProtocolVersion)
            && sendU32(fd, static_cast<uint32_t>(state))
            && sendString(fd, pattern, kMaxPatternBytes)
            && sendString(fd, detail, kMaxDetailBytes)
            && sendString(fd, appLog, kMaxAppLogBytes);

    uint32_t response[4]{};
    if (ok) ok = recvAll(fd, response, sizeof(response));
    close(fd);
    if (!ok) return;

    const uint32_t magic = ntohl(response[0]);
    const uint32_t version = ntohl(response[1]);
    const int threshold = static_cast<int>(ntohl(response[2]));
    const int logLevel = static_cast<int>(ntohl(response[3]));
    if (magic != kProtocolMagic || version != kProtocolVersion) return;
    if (threshold < kMinThresholdDp || threshold > kMaxThresholdDp) return;
    if (logLevel < kMinLogLevel || logLevel > kMaxLogLevel) return;

    bool thresholdChanged = false;
    bool logLevelChanged = false;
    {
        SpinGuard guard;
        thresholdChanged = gThresholdDp != threshold;
        logLevelChanged = gLogLevel != logLevel;
        gThresholdDp = threshold;
        gLogLevel = logLevel;
        if (logLevel <= 0) gAppLog.clear();
    }
    if (thresholdChanged) persistValue(kConfigFileName, threshold);
    if (logLevelChanged) persistValue(kLogLevelFileName, logLevel);
}

void *controlWorkerMain(void *) {
    while (isLauncherProcess()) {
        swipegate_control_sync_if_due();
        usleep(100 * 1000);
    }
    gWorkerStarted.store(false, std::memory_order_release);
    return nullptr;
}

void startControlWorkerIfLauncher() {
    if (!isLauncherProcess()) return;
    bool expected = false;
    if (!gWorkerStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }

    pthread_t thread{};
    const int result = pthread_create(&thread, nullptr, controlWorkerMain, nullptr);
    if (result != 0) {
        gWorkerStarted.store(false, std::memory_order_release);
        SpinGuard guard;
        gHookState = kHookFailed;
        gDetail = "Native control worker 启动失败 rc=" + std::to_string(result);
        return;
    }
    pthread_detach(thread);
}

void *childProbeMain(void *) {
    for (int attempt = 0; attempt < kChildProbeAttempts; ++attempt) {
        if (isLauncherProcess()) {
            {
                SpinGuard guard;
                if (gHookState == kHookWaiting && gDetail.find("Native 模块已加载") == 0) {
                    gDetail = "Launcher child 已识别，等待 native_init / Hook";
                }
            }
            gChildProbeStarted.store(false, std::memory_order_release);
            startControlWorkerIfLauncher();
            return nullptr;
        }
        usleep(kChildProbeIntervalMs * 1000);
    }
    gChildProbeStarted.store(false, std::memory_order_release);
    return nullptr;
}

void startChildProbeAfterFork() {
    bool expected = false;
    if (!gChildProbeStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    pthread_t thread{};
    const int result = pthread_create(&thread, nullptr, childProbeMain, nullptr);
    if (result != 0) {
        gChildProbeStarted.store(false, std::memory_order_release);
        return;
    }
    pthread_detach(thread);
}

void resetAfterFork() {
    gStateLock.clear(std::memory_order_release);
    gLastSyncAttemptMs.store(0, std::memory_order_release);
    gWorkerStarted.store(false, std::memory_order_release);
    gChildProbeStarted.store(false, std::memory_order_release);

    // HYOS commonly loads LSPosed native modules in the root spawner (cmdline usap64) and then
    // forks the actual package child. ELF constructors do not run again in that child, so create
    // a bounded probe thread here and wait for specialization to rename cmdline to com.miui.home.
    // Bionic has completed its own child-atfork bookkeeping before user child handlers run; the
    // thread only performs ordinary work after pthread_create returns in the child.
    startChildProbeAfterFork();
}

__attribute__((constructor)) void initializeControlChannel() {
    (void) pthread_atfork(nullptr, nullptr, resetAfterFork);
    // Also cover frameworks that dlopen the module directly in the final launcher child.
    startControlWorkerIfLauncher();
}

}  // namespace

extern "C" int swipegate_control_threshold_dp() {
    SpinGuard guard;
    return gThresholdDp;
}

extern "C" int swipegate_control_log_level() {
    SpinGuard guard;
    return gLogLevel;
}

extern "C" void swipegate_control_on_log(int, const char *text) {
    startControlWorkerIfLauncher();
    {
        SpinGuard guard;
        updateHookStateLocked(text);
        appendAppLogLocked(text);
    }
    swipegate_control_sync_if_due();
}

extern "C" void swipegate_control_sync_if_due() {
    if (!isLauncherProcess()) return;
    startControlWorkerIfLauncher();

    const int64_t now = monotonicMsRaw();
    if (now <= 0) return;

    int64_t previous = gLastSyncAttemptMs.load(std::memory_order_acquire);
    while (now - previous >= kSyncIntervalMs) {
        if (gLastSyncAttemptMs.compare_exchange_weak(
                    previous, now, std::memory_order_acq_rel, std::memory_order_acquire)) {
            exchangeWithApp();
            return;
        }
    }
}

extern "C" int __wrap_clock_gettime(clockid_t clockId, struct timespec *tp) {
    const int result = __real_clock_gettime(clockId, tp);
    if (result == 0 && clockId == CLOCK_MONOTONIC) {
        swipegate_control_sync_if_due();
    }
    return result;
}
