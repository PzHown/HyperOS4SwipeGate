#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kLogLevelFileName = "hyperos4swipegate_log_level";
constexpr int kLogLevelOff = 0;
constexpr int kLogLevelCompact = 1;
constexpr int kLogLevelDetailed = 2;
constexpr int kAndroidUserOffset = 100000;

bool readLevelFile(const char *path, int *outLevel) {
    if (path == nullptr || outLevel == nullptr) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    char buffer[16]{};
    const ssize_t size = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (size <= 0) return false;
    buffer[size] = '\0';

    char *start = buffer;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
    char *end = nullptr;
    const long parsed = std::strtol(start, &end, 10);
    if (end == start) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0' || parsed < kLogLevelOff || parsed > kLogLevelDetailed) return false;

    *outLevel = static_cast<int>(parsed);
    return true;
}

int readNativeLogLevel() {
    const int userId = static_cast<int>(getuid()) / kAndroidUserOffset;
    char path[256]{};
    int level = kLogLevelOff;

    std::snprintf(path, sizeof(path),
                  "/data/user_de/%d/com.miui.home/cache/%s", userId, kLogLevelFileName);
    if (readLevelFile(path, &level)) return level;

    std::snprintf(path, sizeof(path),
                  "/data/user/%d/com.miui.home/cache/%s", userId, kLogLevelFileName);
    if (readLevelFile(path, &level)) return level;

    if (userId == 0) {
        std::snprintf(path, sizeof(path),
                      "/data/data/com.miui.home/cache/%s", kLogLevelFileName);
        if (readLevelFile(path, &level)) return level;
    }
    return kLogLevelOff;
}

bool isDetailedOnlyLine(const char *message) {
    if (message == nullptr) return false;
    return std::strncmp(message, "DP_GATE rawDx=", 14) == 0
            || std::strncmp(message, "HOOK_HEALTH healthy ", 20) == 0;
}

bool shouldPersist(const char *message) {
    const int level = readNativeLogLevel();
    if (level >= kLogLevelDetailed) return true;
    if (isDetailedOnlyLine(message)) return false;

    // OFF still permits short-lived hook lifecycle/error lines. ModuleMain consumes these
    // for strict hook-health state and immediately removes the transient source file.
    // COMPACT keeps those same lifecycle/config/error lines as the user-visible log.
    return true;
}

}  // namespace

extern "C" int __wrap_dprintf(int fd, const char *format, ...) {
    va_list args;
    va_start(args, format);

    bool suppress = false;
    if (format != nullptr && std::strcmp(format, "%s\n") == 0) {
        va_list inspect;
        va_copy(inspect, args);
        const char *message = va_arg(inspect, const char *);
        va_end(inspect);
        suppress = !shouldPersist(message);
    }

    int result = 0;
    if (!suppress) {
        result = vdprintf(fd, format, args);
    }
    va_end(args);
    return result;
}
