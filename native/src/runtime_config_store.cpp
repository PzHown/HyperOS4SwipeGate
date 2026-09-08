#include "runtime_config_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <utility>

namespace swipegate {
namespace {
constexpr const char *kConfigName = "runtime.conf";
constexpr size_t kMaxConfigBytes = 512;
std::atomic<unsigned long long> nextTemporaryId{0};

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return fd_; }
private:
    int fd_;
};

bool ownedDirectory(int fd) {
    struct stat st{};
    return fd >= 0 && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid();
}

bool syncFd(int fd);

int openChildDirectory(int parent, const char *name, bool create) {
    if (create) {
        if (mkdirat(parent, name, 0700) == 0) {
            if (!syncFd(parent)) return -1;
        } else if (errno != EEXIST) return -1;
    }
    const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (ownedDirectory(fd)) return fd;
    close(fd);
    errno = EACCES;
    return -1;
}

int openConfigDirectory(const std::string &root, bool create) {
    Fd parent(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!ownedDirectory(parent.get())) return -1;
    Fd files(openChildDirectory(parent.get(), "files", create));
    if (files.get() < 0) return -1;
    return openChildDirectory(files.get(), "swipegate", create);
}

enum class ReadResult { Ok, Missing, Invalid, IoError };
ReadResult readFile(int parent, const char *name, std::string *out) {
    Fd file(openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0) return errno == ENOENT ? ReadResult::Missing : ReadResult::IoError;
    struct stat st{};
    if (fstat(file.get(), &st) != 0) return ReadResult::IoError;
    if (!S_ISREG(st.st_mode) || st.st_uid != getuid() || st.st_size < 0
            || static_cast<unsigned long long>(st.st_size) > kMaxConfigBytes) {
        return ReadResult::Invalid;
    }
    std::string text;
    char buffer[128];
    for (;;) {
        const ssize_t count = read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return ReadResult::IoError;
        if (count == 0) break;
        text.append(buffer, static_cast<size_t>(count));
        if (text.size() > kMaxConfigBytes) return ReadResult::Invalid;
    }
    *out = std::move(text);
    return ReadResult::Ok;
}

bool parseInteger(std::string_view text, int minimum, int maximum, int *out) {
    if (text.empty()) return false;
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
            || value < minimum || value > maximum) return false;
    *out = value;
    return true;
}

bool readLegacy(const std::vector<std::string> &roots, const char *name,
                int minimum, int maximum, int *out) {
    for (const auto &root : roots) {
        Fd parent(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (!ownedDirectory(parent.get())) continue;
        Fd cache(openChildDirectory(parent.get(), "cache", false));
        if (cache.get() < 0) continue;
        std::string text;
        if (readFile(cache.get(), name, &text) != ReadResult::Ok) continue;
        std::string_view view(text);
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t'
                || view.front() == '\r' || view.front() == '\n')) view.remove_prefix(1);
        while (!view.empty() && (view.back() == ' ' || view.back() == '\t'
                || view.back() == '\r' || view.back() == '\n')) view.remove_suffix(1);
        if (parseInteger(view, minimum, maximum, out)) return true;
    }
    return false;
}

bool syncFd(int fd) {
    int result;
    do { result = fsync(fd); } while (result != 0 && errno == EINTR);
    return result == 0;
}

bool writeAtomically(int directory, const std::string &text) {
    char temporary[96]{};
    int raw = -1;
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::snprintf(temporary, sizeof(temporary), ".runtime.%ld.%llu.tmp",
                      static_cast<long>(getpid()), nextTemporaryId.fetch_add(1));
        raw = openat(directory, temporary,
                     O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (raw >= 0 || errno != EEXIST) break;
    }
    if (raw < 0) return false;
    Fd file(raw);
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = write(file.get(), text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<size_t>(count);
    }
    if (written != text.size() || !syncFd(file.get())
            || renameat(directory, temporary, directory, kConfigName) != 0) {
        unlinkat(directory, temporary, 0);
        return false;
    }
    return syncFd(directory);
}
}  // namespace

RuntimeConfigStore::RuntimeConfigStore(std::vector<std::string> roots)
        : roots_(std::move(roots)) {}

bool RuntimeConfigStore::valid(const RuntimeConfig &config) {
    return config.thresholdDp >= 88 && config.thresholdDp <= 300
            && config.logLevel >= 0 && config.logLevel <= 2;
}

std::string RuntimeConfigStore::encode(const RuntimeConfig &config) {
    if (!valid(config)) return {};
    return "SWIPEGATE_CONFIG 1\nthreshold_dp=" + std::to_string(config.thresholdDp)
            + "\nlog_level=" + std::to_string(config.logLevel)
            + "\nhaptic_enabled=" + (config.hapticEnabled ? "1" : "0")
            + "\nbreak_open_enabled=" + (config.breakOpenEnabled ? "1" : "0") + "\n";
}

bool RuntimeConfigStore::decode(const std::string &text, RuntimeConfig *out) {
    if (out == nullptr || text.size() > kMaxConfigBytes) return false;
    std::string_view view(text);
    constexpr std::string_view header = "SWIPEGATE_CONFIG 1\n";
    if (!view.starts_with(header)) return false;
    view.remove_prefix(header.size());
    RuntimeConfig candidate;
    unsigned fields = 0;
    while (!view.empty()) {
        const size_t end = view.find('\n');
        if (end == std::string_view::npos) return false;
        const std::string_view line = view.substr(0, end);
        view.remove_prefix(end + 1);
        const size_t equal = line.find('=');
        if (equal == std::string_view::npos) return false;
        const auto key = line.substr(0, equal);
        const auto value = line.substr(equal + 1);
        int parsed = 0;
        unsigned field = 0;
        if (key == "threshold_dp") {
            field = 1;
            if (!parseInteger(value, 88, 300, &parsed)) return false;
            candidate.thresholdDp = parsed;
        } else if (key == "log_level") {
            field = 2;
            if (!parseInteger(value, 0, 2, &parsed)) return false;
            candidate.logLevel = parsed;
        } else if (key == "haptic_enabled") {
            field = 4;
            if (!parseInteger(value, 0, 1, &parsed)) return false;
            candidate.hapticEnabled = parsed == 1;
        } else if (key == "break_open_enabled") {
            field = 8;
            if (!parseInteger(value, 0, 1, &parsed)) return false;
            candidate.breakOpenEnabled = parsed == 1;
        } else return false;
        if ((fields & field) != 0) return false;
        fields |= field;
    }
    if (fields != 15) return false;
    *out = candidate;
    return true;
}

ConfigLoadResult RuntimeConfigStore::load(RuntimeConfig *out) const {
    if (out == nullptr) return ConfigLoadResult::Invalid;
    *out = RuntimeConfig{};
    bool ioError = false;
    bool availableRoot = false;
    for (const auto &root : roots_) {
        Fd parent(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (ownedDirectory(parent.get())) availableRoot = true;
        Fd directory(openConfigDirectory(root, false));
        if (directory.get() < 0) {
            if (errno != ENOENT) ioError = true;
            continue;
        }
        std::string text;
        const ReadResult read = readFile(directory.get(), kConfigName, &text);
        if (read == ReadResult::Missing) continue;
        if (read == ReadResult::IoError) return ConfigLoadResult::IoError;
        if (read == ReadResult::Invalid || !decode(text, out)) return ConfigLoadResult::Invalid;
        return ConfigLoadResult::Loaded;
    }
    RuntimeConfig legacy;
    bool found = readLegacy(roots_, "hyperos4swipegate_config", 88, 300, &legacy.thresholdDp);
    found = readLegacy(roots_, "hyperos4swipegate_log_level", 0, 2, &legacy.logLevel) || found;
    int breakOpen = 0;
    found = readLegacy(roots_, "hyperos4swipegate_break_open", 0, 1, &breakOpen) || found;
    legacy.breakOpenEnabled = breakOpen == 1;
    if (found) {
        *out = legacy;
        return ConfigLoadResult::Migrated;
    }
    return ioError || !availableRoot ? ConfigLoadResult::IoError : ConfigLoadResult::Missing;
}

bool RuntimeConfigStore::save(const RuntimeConfig &config) const {
    const std::string text = encode(config);
    if (text.empty()) return false;
    for (const auto &root : roots_) {
        Fd directory(openConfigDirectory(root, true));
        if (directory.get() < 0) continue;
        return writeAtomically(directory.get(), text);
    }
    return false;
}
}  // namespace swipegate
