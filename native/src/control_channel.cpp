#include "control_channel.h"
#include "native_api.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
alignas(8) void *g_swipegate_original_broadcast_send = nullptr;
void SwipeGateBroadcastSendCaptureHook();
void SwipeGateCaptureBroadcastRuntime(void *holder);
}

namespace {

constexpr const char *kTag = "HyperOS4SwipeGateRuntime";
constexpr const char *kLauncherPackage = "com.miui.home";
constexpr const char *kSystemUiPackage = "com.android.systemui";
constexpr const char *kLauncherLibrary = "libapp_launcher.so";
constexpr const char *kBroadcastPrivateName = "libhyper_os_broadcast_private.dylib.so";
constexpr const char *kCarrierAction = "com.android.systemui.fsgesture";
constexpr const char *kNativeReplyAction =
        "io.github.pzhown.hyperos4swipegate.action.NATIVE_RUNTIME_REPLY";

constexpr const char *kMarkerExtra = "swipegate_control";
constexpr const char *kNonceExtra = "swipegate_nonce";
constexpr const char *kThresholdExtra = "swipegate_threshold_dp";
constexpr const char *kLogLevelExtra = "swipegate_log_level";
constexpr const char *kHapticEnabledExtra = "swipegate_haptic_enabled";
constexpr const char *kHookStateExtra = "swipegate_hook_state";
constexpr const char *kPatternExtra = "swipegate_pattern";
constexpr const char *kDetailExtra = "swipegate_detail";
constexpr const char *kNativeLogExtra = "swipegate_native_log";
constexpr const char *kSenderUidExtra = "sender_uid";

constexpr const char *kConfigFileName = "hyperos4swipegate_config";
constexpr const char *kLogLevelFileName = "hyperos4swipegate_log_level";
constexpr int kAndroidUserOffset = 100000;
constexpr int kMinThresholdDp = 88;
constexpr int kMaxThresholdDp = 300;
constexpr int kMinLogLevel = 0;
constexpr int kMaxLogLevel = 2;
constexpr int kProbeIntervalMs = 100;
constexpr int kProbeAttempts = 300;
constexpr size_t kMaxPatternBytes = 128;
constexpr size_t kMaxDetailBytes = 768;
constexpr size_t kMaxAppLogBytes = 12 * 1024;

constexpr const char *kBroadcastReceiverOnReceiveSymbol =
        "_RNvMs3_NtNtCslLvADlVgqlk_26hyper_os_broadcast_private13dyn_"
        "broadcast23BroadcastReceiver_traitINtB5_20BroadcastReceiver_TOINtNtNtNt"
        "Cs9Neji4M1weT_10abi_stable9std_types5boxed7private4RBoxuEE10on_receiveB9_";
constexpr const char *kBroadcastSendSymbol =
        "_RNvNtNtCslLvADlVgqlk_26hyper_os_broadcast_private5scene5impls14send_broadcast";

enum HookState : uint32_t {
    kHookUnknown = 0,
    kHookWaiting = 1,
    kHookHealthy = 2,
    kHookRepairing = 3,
    kHookFailed = 4,
};

struct RString {
    char *data;
    uintptr_t length;
    uintptr_t capacity;
    const void *vtable;
};

struct ROptionRString {
    uint8_t tag;
    uint8_t padding[7];
    RString value;
};

struct BorrowedROptionRString {
    uint8_t tag;
    uint8_t padding[7];
    const char *data;
    uintptr_t length;
};

struct NativeResult {
    uint8_t bytes[48];
};

struct NativeI64Option {
    uint64_t tag;
    int64_t value;
};

using IntentGetActionFn = BorrowedROptionRString (*)(void *);
using IntentGetSenderPackageFn = BorrowedROptionRString (*)(void *);
using IntentGetExtrasFn = void *(*)(void *);
using IntentDefaultFn = void *(*)();
using IntentDropFn = void (*)(void *);
using IntentSetStringFn = void (*)(void *, ROptionRString *);
using IntentSetExtrasFn = void (*)(void *, void *);
using BundleDefaultFn = void *(*)();
using BundleGetBoolFn = uint64_t (*)(void *, const char *, size_t);
using BundleGetI32Fn = uint64_t (*)(void *, const char *, size_t);
using BundleGetI64Fn = NativeI64Option (*)(void *, const char *, size_t);
using BundleInsertBoolFn = void (*)(void *, RString *, uint8_t);
using BundleInsertI32Fn = void (*)(void *, RString *, int32_t);
using BundleInsertI64Fn = void (*)(void *, RString *, int64_t);
using BundleInsertStringFn = void (*)(void *, RString *, ROptionRString *);
using RuntimeStrongFn = void (*)(void *);
using BroadcastSendFn = NativeResult (*)(void *, void *);
using PackageManagerGetApplicationInfoFn = NativeResult (*)(const char *, size_t, uint64_t);
using ApplicationInfoGetUidFn = int32_t (*)(void *);
using ApplicationInfoDropFn = void (*)(void *);
using BroadcastReceiverOnReceiveFn = void (*)(void *, void *, void *);

static_assert(sizeof(RString) == 32u);
static_assert(sizeof(ROptionRString) == 40u);
static_assert(sizeof(BorrowedROptionRString) == 24u);

std::atomic_flag gStateLock = ATOMIC_FLAG_INIT;
std::atomic<HookFunType> gHookFunction{nullptr};
std::atomic<bool> gInstallerStarted{false};
std::atomic<bool> gChildProbeStarted{false};
std::atomic<bool> gReceiverHookInstalled{false};
std::atomic<bool> gSendCaptureHookInstalled{false};
std::atomic<bool> gRStringVtableCaptureHookInstalled{false};
std::atomic<void *> gOriginalReceiver{nullptr};
std::atomic<void *> gOriginalIntentSetAction{nullptr};
std::atomic<void *> gCapturedRuntime{nullptr};
std::atomic<void *> gCapturedRStringVtable{nullptr};
std::atomic<int64_t> gLastAcceptedCarrierNonce{0};

int gThresholdDp = -1;
int gLogLevel = -1;
int gHapticEnabled = -1;
HookState gHookState = kHookWaiting;
std::string gPattern;
std::string gDetail = "Native 模块已加载，等待 HyOS Runtime / Hook";
std::string gAppLog;

class SpinGuard {
public:
    SpinGuard() {
        while (gStateLock.test_and_set(std::memory_order_acquire)) sched_yield();
    }
    ~SpinGuard() { gStateLock.clear(std::memory_order_release); }
};

struct LoadedImage {
    uintptr_t base = 0;
    std::string path;
};

struct ImageSearch {
    const char *needle = nullptr;
    LoadedImage result;
};

int imageSearchCallback(dl_phdr_info *info, size_t, void *opaque) {
    if (info == nullptr || info->dlpi_name == nullptr || opaque == nullptr) return 0;
    auto *search = static_cast<ImageSearch *>(opaque);
    const std::string path(info->dlpi_name);
    if (search->needle == nullptr || path.find(search->needle) == std::string::npos) return 0;
    search->result.base = static_cast<uintptr_t>(info->dlpi_addr);
    search->result.path = path;
    return 1;
}

LoadedImage findLoadedImage(const char *needle) {
    ImageSearch search{needle, {}};
    dl_iterate_phdr(imageSearchCallback, &search);
    return search.result;
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

constexpr size_t constStringLength(const char *value) {
    size_t length = 0;
    if (value == nullptr) return 0;
    while (value[length] != '\0') ++length;
    return length;
}

bool borrowedEquals(const BorrowedROptionRString &value, const char *expected) {
    const size_t expectedLength = constStringLength(expected);
    return value.tag == 0u && value.data != nullptr && value.length == expectedLength
            && std::memcmp(value.data, expected, expectedLength) == 0;
}

void bridgeLog(int priority, const char *message) {
    __android_log_write(priority, kTag, message == nullptr ? "" : message);
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
    char text[24]{};
    const int length = std::snprintf(text, sizeof(text), "%d\n", value);
    if (length <= 0) return;
    for (size_t index = 0; index < 3; ++index) {
        if (index == 2 && userId != 0) break;
        if (index < 2) std::snprintf(path, sizeof(path), formats[index], userId, fileName);
        else std::snprintf(path, sizeof(path), formats[index], fileName);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        const ssize_t written = write(fd, text, static_cast<size_t>(length));
        close(fd);
        if (written == length) return;
    }
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
    if (text == nullptr || *text == '\0' || gLogLevel <= 0) return;
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

void parseHookBackendFromLog(const char *text) {
    if (text == nullptr || std::strstr(text, "DP_GATE native_init accepted") == nullptr) return;
    const char *field = std::strstr(text, "hook_func=");
    if (field == nullptr) return;
    field += std::strlen("hook_func=");
    char *end = nullptr;
    const unsigned long long raw = std::strtoull(field, &end, 0);
    if (end == field || raw < 0x10000ull) return;
    gHookFunction.store(reinterpret_cast<HookFunType>(static_cast<uintptr_t>(raw)),
                        std::memory_order_release);
}

bool preadExact(int fd, void *buffer, size_t size, off_t offset) {
    auto *bytes = static_cast<uint8_t *>(buffer);
    size_t done = 0;
    while (done < size) {
        const ssize_t result = pread(fd, bytes + done, size - done,
                                     offset + static_cast<off_t>(done));
        if (result <= 0) return false;
        done += static_cast<size_t>(result);
    }
    return true;
}

void *resolveExactFileSymbol(const LoadedImage &image, const char *name) {
    if (image.base == 0 || image.path.empty() || name == nullptr) return nullptr;
    const int fd = open(image.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;

    Elf64_Ehdr header{};
    if (!preadExact(fd, &header, sizeof(header), 0)
            || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0
            || header.e_ident[EI_CLASS] != ELFCLASS64
            || header.e_machine != EM_AARCH64
            || header.e_shentsize != sizeof(Elf64_Shdr)
            || header.e_shnum == 0 || header.e_shnum > 4096) {
        close(fd);
        return nullptr;
    }

    std::vector<Elf64_Shdr> sections(header.e_shnum);
    if (!preadExact(fd, sections.data(), sections.size() * sizeof(Elf64_Shdr),
                    static_cast<off_t>(header.e_shoff))) {
        close(fd);
        return nullptr;
    }

    uintptr_t matchedValue = 0;
    int matches = 0;
    for (const Elf64_Shdr &symbols : sections) {
        if ((symbols.sh_type != SHT_DYNSYM && symbols.sh_type != SHT_SYMTAB)
                || symbols.sh_entsize != sizeof(Elf64_Sym)
                || symbols.sh_link >= sections.size() || symbols.sh_size == 0) continue;
        const Elf64_Shdr &strings = sections[symbols.sh_link];
        if (strings.sh_size == 0 || strings.sh_size > 64 * 1024 * 1024) continue;
        std::vector<char> stringTable(static_cast<size_t>(strings.sh_size));
        if (!preadExact(fd, stringTable.data(), stringTable.size(),
                        static_cast<off_t>(strings.sh_offset))) continue;
        const size_t symbolCount = static_cast<size_t>(symbols.sh_size / sizeof(Elf64_Sym));
        std::vector<Elf64_Sym> table(symbolCount);
        if (!preadExact(fd, table.data(), table.size() * sizeof(Elf64_Sym),
                        static_cast<off_t>(symbols.sh_offset))) continue;
        for (const Elf64_Sym &symbol : table) {
            if (symbol.st_name >= stringTable.size() || symbol.st_value == 0) continue;
            const char *candidate = stringTable.data() + symbol.st_name;
            if (std::strcmp(candidate, name) != 0) continue;
            matchedValue = static_cast<uintptr_t>(symbol.st_value);
            ++matches;
            if (matches > 1) break;
        }
        if (matches > 1) break;
    }
    close(fd);
    return matches == 1 ? reinterpret_cast<void *>(image.base + matchedValue) : nullptr;
}

template <typename T>
T resolveLauncherSymbol(const char *name) {
    void *resolved = dlsym(RTLD_DEFAULT, name);
    if (resolved != nullptr) return reinterpret_cast<T>(resolved);
    const LoadedImage launcher = findLoadedImage(kLauncherLibrary);
    if (launcher.base == 0) return nullptr;
    void *handle = dlopen(launcher.path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) handle = dlopen(kLauncherLibrary, RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) return nullptr;
    return reinterpret_cast<T>(dlsym(handle, name));
}

bool isNativeSuccess(const NativeResult &result) {
    return (result.bytes[0] & uint8_t{1}) == 0u;
}

bool readNativeBool(void *bundle, const char *key, bool *output) {
    const auto get = resolveLauncherSymbol<BundleGetBoolFn>("Bundle_get_boolean");
    if (bundle == nullptr || key == nullptr || output == nullptr || get == nullptr) return false;
    const uint64_t encoded = get(bundle, key, constStringLength(key));
    if ((encoded & uint64_t{1}) != 0u) return false;
    *output = ((encoded >> 8u) & uint64_t{1}) != 0u;
    return true;
}

bool readNativeI32(void *bundle, const char *key, int32_t *output) {
    const auto get = resolveLauncherSymbol<BundleGetI32Fn>("Bundle_get_i32");
    if (bundle == nullptr || key == nullptr || output == nullptr || get == nullptr) return false;
    const uint64_t encoded = get(bundle, key, constStringLength(key));
    if ((encoded & uint64_t{1}) != 0u) return false;
    *output = static_cast<int32_t>(encoded >> 32u);
    return true;
}

bool readNativeI64(void *bundle, const char *key, int64_t *output) {
    const auto get = resolveLauncherSymbol<BundleGetI64Fn>("Bundle_get_i64");
    if (bundle == nullptr || key == nullptr || output == nullptr || get == nullptr) return false;
    const NativeI64Option encoded = get(bundle, key, constStringLength(key));
    if ((encoded.tag & uint64_t{1}) != 0u) return false;
    *output = encoded.value;
    return true;
}

bool verifyPackageUid(const char *packageName, int32_t claimedUid) {
    const auto getInfo = resolveLauncherSymbol<PackageManagerGetApplicationInfoFn>(
            "PackageManager_get_application_info");
    const auto getUid = resolveLauncherSymbol<ApplicationInfoGetUidFn>("ApplicationInfo_get_uid");
    const auto drop = resolveLauncherSymbol<ApplicationInfoDropFn>("ApplicationInfo_drop");
    if (packageName == nullptr || claimedUid < 0 || getInfo == nullptr
            || getUid == nullptr || drop == nullptr) return false;
    NativeResult result = getInfo(packageName, constStringLength(packageName), 0u);
    if (!isNativeSuccess(result)) return false;
    void *applicationInfo = nullptr;
    std::memcpy(&applicationInfo, result.bytes + 8u, sizeof(applicationInfo));
    if (applicationInfo == nullptr) return false;
    const int32_t actualUid = getUid(applicationInfo);
    drop(applicationInfo);
    return actualUid == claimedUid;
}

const void *rStringVtable() {
    return gCapturedRStringVtable.load(std::memory_order_acquire);
}

bool makeOwnedRString(const std::string &source, RString *output) {
    if (output == nullptr) return false;
    const void *vtable = rStringVtable();
    if (vtable == nullptr) return false;
    const size_t length = source.size();
    char *data = static_cast<char *>(std::malloc(length == 0 ? 1 : length));
    if (data == nullptr) return false;
    if (length != 0) std::memcpy(data, source.data(), length);
    output->data = data;
    output->length = length;
    output->capacity = length;
    output->vtable = vtable;
    return true;
}

bool makeOwnedROptionString(const std::string &source, ROptionRString *output) {
    if (output == nullptr) return false;
    std::memset(output, 0, sizeof(*output));
    return makeOwnedRString(source, &output->value);
}

bool addBundleI32(void *bundle, const char *key, int32_t value) {
    const auto insert = resolveLauncherSymbol<BundleInsertI32Fn>("Bundle_insert_i32");
    RString owned{};
    if (insert == nullptr || !makeOwnedRString(key, &owned)) return false;
    insert(bundle, &owned, value);
    return true;
}

bool addBundleI64(void *bundle, const char *key, int64_t value) {
    const auto insert = resolveLauncherSymbol<BundleInsertI64Fn>("Bundle_insert_i64");
    RString owned{};
    if (insert == nullptr || !makeOwnedRString(key, &owned)) return false;
    insert(bundle, &owned, value);
    return true;
}

bool addBundleBool(void *bundle, const char *key, bool value) {
    const auto insert = resolveLauncherSymbol<BundleInsertBoolFn>("Bundle_insert_boolean");
    RString owned{};
    if (insert == nullptr || !makeOwnedRString(key, &owned)) return false;
    insert(bundle, &owned, value ? uint8_t{1} : uint8_t{0});
    return true;
}

bool addBundleString(void *bundle, const char *key, const std::string &value) {
    const auto insert = resolveLauncherSymbol<BundleInsertStringFn>("Bundle_insert_string");
    RString ownedKey{};
    ROptionRString ownedValue{};
    if (insert == nullptr || !makeOwnedRString(key, &ownedKey)
            || !makeOwnedROptionString(value, &ownedValue)) return false;
    insert(bundle, &ownedKey, &ownedValue);
    return true;
}

bool intentActionEquals(void *intent, const char *expected) {
    const auto getAction = resolveLauncherSymbol<IntentGetActionFn>("Intent_get_action");
    if (intent == nullptr || expected == nullptr || getAction == nullptr) return false;
    return borrowedEquals(getAction(intent), expected);
}

bool intentSenderEquals(void *intent, const char *expected) {
    const auto getSender = resolveLauncherSymbol<IntentGetSenderPackageFn>(
            "Intent_get_sender_package_name");
    if (intent == nullptr || expected == nullptr || getSender == nullptr) return false;
    return borrowedEquals(getSender(intent), expected);
}

void hookIntentSetActionCapture(void *intent, ROptionRString *value) {
    if (value != nullptr && value->tag == 0u
            && value->value.vtable != nullptr
            && reinterpret_cast<uintptr_t>(value->value.vtable) >= 0x10000u
            && value->value.length <= 4096u) {
        void *expected = nullptr;
        void *captured = const_cast<void *>(value->value.vtable);
        if (gCapturedRStringVtable.compare_exchange_strong(
                expected, captured, std::memory_order_acq_rel)) {
            char line[192]{};
            std::snprintf(line, sizeof(line),
                          "RSTRING_VTABLE captured feature=Intent_set_action vtable=%p",
                          captured);
            bridgeLog(ANDROID_LOG_INFO, line);
        }
    }

    const auto original = reinterpret_cast<IntentSetStringFn>(
            gOriginalIntentSetAction.load(std::memory_order_acquire));
    if (original != nullptr) original(intent, value);
}

bool sendNativeReply(int64_t nonce) {
    void *runtime = gCapturedRuntime.load(std::memory_order_acquire);
    if (reinterpret_cast<uintptr_t>(runtime) < 0x100000000ull) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=runtime-not-captured");
        return false;
    }
    if (rStringVtable() == nullptr) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=rstring-vtable-not-captured");
        return false;
    }

    const auto intentDefault = resolveLauncherSymbol<IntentDefaultFn>("Intent_default");
    const auto intentDrop = resolveLauncherSymbol<IntentDropFn>("Intent_drop");
    const auto setAction = resolveLauncherSymbol<IntentSetStringFn>("Intent_set_action");
    const auto setPackage = resolveLauncherSymbol<IntentSetStringFn>("Intent_set_package");
    const auto setExtras = resolveLauncherSymbol<IntentSetExtrasFn>("Intent_set_extras");
    const auto bundleDefault = resolveLauncherSymbol<BundleDefaultFn>("Bundle_default");
    const auto send = resolveLauncherSymbol<BroadcastSendFn>("Broadcast_send_broadcast");
    const auto inc = resolveLauncherSymbol<RuntimeStrongFn>("Runtime_inc_strong");
    const auto dec = resolveLauncherSymbol<RuntimeStrongFn>("Runtime_dec_strong");
    if (intentDefault == nullptr || intentDrop == nullptr || setAction == nullptr
            || setPackage == nullptr || setExtras == nullptr || bundleDefault == nullptr
            || send == nullptr || inc == nullptr || dec == nullptr) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=runtime-symbols-unavailable");
        return false;
    }

    HookState state;
    int threshold;
    int logLevel;
    int hapticEnabled;
    std::string pattern;
    std::string detail;
    std::string appLog;
    {
        SpinGuard guard;
        state = gHookState;
        threshold = gThresholdDp;
        logLevel = gLogLevel;
        hapticEnabled = gHapticEnabled;
        pattern = gPattern.substr(0, kMaxPatternBytes);
        detail = gDetail.substr(0, kMaxDetailBytes);
        appLog = gAppLog.substr(gAppLog.size() > kMaxAppLogBytes
                ? gAppLog.size() - kMaxAppLogBytes : 0);
    }

    void *extras = bundleDefault();
    if (extras == nullptr
            || !addBundleBool(extras, kMarkerExtra, true)
            || !addBundleI64(extras, kNonceExtra, nonce)
            || !addBundleI32(extras, kHookStateExtra, static_cast<int32_t>(state))
            || !addBundleI32(extras, kThresholdExtra, threshold)
            || !addBundleI32(extras, kLogLevelExtra, logLevel)
            || !addBundleBool(extras, kHapticEnabledExtra, hapticEnabled > 0)
            || !addBundleI32(extras, kSenderUidExtra, static_cast<int32_t>(getuid()))
            || !addBundleString(extras, kPatternExtra, pattern)
            || !addBundleString(extras, kDetailExtra, detail)
            || !addBundleString(extras, kNativeLogExtra, appLog)) return false;

    void *intent = intentDefault();
    if (intent == nullptr) return false;
    ROptionRString action{};
    ROptionRString package{};
    if (!makeOwnedROptionString(kNativeReplyAction, &action)
            || !makeOwnedROptionString(kSystemUiPackage, &package)) {
        intentDrop(intent);
        return false;
    }
    setAction(intent, &action);
    setPackage(intent, &package);
    setExtras(intent, extras);

    inc(runtime);
    void *sharedRuntime = runtime;
    const NativeResult result = send(&sharedRuntime, intent);
    dec(runtime);
    intentDrop(intent);
    return isNativeSuccess(result);
}

void handleControlCarrier(void *intent) {
    const auto getExtras = resolveLauncherSymbol<IntentGetExtrasFn>("Intent_get_extras");
    if (intent == nullptr || getExtras == nullptr || !intentActionEquals(intent, kCarrierAction)) return;

    // HyperOS Runtime supplies sender_package_name independently of Intent extras. Treat that as
    // the authentication boundary. sender_uid remains a best-effort cross-check only because
    // Xiaomi's private PackageManager ABI can differ across launcher/runtime builds.
    if (!intentSenderEquals(intent, kSystemUiPackage)) {
        bridgeLog(ANDROID_LOG_WARN, "CONTROL_CARRIER rejected reason=runtime-sender-not-systemui");
        return;
    }

    void *extras = getExtras(intent);
    bool marker = false;
    int32_t senderUid = -1;
    int32_t thresholdDp = -1;
    int32_t logLevel = -1;
    bool hapticEnabled = false;
    int64_t nonce = 0;

    const bool markerRead = readNativeBool(extras, kMarkerExtra, &marker);
    const bool senderUidRead = readNativeI32(extras, kSenderUidExtra, &senderUid);
    const bool nonceRead = readNativeI64(extras, kNonceExtra, &nonce);
    const bool thresholdRead = readNativeI32(extras, kThresholdExtra, &thresholdDp);
    const bool logLevelRead = readNativeI32(extras, kLogLevelExtra, &logLevel);
    const bool hapticFieldPresent = readNativeBool(extras, kHapticEnabledExtra, &hapticEnabled);

    char carrierLog[320]{};
    if (!markerRead || !marker) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER rejected reason=marker read=%d value=%d",
                      markerRead ? 1 : 0, marker ? 1 : 0);
        bridgeLog(ANDROID_LOG_WARN, carrierLog);
        return;
    }
    if (!nonceRead || nonce <= 0) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER rejected reason=nonce read=%d value=%lld",
                      nonceRead ? 1 : 0, static_cast<long long>(nonce));
        bridgeLog(ANDROID_LOG_WARN, carrierLog);
        return;
    }
    if (!thresholdRead || thresholdDp < kMinThresholdDp || thresholdDp > kMaxThresholdDp) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER rejected reason=threshold read=%d value=%d range=%d..%d",
                      thresholdRead ? 1 : 0, thresholdDp, kMinThresholdDp, kMaxThresholdDp);
        bridgeLog(ANDROID_LOG_WARN, carrierLog);
        return;
    }
    if (!logLevelRead || logLevel < kMinLogLevel || logLevel > kMaxLogLevel) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER rejected reason=log-level read=%d value=%d range=%d..%d",
                      logLevelRead ? 1 : 0, logLevel, kMinLogLevel, kMaxLogLevel);
        bridgeLog(ANDROID_LOG_WARN, carrierLog);
        return;
    }

    if (senderUidRead && !verifyPackageUid(kSystemUiPackage, senderUid)) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER sender_uid cross-check unavailable-or-mismatch claimed=%d; accepted-by-runtime-sender=1",
                      senderUid);
        bridgeLog(ANDROID_LOG_WARN, carrierLog);
    } else if (!senderUidRead) {
        bridgeLog(ANDROID_LOG_WARN,
                  "CONTROL_CARRIER sender_uid missing; accepted-by-runtime-sender=1");
    }

    if (!hapticFieldPresent) {
        {
            SpinGuard guard;
            hapticEnabled = gHapticEnabled == 1;
        }
        bridgeLog(ANDROID_LOG_WARN,
                  "CONTROL_CARRIER haptic field missing; preserving previous/default state");
    }

    const int64_t previousNonce = gLastAcceptedCarrierNonce.exchange(
            nonce, std::memory_order_acq_rel);
    if (previousNonce == nonce) {
        std::snprintf(carrierLog, sizeof(carrierLog),
                      "CONTROL_CARRIER duplicate nonce=%lld; state unchanged, reply retry=%d",
                      static_cast<long long>(nonce), sendNativeReply(nonce) ? 1 : 0);
        bridgeLog(ANDROID_LOG_DEBUG, carrierLog);
        return;
    }

    bool thresholdChanged;
    bool logLevelChanged;
    bool hapticChanged;
    {
        SpinGuard guard;
        thresholdChanged = gThresholdDp != thresholdDp;
        logLevelChanged = gLogLevel != logLevel;
        hapticChanged = gHapticEnabled != (hapticEnabled ? 1 : 0);
        gThresholdDp = thresholdDp;
        gLogLevel = logLevel;
        gHapticEnabled = hapticEnabled ? 1 : 0;
        if (logLevel <= 0) gAppLog.clear();
    }
    if (thresholdChanged) persistValue(kConfigFileName, thresholdDp);
    if (logLevelChanged) persistValue(kLogLevelFileName, logLevel);

    std::snprintf(carrierLog, sizeof(carrierLog),
                  "CONTROL_CARRIER accepted nonce=%lld threshold=%d logLevel=%d haptic=%d senderUidRead=%d",
                  static_cast<long long>(nonce), thresholdDp, logLevel, hapticEnabled ? 1 : 0,
                  senderUidRead ? 1 : 0);
    bridgeLog(ANDROID_LOG_INFO, carrierLog);

    if (!sendNativeReply(nonce)) {
        bridgeLog(ANDROID_LOG_WARN,
                "Runtime carrier accepted but native reply is not ready; waiting for HyperOS Runtime capture");
    }
}

void hookBroadcastReceiverOnReceive(void *receiver, void *context, void *intent) {
    const auto original = reinterpret_cast<BroadcastReceiverOnReceiveFn>(
            gOriginalReceiver.load(std::memory_order_acquire));
    if (original == nullptr) return;
    if (intent != nullptr && intentActionEquals(intent, kCarrierAction)) handleControlCarrier(intent);
    original(receiver, context, intent);
}

void *installerMain(void *) {
    for (int attempt = 0; attempt < kProbeAttempts; ++attempt) {
        if (!isLauncherProcess()) {
            usleep(kProbeIntervalMs * 1000);
            continue;
        }
        HookFunType hook = gHookFunction.load(std::memory_order_acquire);
        const LoadedImage broadcastImage = findLoadedImage(kBroadcastPrivateName);
        const LoadedImage launcherImage = findLoadedImage(kLauncherLibrary);
        if (hook != nullptr && broadcastImage.base != 0 && launcherImage.base != 0) {
            if (!gSendCaptureHookInstalled.load(std::memory_order_acquire)) {
                void *target = resolveExactFileSymbol(broadcastImage, kBroadcastSendSymbol);
                if (target != nullptr
                        && hook(target, reinterpret_cast<void *>(SwipeGateBroadcastSendCaptureHook),
                                &g_swipegate_original_broadcast_send) == 0
                        && g_swipegate_original_broadcast_send != nullptr) {
                    gSendCaptureHookInstalled.store(true, std::memory_order_release);
                    bridgeLog(ANDROID_LOG_INFO, "HyOS private broadcast runtime capture installed");
                }
            }
            if (!gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) {
                const auto targetFn = resolveLauncherSymbol<IntentSetStringFn>("Intent_set_action");
                void *target = reinterpret_cast<void *>(targetFn);
                void *backup = nullptr;
                if (target != nullptr
                        && hook(target, reinterpret_cast<void *>(hookIntentSetActionCapture),
                                &backup) == 0 && backup != nullptr) {
                    gOriginalIntentSetAction.store(backup, std::memory_order_release);
                    gRStringVtableCaptureHookInstalled.store(true, std::memory_order_release);
                    bridgeLog(ANDROID_LOG_INFO,
                              "RSTRING_VTABLE capture hook installed feature=Intent_set_action");
                }
            }
            if (!gReceiverHookInstalled.load(std::memory_order_acquire)) {
                void *target = resolveExactFileSymbol(broadcastImage,
                                                       kBroadcastReceiverOnReceiveSymbol);
                void *backup = nullptr;
                if (target != nullptr
                        && hook(target, reinterpret_cast<void *>(hookBroadcastReceiverOnReceive),
                                &backup) == 0 && backup != nullptr) {
                    gOriginalReceiver.store(backup, std::memory_order_release);
                    gReceiverHookInstalled.store(true, std::memory_order_release);
                    bridgeLog(ANDROID_LOG_INFO, "HyOS fsgesture native receiver bridge installed");
                }
            }
            if (gSendCaptureHookInstalled.load(std::memory_order_acquire)
                    && gReceiverHookInstalled.load(std::memory_order_acquire)
                    && gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) {
                gInstallerStarted.store(false, std::memory_order_release);
                return nullptr;
            }
        }
        usleep(kProbeIntervalMs * 1000);
    }
    bridgeLog(ANDROID_LOG_ERROR,
            "HyOS Runtime bridge install timed out; gesture hook remains fail-independent");
    gInstallerStarted.store(false, std::memory_order_release);
    return nullptr;
}

void startInstallerIfLauncher() {
    if (!isLauncherProcess() || gHookFunction.load(std::memory_order_acquire) == nullptr) return;
    if (gReceiverHookInstalled.load(std::memory_order_acquire)
            && gSendCaptureHookInstalled.load(std::memory_order_acquire)
            && gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) return;
    bool expected = false;
    if (!gInstallerStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, installerMain, nullptr) != 0) {
        gInstallerStarted.store(false, std::memory_order_release);
        bridgeLog(ANDROID_LOG_ERROR, "Failed to start HyOS Runtime bridge installer");
        return;
    }
    pthread_detach(thread);
}

void *childProbeMain(void *) {
    for (int attempt = 0; attempt < kProbeAttempts; ++attempt) {
        if (isLauncherProcess()) {
            gChildProbeStarted.store(false, std::memory_order_release);
            startInstallerIfLauncher();
            return nullptr;
        }
        usleep(kProbeIntervalMs * 1000);
    }
    gChildProbeStarted.store(false, std::memory_order_release);
    return nullptr;
}

void startChildProbeAfterFork() {
    bool expected = false;
    if (!gChildProbeStarted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, childProbeMain, nullptr) != 0) {
        gChildProbeStarted.store(false, std::memory_order_release);
        return;
    }
    pthread_detach(thread);
}

void resetAfterFork() {
    gStateLock.clear(std::memory_order_release);
    gInstallerStarted.store(false, std::memory_order_release);
    gChildProbeStarted.store(false, std::memory_order_release);
    gReceiverHookInstalled.store(false, std::memory_order_release);
    gSendCaptureHookInstalled.store(false, std::memory_order_release);
    gRStringVtableCaptureHookInstalled.store(false, std::memory_order_release);
    gOriginalReceiver.store(nullptr, std::memory_order_release);
    gOriginalIntentSetAction.store(nullptr, std::memory_order_release);
    gCapturedRuntime.store(nullptr, std::memory_order_release);
    gCapturedRStringVtable.store(nullptr, std::memory_order_release);
    gLastAcceptedCarrierNonce.store(0, std::memory_order_release);
    startChildProbeAfterFork();
}

__attribute__((constructor)) void initializeRuntimeBridge() {
    (void) pthread_atfork(nullptr, nullptr, resetAfterFork);
}

}  // namespace

extern "C" void SwipeGateCaptureBroadcastRuntime(void *holder) {
    if (reinterpret_cast<uintptr_t>(holder) < 0x100000000ull) return;
    void *shared = *reinterpret_cast<void **>(holder);
    if (reinterpret_cast<uintptr_t>(shared) < 0x100000000ull) return;
    gCapturedRuntime.store(shared, std::memory_order_release);
}

extern "C" int swipegate_control_threshold_dp() {
    SpinGuard guard;
    return gThresholdDp;
}

extern "C" int swipegate_control_log_level() {
    SpinGuard guard;
    return gLogLevel;
}

extern "C" int swipegate_control_haptic_enabled() {
    SpinGuard guard;
    return gHapticEnabled;
}

extern "C" void swipegate_control_on_log(int, const char *text) {
    parseHookBackendFromLog(text);
    {
        SpinGuard guard;
        updateHookStateLocked(text);
        appendAppLogLocked(text);
    }
    startInstallerIfLauncher();
}

extern "C" void swipegate_control_sync_if_due() {
    startInstallerIfLauncher();
}
