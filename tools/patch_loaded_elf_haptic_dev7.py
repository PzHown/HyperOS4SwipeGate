from pathlib import Path

main = Path('native/src/main.cpp')
text = main.read_text()

text = text.replace('{"8.01.02.5459-v1", kOnSwipeProcessPatternV1, kOnSwipeProcessMaskV1,',
                    '{"gesture-frame-v1", kOnSwipeProcessPatternV1, kOnSwipeProcessMaskV1,', 1)
text = text.replace('{"8.01.02.6174-v2", kOnSwipeProcessPatternV2, kOnSwipeProcessMaskV2,',
                    '{"gesture-frame-v2", kOnSwipeProcessPatternV2, kOnSwipeProcessMaskV2,', 1)
text = text.replace('constexpr size_t kMaxExecutableRanges = 12;\n',
                    'constexpr size_t kMaxExecutableRanges = 12;\nconstexpr size_t kMaxLoadRanges = 16;\n', 1)

old_comment = '''// Haptic V2 is deliberately isolated from gHookMutex and the hook-health watchdog.\n// The haptic library is never dlopen'ed by this module: LSPosed gives us the handle\n// after the library is loaded and we resolve symbols exactly once from that handle.\n'''
new_comment = '''// Haptic V2 is deliberately isolated from gHookMutex. Primary discovery follows the\n// production Android hook-library pattern used by xHook/ByteHook/xDL: inspect the already\n// loaded Launcher's PT_DYNAMIC metadata in memory. No APK path open and no haptic dlopen.\n'''
assert text.count(old_comment) == 1
text = text.replace(old_comment, new_comment, 1)

old_struct = '''struct LibraryInfo {\n    uintptr_t base = 0;\n    std::string path;\n    std::array<ExecutableRange, kMaxExecutableRanges> executableRanges{};\n    size_t executableRangeCount = 0;\n};\n'''
new_struct = '''struct LibraryInfo {\n    uintptr_t base = 0;\n    std::string path;\n    const ElfW(Phdr) *programHeaders = nullptr;\n    ElfW(Half) programHeaderCount = 0;\n    std::array<ExecutableRange, kMaxExecutableRanges> executableRanges{};\n    size_t executableRangeCount = 0;\n    std::array<ExecutableRange, kMaxLoadRanges> loadRanges{};\n    size_t loadRangeCount = 0;\n};\n'''
assert text.count(old_struct) == 1
text = text.replace(old_struct, new_struct, 1)

old_callback = '''    result->base = static_cast<uintptr_t>(info->dlpi_addr);\n    result->path = path;\n    result->executableRangeCount = 0;\n\n    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {\n        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];\n        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0) continue;\n        if (result->executableRangeCount >= result->executableRanges.size()) break;\n        auto &range = result->executableRanges[result->executableRangeCount++];\n        range.start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);\n        range.size = static_cast<size_t>(phdr.p_memsz);\n    }\n'''
new_callback = '''    result->base = static_cast<uintptr_t>(info->dlpi_addr);\n    result->path = path;\n    result->programHeaders = info->dlpi_phdr;\n    result->programHeaderCount = info->dlpi_phnum;\n    result->executableRangeCount = 0;\n    result->loadRangeCount = 0;\n\n    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {\n        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];\n        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) continue;\n        if (result->loadRangeCount < result->loadRanges.size()) {\n            auto &load = result->loadRanges[result->loadRangeCount++];\n            load.start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);\n            load.size = static_cast<size_t>(phdr.p_memsz);\n        }\n        if ((phdr.p_flags & PF_X) == 0\n                || result->executableRangeCount >= result->executableRanges.size()) continue;\n        auto &range = result->executableRanges[result->executableRangeCount++];\n        range.start = result->base + static_cast<uintptr_t>(phdr.p_vaddr);\n        range.size = static_cast<size_t>(phdr.p_memsz);\n    }\n'''
assert text.count(old_callback) == 1
text = text.replace(old_callback, new_callback, 1)

start = text.index('bool preadExactMain(')
end = text.index('\nbool readProbeHead(', start)
new_resolver = r'''bool rangeContains(const ExecutableRange &range, uintptr_t address, size_t size) {
    if (range.start == 0 || range.size == 0 || address == 0 || size == 0) return false;
    if (address < range.start) return false;
    const uintptr_t rangeEnd = range.start + range.size;
    const uintptr_t addressEnd = address + size;
    if (rangeEnd < range.start || addressEnd < address) return false;
    return addressEnd <= rangeEnd;
}

bool libraryContainsRange(const LibraryInfo &library, uintptr_t address, size_t size) {
    for (size_t i = 0; i < library.loadRangeCount; ++i) {
        if (rangeContains(library.loadRanges[i], address, size)) return true;
    }
    return false;
}

uintptr_t resolveLoadedElfPointer(const LibraryInfo &library, ElfW(Addr) value, size_t size) {
    if (value == 0 || size == 0) return 0;
    if (value <= UINTPTR_MAX - library.base) {
        const uintptr_t relative = library.base + static_cast<uintptr_t>(value);
        if (libraryContainsRange(library, relative, size)) return relative;
    }
    const uintptr_t absolute = static_cast<uintptr_t>(value);
    if (libraryContainsRange(library, absolute, size)) return absolute;
    return 0;
}

struct ImportedFunctionResolution {
    uintptr_t slot = 0;
    void *target = nullptr;
    size_t matches = 0;
    const char *relocationKind = nullptr;
};

bool dynStringEquals(const LibraryInfo &library, uintptr_t strtab, size_t strsz,
                     size_t offset, const char *expected) {
    if (expected == nullptr || strtab == 0 || offset >= strsz) return false;
    const size_t remaining = strsz - offset;
    const uintptr_t address = strtab + offset;
    if (address < strtab || !libraryContainsRange(library, address, remaining)) return false;
    const char *candidate = reinterpret_cast<const char *>(address);
    const size_t length = strnlen(candidate, remaining);
    return length < remaining && std::strlen(expected) == length
            && std::memcmp(candidate, expected, length) == 0;
}

ImportedFunctionResolution resolveImportedFunction(const LibraryInfo &library,
                                                   const char *symbolName) {
    ImportedFunctionResolution result{};
    if (library.base == 0 || library.programHeaders == nullptr
            || library.programHeaderCount == 0 || symbolName == nullptr) return result;

    const ElfW(Phdr) *dynamicHeader = nullptr;
    for (ElfW(Half) i = 0; i < library.programHeaderCount; ++i) {
        const ElfW(Phdr) &phdr = library.programHeaders[i];
        if (phdr.p_type == PT_DYNAMIC && phdr.p_memsz >= sizeof(ElfW(Dyn))) {
            dynamicHeader = &phdr;
            break;
        }
    }
    if (dynamicHeader == nullptr) return result;

    const uintptr_t dynamicAddress = resolveLoadedElfPointer(
            library, dynamicHeader->p_vaddr, static_cast<size_t>(dynamicHeader->p_memsz));
    if (dynamicAddress == 0) return result;
    const auto *dynamic = reinterpret_cast<const ElfW(Dyn) *>(dynamicAddress);
    const size_t dynamicCount = static_cast<size_t>(dynamicHeader->p_memsz / sizeof(ElfW(Dyn)));

    ElfW(Addr) symtabValue = 0;
    ElfW(Addr) strtabValue = 0;
    ElfW(Addr) jmprelValue = 0;
    ElfW(Addr) relaValue = 0;
    size_t strsz = 0;
    size_t pltrelsz = 0;
    size_t relasz = 0;
    size_t syment = sizeof(ElfW(Sym));
    size_t relaent = sizeof(ElfW(Rela));
    ElfW(Sxword) pltrelType = DT_RELA;

    for (size_t i = 0; i < dynamicCount; ++i) {
        const ElfW(Dyn) &entry = dynamic[i];
        if (entry.d_tag == DT_NULL) break;
        switch (entry.d_tag) {
            case DT_SYMTAB: symtabValue = entry.d_un.d_ptr; break;
            case DT_STRTAB: strtabValue = entry.d_un.d_ptr; break;
            case DT_STRSZ: strsz = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_SYMENT: syment = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_JMPREL: jmprelValue = entry.d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_PLTREL: pltrelType = entry.d_un.d_val; break;
            case DT_RELA: relaValue = entry.d_un.d_ptr; break;
            case DT_RELASZ: relasz = static_cast<size_t>(entry.d_un.d_val); break;
            case DT_RELAENT: relaent = static_cast<size_t>(entry.d_un.d_val); break;
            default: break;
        }
    }

    if (symtabValue == 0 || strtabValue == 0 || strsz == 0
            || syment != sizeof(ElfW(Sym)) || relaent != sizeof(ElfW(Rela))) return result;

    const uintptr_t symtab = resolveLoadedElfPointer(library, symtabValue, sizeof(ElfW(Sym)));
    const uintptr_t strtab = resolveLoadedElfPointer(library, strtabValue, strsz);
    if (symtab == 0 || strtab == 0) return result;

    auto scanRela = [&](uintptr_t tableAddress, size_t tableSize, const char *kind) {
        if (tableAddress == 0 || tableSize < sizeof(ElfW(Rela))
                || tableSize % sizeof(ElfW(Rela)) != 0) return;
        if (!libraryContainsRange(library, tableAddress, tableSize)) return;
        const auto *relocations = reinterpret_cast<const ElfW(Rela) *>(tableAddress);
        const size_t count = tableSize / sizeof(ElfW(Rela));
        for (size_t i = 0; i < count; ++i) {
            const ElfW(Rela) &relocation = relocations[i];
            const uint32_t type = ELF64_R_TYPE(relocation.r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
            const size_t symbolIndex = static_cast<size_t>(ELF64_R_SYM(relocation.r_info));
            if (symbolIndex > (SIZE_MAX / sizeof(ElfW(Sym)))) continue;
            const uintptr_t symbolAddress = symtab + symbolIndex * sizeof(ElfW(Sym));
            if (symbolAddress < symtab
                    || !libraryContainsRange(library, symbolAddress, sizeof(ElfW(Sym)))) continue;
            const auto &symbol = *reinterpret_cast<const ElfW(Sym) *>(symbolAddress);
            if (!dynStringEquals(library, strtab, strsz,
                                 static_cast<size_t>(symbol.st_name), symbolName)) continue;
            const uintptr_t slot = resolveLoadedElfPointer(
                    library, relocation.r_offset, sizeof(void *));
            if (slot == 0) continue;
            void *target = nullptr;
            std::memcpy(&target, reinterpret_cast<const void *>(slot), sizeof(target));
            if (reinterpret_cast<uintptr_t>(target) < 0x10000u) continue;
            if (result.matches == 0) {
                result.slot = slot;
                result.target = target;
                result.matches = 1;
                result.relocationKind = kind;
            } else if (result.target != target) {
                result.matches = 2;
                result.relocationKind = "ambiguous";
                return;
            }
        }
    };

    if (jmprelValue != 0 && pltrelsz != 0 && pltrelType == DT_RELA) {
        const uintptr_t jmprel = resolveLoadedElfPointer(library, jmprelValue, pltrelsz);
        scanRela(jmprel, pltrelsz, "DT_JMPREL");
    }
    if (result.matches <= 1 && relaValue != 0 && relasz != 0) {
        const uintptr_t rela = resolveLoadedElfPointer(library, relaValue, relasz);
        scanRela(rela, relasz, "DT_RELA");
    }
    return result;
}
'''
text = text[:start] + new_resolver + text[end:]

old_log = '''    if (resolution.matches != 1 || resolution.target == nullptr) {\n        logLine(ANDROID_LOG_WARN,\n                "HAPTIC_V2 feature import unresolved source=%s symbol=%s matches=%zu launcher=%s",\n                source == nullptr ? "unknown" : source, kHapticSymbol,\n                resolution.matches, library.path.c_str());\n        return false;\n    }\n    logLine(ANDROID_LOG_INFO,\n            "HAPTIC_V2 feature import resolved source=%s symbol=%s got=%p target=%p launcher=%s",\n            source == nullptr ? "unknown" : source, kHapticSymbol,\n            reinterpret_cast<void *>(resolution.slot), resolution.target, library.path.c_str());\n'''
new_log = '''    if (resolution.matches != 1 || resolution.target == nullptr) {\n        logLine(ANDROID_LOG_WARN,\n                "HAPTIC_V2 loaded-elf import unresolved source=%s symbol=%s matches=%zu launcher=%s",\n                source == nullptr ? "unknown" : source, kHapticSymbol,\n                resolution.matches, library.path.c_str());\n        return false;\n    }\n    logLine(ANDROID_LOG_INFO,\n            "HAPTIC_V2 loaded-elf import resolved source=%s symbol=%s reloc=%s got=%p target=%p launcher=%s",\n            source == nullptr ? "unknown" : source, kHapticSymbol,\n            resolution.relocationKind == nullptr ? "unknown" : resolution.relocationKind,\n            reinterpret_cast<void *>(resolution.slot), resolution.target, library.path.c_str());\n'''
assert text.count(old_log) == 1
text = text.replace(old_log, new_log, 1)

old_policy = 'HAPTIC_V2 enabled policy=launcher-import-feature-primary provider-loader-fallback no-dlopen no-hook-mutex ready=ext-fallback commit=ext-0'
new_policy = 'HAPTIC_V2 enabled policy=loaded-elf-dynamic-feature-primary provider-loader-fallback no-apk-open no-haptic-dlopen no-hook-mutex ready=ext-fallback commit=ext-0'
assert text.count(old_policy) == 1
text = text.replace(old_policy, new_policy, 1)
main.write_text(text)

gradle = Path('app/build.gradle.kts')
g = gradle.read_text()
assert 'versionCode = 38' in g
assert 'versionName = "0.8.0-dev.6"' in g
g = g.replace('versionCode = 38', 'versionCode = 39', 1)
g = g.replace('versionName = "0.8.0-dev.6"', 'versionName = "0.8.0-dev.7"', 1)
gradle.write_text(g)
