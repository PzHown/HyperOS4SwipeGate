#pragma once

#include <string>
#include <vector>

namespace swipegate {

struct RuntimeConfig {
    int thresholdDp = 88;
    int logLevel = 0;
    bool hapticEnabled = false;
    bool breakOpenEnabled = false;
    bool operator==(const RuntimeConfig &) const = default;
};

enum class ConfigLoadResult { Loaded, Migrated, Missing, Invalid, IoError };

// Roots are Launcher-owned data directories, preferred device-encrypted first.
// A malformed current file must never resurrect an older legacy cache value.
class RuntimeConfigStore {
public:
    explicit RuntimeConfigStore(std::vector<std::string> roots);
    ConfigLoadResult load(RuntimeConfig *out) const;
    bool save(const RuntimeConfig &config) const;
    static bool valid(const RuntimeConfig &config);
    static std::string encode(const RuntimeConfig &config);
    static bool decode(const std::string &text, RuntimeConfig *out);

private:
    std::vector<std::string> roots_;
};

}  // namespace swipegate
