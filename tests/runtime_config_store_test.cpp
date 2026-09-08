#include "runtime_config_store.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using swipegate::ConfigLoadResult;
using swipegate::RuntimeConfig;
using swipegate::RuntimeConfigStore;
namespace fs = std::filesystem;

namespace {
std::atomic<int> assertions{0};
void check(bool value, const char *message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        char name[] = "/tmp/swipegate-config-test-XXXXXX";
        const char *result = mkdtemp(name);
        check(result != nullptr, "mkdtemp");
        path = result;
    }
    ~TemporaryDirectory() { fs::remove_all(path); }
};
void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    check(static_cast<bool>(stream), "write fixture");
}
std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

int main() {
    int scenarios = 0;
    const RuntimeConfig enabled{175, 2, true, true};
    const RuntimeConfig disabled{88, 0, false, false};
    {
        TemporaryDirectory directory;
        RuntimeConfigStore store({directory.path.string()});
        RuntimeConfig loaded{200, 1, true, true};
        check(store.load(&loaded) == ConfigLoadResult::Missing, "fresh install has no persisted config");
        check(loaded == RuntimeConfig{}, "safe stock defaults");
        check(store.save(enabled), "persist complete config");
        RuntimeConfigStore restarted({directory.path.string()});
        check(restarted.load(&loaded) == ConfigLoadResult::Loaded && loaded == enabled,
              "all fields recover in fresh store without App state");
        struct stat state{};
        check(stat((directory.path / "files/swipegate/runtime.conf").c_str(), &state) == 0,
              "config in persistent files directory");
        check((state.st_mode & 0777) == 0600, "private config file permissions");
        ++scenarios;
    }
    {
        const auto encoded = RuntimeConfigStore::encode(enabled);
        RuntimeConfig output;
        check(RuntimeConfigStore::decode(encoded, &output) && output == enabled, "serialization round trip");
        for (size_t size = 0; size < encoded.size(); ++size) {
            RuntimeConfig sentinel{199, 1, false, true};
            check(!RuntimeConfigStore::decode(encoded.substr(0, size), &sentinel), "reject every truncated prefix");
            check(sentinel.thresholdDp == 199, "failed decode does not publish a partial config");
        }
        const std::vector<std::string> invalid{
            encoded + "haptic_enabled=0\n", encoded + "unknown=1\n", encoded + "\n",
            "SWIPEGATE_CONFIG 2\n" + encoded.substr(19), std::string(600, 'x'),
        };
        for (const auto &text : invalid) check(!RuntimeConfigStore::decode(text, &output), "reject malformed config");
        for (const auto &bad : {"87", "301", "999999999999999999999999999", "1e2", "-1", "175junk"}) {
            auto text = encoded;
            text.replace(text.find("175"), 3, bad);
            check(!RuntimeConfigStore::decode(text, &output), "strict threshold range and numeric parsing");
        }
        auto text = encoded;
        text.replace(text.find("haptic_enabled=1"), 16, "haptic_enabled=2");
        check(!RuntimeConfigStore::decode(text, &output), "boolean range enforced");
        check(!RuntimeConfigStore::decode(encoded, nullptr), "null decode destination rejected");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        write(directory.path / "cache/hyperos4swipegate_config", "190\n");
        write(directory.path / "cache/hyperos4swipegate_log_level", "2\n");
        write(directory.path / "cache/hyperos4swipegate_break_open", "1\n");
        RuntimeConfigStore store({directory.path.string()});
        RuntimeConfig migrated;
        check(store.load(&migrated) == ConfigLoadResult::Migrated, "legacy cache found");
        check(migrated.thresholdDp == 190 && migrated.logLevel == 2 && migrated.breakOpenEnabled,
              "legacy threshold/log/break-open preserved");
        check(!migrated.hapticEnabled, "unpersisted legacy haptic defaults off, not invented");
        check(store.save(migrated), "migration persisted atomically");
        fs::remove_all(directory.path / "cache");
        RuntimeConfig afterCacheDeletion;
        check(store.load(&afterCacheDeletion) == ConfigLoadResult::Loaded && afterCacheDeletion == migrated,
              "cache clearing cannot erase migrated config");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        write(directory.path / "cache/hyperos4swipegate_config", "299\n");
        write(directory.path / "files/swipegate/runtime.conf", "SWIPEGATE_CONFIG 1\nthreshold_dp=299\n");
        RuntimeConfigStore store({directory.path.string()});
        RuntimeConfig config;
        check(store.load(&config) == ConfigLoadResult::Invalid, "corrupt current file is detected");
        check(config == RuntimeConfig{}, "corruption never resurrects stale legacy settings");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        RuntimeConfigStore store({directory.path.string()});
        check(store.save(enabled), "baseline for interrupted write");
        write(directory.path / "files/swipegate/.runtime.interrupted.tmp", "partial");
        RuntimeConfig recovered;
        check(store.load(&recovered) == ConfigLoadResult::Loaded && recovered == enabled,
              "interrupted temp file ignored");
        check(!store.save(RuntimeConfig{500, 1, true, true}), "invalid replacement not written");
        check(store.load(&recovered) == ConfigLoadResult::Loaded && recovered == enabled,
              "failed write leaves last good config intact");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        RuntimeConfigStore store({directory.path.string()});
        check(store.save(enabled), "baseline for concurrent updates");
        std::atomic<bool> finished{false};
        std::thread reader([&] {
            do {
                RuntimeConfig config;
                check(store.load(&config) == ConfigLoadResult::Loaded, "atomic reader always sees complete file");
                check(config == enabled || config == disabled, "atomic reader never sees mixed fields");
            } while (!finished.load());
        });
        for (int i = 0; i < 60; ++i) check(store.save(i % 2 == 0 ? disabled : enabled), "atomic writer");
        finished.store(true);
        reader.join();
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        RuntimeConfigStore store({directory.path.string()});
        check(store.save(enabled), "baseline for process restart");
        pid_t child = fork();
        check(child >= 0, "fork restart simulator");
        if (child == 0) {
            RuntimeConfig fromDisk;
            RuntimeConfigStore newProcess({directory.path.string()});
            _exit(newProcess.load(&fromDisk) == ConfigLoadResult::Loaded && fromDisk == enabled ? 0 : 1);
        }
        int status = 0;
        check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "fresh process restores all settings without App");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        TemporaryDirectory outside;
        write(outside.path / "config", RuntimeConfigStore::encode(enabled));
        fs::create_directories(directory.path / "files/swipegate");
        fs::create_symlink(outside.path / "config", directory.path / "files/swipegate/runtime.conf");
        RuntimeConfigStore store({directory.path.string()});
        RuntimeConfig loaded;
        check(store.load(&loaded) == ConfigLoadResult::IoError, "config symlink not followed");
        check(read(outside.path / "config") == RuntimeConfigStore::encode(enabled), "external target unchanged");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        TemporaryDirectory outside;
        fs::create_directory_symlink(outside.path, directory.path / "files");
        RuntimeConfigStore store({directory.path.string()});
        check(!store.save(enabled), "symlinked private directory rejected");
        check(!fs::exists(outside.path / "swipegate/runtime.conf"), "write cannot escape through directory symlink");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        fs::create_directories(directory.path / "files/swipegate");
        check(mkfifo((directory.path / "files/swipegate/runtime.conf").c_str(), 0600) == 0, "create FIFO fixture");
        RuntimeConfigStore store({directory.path.string()});
        RuntimeConfig config;
        check(store.load(&config) == ConfigLoadResult::Invalid, "non-regular file rejected without blocking");
        ++scenarios;
    }
    {
        TemporaryDirectory directory;
        const auto unavailable = directory.path / "not-mounted-yet";
        RuntimeConfigStore store({unavailable.string()});
        RuntimeConfig config;
        check(store.load(&config) == ConfigLoadResult::IoError, "unavailable data directory is retryable");
        fs::create_directories(unavailable);
        check(store.save(enabled), "storage becomes available");
        check(store.load(&config) == ConfigLoadResult::Loaded && config == enabled,
              "retry recovers after storage availability changes");
        ++scenarios;
    }
    {
        TemporaryDirectory userOne;
        TemporaryDirectory userTwo;
        RuntimeConfigStore first({userOne.path.string()});
        RuntimeConfigStore second({userTwo.path.string()});
        check(first.save(enabled) && second.save(disabled), "independent user stores");
        RuntimeConfig one, two;
        check(first.load(&one) == ConfigLoadResult::Loaded && one == enabled, "first user unchanged");
        check(second.load(&two) == ConfigLoadResult::Loaded && two == disabled, "second user isolated");
        ++scenarios;
    }
    std::cout << "PASS: " << scenarios << " native persistence scenarios, " << assertions
              << " assertions (including concurrent reads)\n";
}
