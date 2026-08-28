#include <fcntl.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kThresholdDpProperty = "persist.hyperos4swipegate.threshold_dp";
constexpr const char *kConfigFileName = "hyperos4swipegate_config";
constexpr int kMaxThresholdDp = 300;
constexpr int kAndroidUserOffset = 100000;

bool readThresholdFile(const char *path, int *outValue) {
    if (path == nullptr || outValue == nullptr) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    char buffer[32]{};
    const ssize_t size = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (size <= 0) return false;
    buffer[size] = '\0';

    char *start = buffer;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;

    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(start, &end, 10);
    if (errno != 0 || end == start) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    if (*end != '\0' || parsed < 0 || parsed > kMaxThresholdDp) return false;

    *outValue = static_cast<int>(parsed);
    return true;
}

bool readRootlessThreshold(int *outValue) {
    const int userId = static_cast<int>(getuid()) / kAndroidUserOffset;
    char path[256]{};

    std::snprintf(path, sizeof(path),
                  "/data/user_de/%d/com.miui.home/cache/%s", userId, kConfigFileName);
    if (readThresholdFile(path, outValue)) return true;

    std::snprintf(path, sizeof(path),
                  "/data/user/%d/com.miui.home/cache/%s", userId, kConfigFileName);
    if (readThresholdFile(path, outValue)) return true;

    if (userId == 0) {
        std::snprintf(path, sizeof(path),
                      "/data/data/com.miui.home/cache/%s", kConfigFileName);
        if (readThresholdFile(path, outValue)) return true;
    }
    return false;
}

}  // namespace

extern "C" int __real___system_property_get(const char *name, char *value);

extern "C" int __wrap___system_property_get(const char *name, char *value) {
    if (name != nullptr && value != nullptr && std::strcmp(name, kThresholdDpProperty) == 0) {
        int thresholdDp = 0;
        if (readRootlessThreshold(&thresholdDp)) {
            const int written = std::snprintf(value, PROP_VALUE_MAX, "%d", thresholdDp);
            return written > 0 ? written : 0;
        }
    }
    return __real___system_property_get(name, value);
}
