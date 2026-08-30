from pathlib import Path

main = Path('native/src/main.cpp')
text = main.read_text()

old = 'constexpr const char *kStandardHapticSymbol = "HapticFeedback_perform_haptic_feedback";\n'
new = old + 'constexpr int64_t kHapticFeatureResolveIntervalMs = 5000;\n'
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = 'std::atomic<bool> gHapticCaptureHookInstalled{false};\nstd::atomic<bool> gHapticUnavailableLogged{false};\n'
new = ('std::atomic<bool> gHapticCaptureHookInstalled{false};\n'
       'std::atomic<bool> gHapticUnavailableLogged{false};\n'
       'std::atomic<int64_t> gLastHapticFeatureResolveMs{0};\n')
assert text.count(old) == 1
text = text.replace(old, new, 1)

marker = '''LibraryInfo findLauncherLibrary() {
    LibraryInfo result;
    dl_iterate_phdr(libraryCallback, &result);
    return result;
}

'''
insert = marker + r'''bool preadExactMain(int fd, void *buffer, size_t size, off_t offset) {
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

struct ImportedFunctionResolution {
    uintptr_t slot = 0;
    void *target = nullptr;
    size_t matches = 0;
};

ImportedFunctionResolution resolveImportedFunction(const LibraryInfo &library,
                                                   const char *symbolName) {
    ImportedFunctionResolution result{};
    if (library.base == 0 || library.path.empty() || symbolName == nullptr) return result;

    const int fd = open(library.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return result;

    Elf64_Ehdr header{};
    if (!preadExactMain(fd, &header, sizeof(header), 0)
            || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0
            || header.e_ident[EI_CLASS] != ELFCLASS64
            || header.e_machine != EM_AARCH64
            || header.e_shentsize != sizeof(Elf64_Shdr)
            || header.e_shnum == 0 || header.e_shnum > 4096) {
        close(fd);
        return result;
    }

    std::vector<Elf64_Shdr> sections(header.e_shnum);
    if (!preadExactMain(fd, sections.data(), sections.size() * sizeof(Elf64_Shdr),
                        static_cast<off_t>(header.e_shoff))) {
        close(fd);
        return result;
    }

    for (const Elf64_Shdr &relocations : sections) {
        if (relocations.sh_type != SHT_RELA || relocations.sh_entsize != sizeof(Elf64_Rela)
                || relocations.sh_link >= sections.size() || relocations.sh_size == 0) continue;
        const Elf64_Shdr &symbols = sections[relocations.sh_link];
        if ((symbols.sh_type != SHT_DYNSYM && symbols.sh_type != SHT_SYMTAB)
                || symbols.sh_entsize != sizeof(Elf64_Sym)
                || symbols.sh_link >= sections.size() || symbols.sh_size == 0) continue;
        const Elf64_Shdr &strings = sections[symbols.sh_link];
        if (strings.sh_size == 0 || strings.sh_size > 64 * 1024 * 1024
                || symbols.sh_size > 64 * 1024 * 1024
                || relocations.sh_size > 64 * 1024 * 1024) continue;

        std::vector<char> stringTable(static_cast<size_t>(strings.sh_size));
        std::vector<Elf64_Sym> symbolTable(
                static_cast<size_t>(symbols.sh_size / sizeof(Elf64_Sym)));
        std::vector<Elf64_Rela> relocationTable(
                static_cast<size_t>(relocations.sh_size / sizeof(Elf64_Rela)));
        if (!preadExactMain(fd, stringTable.data(), stringTable.size(),
                            static_cast<off_t>(strings.sh_offset))
                || !preadExactMain(fd, symbolTable.data(), symbolTable.size() * sizeof(Elf64_Sym),
                                   static_cast<off_t>(symbols.sh_offset))
                || !preadExactMain(fd, relocationTable.data(), relocationTable.size() * sizeof(Elf64_Rela),
                                   static_cast<off_t>(relocations.sh_offset))) continue;

        for (const Elf64_Rela &relocation : relocationTable) {
            const uint32_t type = ELF64_R_TYPE(relocation.r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
            const size_t symbolIndex = static_cast<size_t>(ELF64_R_SYM(relocation.r_info));
            if (symbolIndex >= symbolTable.size()) continue;
            const Elf64_Sym &symbol = symbolTable[symbolIndex];
            if (symbol.st_name >= stringTable.size()) continue;
            const char *candidate = stringTable.data() + symbol.st_name;
            if (std::strcmp(candidate, symbolName) != 0) continue;

            const uintptr_t slot = library.base + static_cast<uintptr_t>(relocation.r_offset);
            void *target = nullptr;
            std::memcpy(&target, reinterpret_cast<const void *>(slot), sizeof(target));
            if (reinterpret_cast<uintptr_t>(target) < 0x10000u) continue;

            if (result.matches == 0) {
                result.slot = slot;
                result.target = target;
                result.matches = 1;
            } else if (result.slot != slot || result.target != target) {
                ++result.matches;
                close(fd);
                return result;
            }
        }
    }
    close(fd);
    return result;
}

'''
assert text.count(marker) == 1
text = text.replace(marker, insert, 1)

start = text.index('bool installHapticCaptureHookFromHandle(void *handle) {')
end = text.index('\nbool performNativeHaptic(', start)
new_block = r'''bool installHapticCaptureHookTarget(void *target, void *standard, const char *source) {
    if (target == nullptr || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    bool expected = false;
    if (!gHapticInstallInProgress.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return gHapticCaptureHookInstalled.load(std::memory_order_acquire);
    }

    void *backup = nullptr;
    const int rc = gHookFunction(
            target, reinterpret_cast<void *>(hapticFeedbackCaptureHook), &backup);
    if (rc != 0 || backup == nullptr) {
        logLine(ANDROID_LOG_ERROR,
                "HAPTIC_V2 capture hook failed source=%s rc=%d target=%p backup=%p",
                source == nullptr ? "unknown" : source, rc, target, backup);
        gHapticInstallInProgress.store(false, std::memory_order_release);
        return false;
    }

    gOriginalHapticFeedback.store(backup, std::memory_order_release);
    gStandardHapticFeedback.store(standard, std::memory_order_release);
    gHapticCaptureHookInstalled.store(true, std::memory_order_release);
    gHapticInstallInProgress.store(false, std::memory_order_release);
    gHapticUnavailableLogged.store(false, std::memory_order_release);
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 capture hook ready source=%s target=%p standard=%p feature=%s watchdog=off no-dlopen=1",
            source == nullptr ? "unknown" : source, target, standard, kHapticSymbol);
    return true;
}

bool installHapticCaptureHookFromHandle(void *handle) {
    if (handle == nullptr || gHookFunction == nullptr || !isLauncherProcess()) return false;
    void *target = dlsym(handle, kHapticSymbol);
    void *standard = dlsym(handle, kStandardHapticSymbol);
    if (target == nullptr) return false;
    return installHapticCaptureHookTarget(target, standard, "provider-loader-fallback");
}

bool installHapticCaptureHookFromLauncherImport(const LibraryInfo &library, const char *source) {
    if (library.base == 0 || gHookFunction == nullptr || !isLauncherProcess()) return false;
    if (gHapticCaptureHookInstalled.load(std::memory_order_acquire)) return true;

    const ImportedFunctionResolution resolution = resolveImportedFunction(library, kHapticSymbol);
    if (resolution.matches != 1 || resolution.target == nullptr) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 feature import unresolved source=%s symbol=%s matches=%zu launcher=%s",
                source == nullptr ? "unknown" : source, kHapticSymbol,
                resolution.matches, library.path.c_str());
        return false;
    }
    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 feature import resolved source=%s symbol=%s got=%p target=%p launcher=%s",
            source == nullptr ? "unknown" : source, kHapticSymbol,
            reinterpret_cast<void *>(resolution.slot), resolution.target, library.path.c_str());
    return installHapticCaptureHookTarget(resolution.target, nullptr, "launcher-import-feature");
}
'''
text = text[:start] + new_block + text[end:]

start = text.index('uintptr_t resolveBackInvokeTarget(const LibraryInfo &library) {')
end = text.index('\nbool installBackInvokeHapticHook(', start)
new_block = r'''uintptr_t resolveBackInvokeTarget(const LibraryInfo &library, const char **featureName) {
    struct BackInvokeFeature {
        const char *name;
        const uint8_t *bytes;
        size_t size;
    };
    const BackInvokeFeature features[] = {
            {"back-invoke-frame-v1", kOnBackInvokePatternV1, sizeof(kOnBackInvokePatternV1)},
            {"back-invoke-frame-v2", kOnBackInvokePatternV2, sizeof(kOnBackInvokePatternV2)},
    };

    uintptr_t found = 0;
    const char *matched = nullptr;
    for (const BackInvokeFeature &feature : features) {
        const uintptr_t candidate = resolveUniqueAuxPattern(library, feature.bytes, feature.size);
        if (candidate == 0) continue;
        if (found != 0 && found != candidate) {
            if (featureName != nullptr) *featureName = "ambiguous";
            return 0;
        }
        found = candidate;
        matched = feature.name;
    }
    if (featureName != nullptr) *featureName = matched;
    return found;
}
'''
text = text[:start] + new_block + text[end:]

old = '''bool installBackInvokeHapticHook(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;
    const uintptr_t target = resolveBackInvokeTarget(library);
    if (target == 0) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 commit hook unresolved profile=%s; ready haptic remains available",
                gActivePatternName);
        return false;
    }
'''
new = '''bool installBackInvokeHapticHook(const LibraryInfo &library) {
    if (library.base == 0 || gHookFunction == nullptr) return false;
    const char *featureName = nullptr;
    const uintptr_t target = resolveBackInvokeTarget(library, &featureName);
    if (target == 0) {
        logLine(ANDROID_LOG_WARN,
                "HAPTIC_V2 commit hook unresolved feature=%s activeProfile=%s; ready haptic remains available",
                featureName == nullptr ? "none" : featureName, gActivePatternName);
        return false;
    }
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 commit hook ready profile=%s target=%p watchdog=off",
            gActivePatternName, reinterpret_cast<void *>(target));
'''
new = '''    logLine(ANDROID_LOG_INFO,
            "HAPTIC_V2 commit hook ready feature=%s activeProfile=%s target=%p watchdog=off",
            featureName == nullptr ? "unknown" : featureName, gActivePatternName,
            reinterpret_cast<void *>(target));
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''        if (library.base != 0) {
            missingPolls = 0;
            ensureHook(library, "watchdog");
        } else {
'''
new = '''        if (library.base != 0) {
            missingPolls = 0;
            ensureHook(library, "watchdog");
            if (!gHapticCaptureHookInstalled.load(std::memory_order_acquire)) {
                const int64_t now = monotonicMs();
                int64_t last = gLastHapticFeatureResolveMs.load(std::memory_order_relaxed);
                if (now - last >= kHapticFeatureResolveIntervalMs
                        && gLastHapticFeatureResolveMs.compare_exchange_strong(
                                last, now, std::memory_order_relaxed)) {
                    installHapticCaptureHookFromLauncherImport(library, "watchdog-feature-probe");
                }
            }
        } else {
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''        if (library.base != 0) {
            ensureHook(library, "loader-callback");
            if (isLauncherProcess()) ensureWorkerStarted();
        }
'''
new = '''        if (library.base != 0) {
            ensureHook(library, "loader-callback");
            if (isLauncherProcess()) {
                installHapticCaptureHookFromLauncherImport(library, "launcher-loader-callback");
                ensureWorkerStarted();
            }
        }
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '            "HAPTIC_V2 enabled policy=loader-handle-only library-needle=libhyper_os_background_tasks_public no-dlopen no-watchdog no-hook-mutex ready=standard-27 commit=ext-0");\n'
new = '            "HAPTIC_V2 enabled policy=launcher-import-feature-primary provider-loader-fallback no-dlopen no-hook-mutex ready=ext-fallback commit=ext-0");\n'
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '        ensureHook(library, "native-init-backfill");\n'
new = old + '        if (launcherProcess) installHapticCaptureHookFromLauncherImport(library, "native-init-feature-probe");\n'
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''// Exact patterns remain authoritative compatibility profiles for builds we have
// already validated. The semantic resolver is allowed to corroborate them, but
// an experimental semantic disagreement must never regress a known-good build.
// On an unknown build with no exact profile, semantic resolution may take over
// only when it proves exactly one candidate.
'''
new = '''// These byte patterns are feature fingerprints, not version gates. A newer Launcher may
// reuse the same gesture implementation and is compatible when the fingerprint still matches.
// Semantic resolution may corroborate a known fingerprint; on a changed implementation it may
// take over only when it proves exactly one candidate.
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

main.write_text(text)

control = Path('native/src/control_channel.cpp')
text = control.read_text()

old = '''// Verified directly from RELEASE-8.01.02.6174-260818-08281208-R's
// libapp_launcher.so. The value is the Rust String vtable used at the
// Bundle_insert_string and Intent_set_action callsites; do not reuse it for an unknown profile.
constexpr uintptr_t kRStringVtableOffset6174 = 0xe116c8u;

'''
assert text.count(old) == 1
text = text.replace(old, '', 1)

old = '''std::atomic<bool> gSendCaptureHookInstalled{false};
std::atomic<void *> gOriginalReceiver{nullptr};
std::atomic<void *> gCapturedRuntime{nullptr};
'''
new = '''std::atomic<bool> gSendCaptureHookInstalled{false};
std::atomic<bool> gRStringVtableCaptureHookInstalled{false};
std::atomic<void *> gOriginalReceiver{nullptr};
std::atomic<void *> gOriginalIntentSetAction{nullptr};
std::atomic<void *> gCapturedRuntime{nullptr};
std::atomic<void *> gCapturedRStringVtable{nullptr};
std::atomic<int64_t> gLastAcceptedCarrierNonce{0};
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

start = text.index('const void *rStringVtable() {')
end = text.index('\nbool makeOwnedRString(', start)
text = text[:start] + '''const void *rStringVtable() {
    return gCapturedRStringVtable.load(std::memory_order_acquire);
}
''' + text[end:]

marker = '''bool intentSenderEquals(void *intent, const char *expected) {
    const auto getSender = resolveLauncherSymbol<IntentGetSenderPackageFn>(
            "Intent_get_sender_package_name");
    if (intent == nullptr || expected == nullptr || getSender == nullptr) return false;
    return borrowedEquals(getSender(intent), expected);
}

'''
insert = marker + '''void hookIntentSetActionCapture(void *intent, ROptionRString *value) {
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

'''
assert text.count(marker) == 1
text = text.replace(marker, insert, 1)

old = '''bool sendNativeReply(int64_t nonce) {
    void *runtime = gCapturedRuntime.load(std::memory_order_acquire);
    if (reinterpret_cast<uintptr_t>(runtime) < 0x100000000ull) return false;

'''
new = '''bool sendNativeReply(int64_t nonce) {
    void *runtime = gCapturedRuntime.load(std::memory_order_acquire);
    if (reinterpret_cast<uintptr_t>(runtime) < 0x100000000ull) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=runtime-not-captured");
        return false;
    }
    if (rStringVtable() == nullptr) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=rstring-vtable-not-captured");
        return false;
    }

'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''            || setPackage == nullptr || setExtras == nullptr || bundleDefault == nullptr
            || send == nullptr || inc == nullptr || dec == nullptr || rStringVtable() == nullptr) {
        return false;
    }
'''
new = '''            || setPackage == nullptr || setExtras == nullptr || bundleDefault == nullptr
            || send == nullptr || inc == nullptr || dec == nullptr) {
        bridgeLog(ANDROID_LOG_WARN, "NATIVE_REPLY waiting reason=runtime-symbols-unavailable");
        return false;
    }
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''    if (!hapticFieldPresent) {
        {
            SpinGuard guard;
            hapticEnabled = gHapticEnabled == 1;
        }
        bridgeLog(ANDROID_LOG_WARN,
                  "CONTROL_CARRIER haptic field missing; preserving previous/default state");
    }

    bool thresholdChanged;
'''
new = '''    if (!hapticFieldPresent) {
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
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

marker = '            if (!gReceiverHookInstalled.load(std::memory_order_acquire)) {\n'
insert = '''            if (!gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) {
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
''' + marker
assert text.count(marker) == 1
text = text.replace(marker, insert, 1)

old = '''            if (gSendCaptureHookInstalled.load(std::memory_order_acquire)
                    && gReceiverHookInstalled.load(std::memory_order_acquire)) {
'''
new = '''            if (gSendCaptureHookInstalled.load(std::memory_order_acquire)
                    && gReceiverHookInstalled.load(std::memory_order_acquire)
                    && gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) {
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''    if (gReceiverHookInstalled.load(std::memory_order_acquire)
            && gSendCaptureHookInstalled.load(std::memory_order_acquire)) return;
'''
new = '''    if (gReceiverHookInstalled.load(std::memory_order_acquire)
            && gSendCaptureHookInstalled.load(std::memory_order_acquire)
            && gRStringVtableCaptureHookInstalled.load(std::memory_order_acquire)) return;
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

old = '''    gReceiverHookInstalled.store(false, std::memory_order_release);
    gSendCaptureHookInstalled.store(false, std::memory_order_release);
    gOriginalReceiver.store(nullptr, std::memory_order_release);
    gCapturedRuntime.store(nullptr, std::memory_order_release);
'''
new = '''    gReceiverHookInstalled.store(false, std::memory_order_release);
    gSendCaptureHookInstalled.store(false, std::memory_order_release);
    gRStringVtableCaptureHookInstalled.store(false, std::memory_order_release);
    gOriginalReceiver.store(nullptr, std::memory_order_release);
    gOriginalIntentSetAction.store(nullptr, std::memory_order_release);
    gCapturedRuntime.store(nullptr, std::memory_order_release);
    gCapturedRStringVtable.store(nullptr, std::memory_order_release);
    gLastAcceptedCarrierNonce.store(0, std::memory_order_release);
'''
assert text.count(old) == 1
text = text.replace(old, new, 1)

control.write_text(text)

gradle = Path('app/build.gradle.kts')
text = gradle.read_text()
assert 'versionCode = 37' in text
assert 'versionName = "0.8.0-dev.5"' in text
text = text.replace('versionCode = 37', 'versionCode = 38', 1)
text = text.replace('versionName = "0.8.0-dev.5"', 'versionName = "0.8.0-dev.6"', 1)
gradle.write_text(text)
