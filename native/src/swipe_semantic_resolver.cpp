#include "swipe_semantic_resolver.h"

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace swipe_semantic {
namespace {

constexpr std::size_t kMaxLoadSegments = 16;
constexpr std::uintptr_t kMaxImageSpan = 0x4000000u;
constexpr std::uintptr_t kGraphSearchStart = 0x10u;
constexpr std::uintptr_t kGraphSearchEnd = 0x240u;
constexpr std::size_t kMotionGraphLength = 5;

constexpr char kMotionActionMasked[] = "input_MotionEvent_getActionMasked";
constexpr char kMotionActionIndex[] = "input_MotionEvent_getActionIndex";
constexpr char kMotionRawX[] = "input_MotionEvent_getRawX";
constexpr char kMotionRawY[] = "input_MotionEvent_getRawY";

struct LoadSegment {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    std::uint32_t flags = 0;
};

struct ElfView {
    const std::uint8_t* base = nullptr;
    LoadSegment loads[kMaxLoadSegments]{};
    std::size_t load_count = 0;
    std::uintptr_t image_span = 0;
    std::uintptr_t string_table = 0;
    std::size_t string_table_size = 0;
    std::uintptr_t symbol_table = 0;
    std::uintptr_t jump_relocations = 0;
    std::size_t jump_relocations_size = 0;
};

struct MotionImports {
    std::uintptr_t action_masked = 0;
    std::uintptr_t action_index = 0;
    std::uintptr_t raw_x = 0;
    std::uintptr_t raw_y = 0;
};

bool AddOverflows(std::uintptr_t left, std::uintptr_t right) {
    return right > UINTPTR_MAX - left;
}

bool Contains(const ElfView& view, std::uintptr_t offset, std::size_t size,
              std::uint32_t required_flags) {
    if (size == 0 || AddOverflows(offset, size)) return false;
    const std::uintptr_t end = offset + size;
    for (std::size_t i = 0; i < view.load_count; ++i) {
        const LoadSegment& load = view.loads[i];
        if (offset >= load.start && end <= load.end &&
            (load.flags & required_flags) == required_flags) {
            return true;
        }
    }
    return false;
}

bool NormalizeDynamicPointer(const ElfView& view, Elf64_Addr value,
                             std::uintptr_t* offset) {
    if (offset == nullptr) return false;
    const std::uintptr_t raw = static_cast<std::uintptr_t>(value);
    const std::uintptr_t base_address =
            reinterpret_cast<std::uintptr_t>(view.base);
    if (raw >= base_address && raw - base_address < view.image_span) {
        *offset = raw - base_address;
        return true;
    }
    if (raw < view.image_span) {
        *offset = raw;
        return true;
    }
    return false;
}

bool ParseElf(std::uintptr_t library_base, ElfView* output) {
    if (library_base == 0 || output == nullptr) return false;
    const auto* base = reinterpret_cast<const std::uint8_t*>(library_base);

    Elf64_Ehdr header{};
    std::memcpy(&header, base, sizeof(header));
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_type != ET_DYN || header.e_machine != EM_AARCH64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
        header.e_phnum > 64 || header.e_phoff > 0x1000u ||
        AddOverflows(header.e_phoff,
                     static_cast<std::uintptr_t>(header.e_phnum) *
                             sizeof(Elf64_Phdr))) {
        return false;
    }

    ElfView view{};
    view.base = base;
    const auto* phdrs = reinterpret_cast<const Elf64_Phdr*>(base + header.e_phoff);
    std::uintptr_t dynamic_offset = 0;
    std::size_t dynamic_size = 0;

    for (std::size_t i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr phdr{};
        std::memcpy(&phdr, phdrs + i, sizeof(phdr));
        if (phdr.p_type == PT_LOAD) {
            if (phdr.p_memsz == 0 || view.load_count >= kMaxLoadSegments ||
                AddOverflows(phdr.p_vaddr, phdr.p_memsz)) {
                return false;
            }
            const std::uintptr_t start = phdr.p_vaddr;
            const std::uintptr_t end = start + phdr.p_memsz;
            if (end > kMaxImageSpan) return false;
            view.loads[view.load_count++] = {start, end, phdr.p_flags};
            if (end > view.image_span) view.image_span = end;
        } else if (phdr.p_type == PT_DYNAMIC) {
            if (phdr.p_memsz == 0 || phdr.p_memsz > SIZE_MAX) return false;
            dynamic_offset = phdr.p_vaddr;
            dynamic_size = static_cast<std::size_t>(phdr.p_memsz);
        }
    }

    if (view.load_count == 0 || view.image_span == 0 ||
        dynamic_size < sizeof(Elf64_Dyn) ||
        !Contains(view, dynamic_offset, dynamic_size, PF_R)) {
        return false;
    }

    Elf64_Addr strtab = 0;
    Elf64_Addr symtab = 0;
    Elf64_Addr jmprel = 0;
    std::size_t strsz = 0;
    std::size_t syment = 0;
    std::size_t pltrelsz = 0;
    std::size_t relaent = sizeof(Elf64_Rela);
    Elf64_Sxword pltrel = 0;
    bool terminated = false;

    const std::size_t dynamic_count = dynamic_size / sizeof(Elf64_Dyn);
    for (std::size_t i = 0; i < dynamic_count; ++i) {
        Elf64_Dyn dyn{};
        std::memcpy(&dyn, base + dynamic_offset + i * sizeof(Elf64_Dyn),
                    sizeof(dyn));
        if (dyn.d_tag == DT_NULL) {
            terminated = true;
            break;
        }
        switch (dyn.d_tag) {
            case DT_STRTAB: strtab = dyn.d_un.d_ptr; break;
            case DT_STRSZ: strsz = dyn.d_un.d_val; break;
            case DT_SYMTAB: symtab = dyn.d_un.d_ptr; break;
            case DT_SYMENT: syment = dyn.d_un.d_val; break;
            case DT_JMPREL: jmprel = dyn.d_un.d_ptr; break;
            case DT_PLTRELSZ: pltrelsz = dyn.d_un.d_val; break;
            case DT_PLTREL: pltrel = dyn.d_un.d_val; break;
            case DT_RELAENT: relaent = dyn.d_un.d_val; break;
            default: break;
        }
    }

    if (!terminated || strsz == 0 || syment != sizeof(Elf64_Sym) ||
        pltrel != DT_RELA || relaent != sizeof(Elf64_Rela) ||
        pltrelsz == 0 || pltrelsz % sizeof(Elf64_Rela) != 0 ||
        !NormalizeDynamicPointer(view, strtab, &view.string_table) ||
        !NormalizeDynamicPointer(view, symtab, &view.symbol_table) ||
        !NormalizeDynamicPointer(view, jmprel, &view.jump_relocations)) {
        return false;
    }

    view.string_table_size = strsz;
    view.jump_relocations_size = pltrelsz;
    if (!Contains(view, view.string_table, view.string_table_size, PF_R) ||
        !Contains(view, view.symbol_table, sizeof(Elf64_Sym), PF_R) ||
        !Contains(view, view.jump_relocations, view.jump_relocations_size, PF_R)) {
        return false;
    }

    *output = view;
    return true;
}

bool BoundedStringEquals(const char* value, std::size_t available,
                         const char* expected) {
    if (value == nullptr || expected == nullptr) return false;
    std::size_t i = 0;
    while (expected[i] != '\0') {
        if (i >= available || value[i] != expected[i]) return false;
        ++i;
    }
    return i < available && value[i] == '\0';
}

bool FindImportGot(const ElfView& view, const char* expected,
                   std::uintptr_t* result) {
    if (result == nullptr) return false;
    std::uintptr_t matched = 0;
    std::size_t matches = 0;
    const std::size_t count = view.jump_relocations_size / sizeof(Elf64_Rela);
    for (std::size_t i = 0; i < count; ++i) {
        Elf64_Rela rela{};
        std::memcpy(&rela,
                    view.base + view.jump_relocations + i * sizeof(Elf64_Rela),
                    sizeof(rela));
        const std::size_t symbol_index = ELF64_R_SYM(rela.r_info);
        if (symbol_index > (SIZE_MAX - view.symbol_table) / sizeof(Elf64_Sym)) {
            return false;
        }
        const std::uintptr_t symbol_offset =
                view.symbol_table + symbol_index * sizeof(Elf64_Sym);
        if (!Contains(view, symbol_offset, sizeof(Elf64_Sym), PF_R)) return false;

        Elf64_Sym symbol{};
        std::memcpy(&symbol, view.base + symbol_offset, sizeof(symbol));
        if (symbol.st_name >= view.string_table_size) return false;
        const char* name = reinterpret_cast<const char*>(
                view.base + view.string_table + symbol.st_name);
        if (!BoundedStringEquals(name, view.string_table_size - symbol.st_name,
                                 expected)) {
            continue;
        }

        std::uintptr_t got = 0;
        if (!NormalizeDynamicPointer(view, rela.r_offset, &got) ||
            !Contains(view, got, sizeof(std::uintptr_t), PF_R)) {
            return false;
        }
        matched = got;
        ++matches;
    }
    if (matches != 1) return false;
    *result = matched;
    return true;
}

bool ResolveMotionImports(const ElfView& view, MotionImports* imports) {
    return imports != nullptr &&
           FindImportGot(view, kMotionActionMasked, &imports->action_masked) &&
           FindImportGot(view, kMotionActionIndex, &imports->action_index) &&
           FindImportGot(view, kMotionRawX, &imports->raw_x) &&
           FindImportGot(view, kMotionRawY, &imports->raw_y);
}

bool ReadInstruction(const ElfView& view, std::uintptr_t offset,
                     std::uint32_t* instruction) {
    if (instruction == nullptr ||
        !Contains(view, offset, sizeof(*instruction), PF_R | PF_X)) {
        return false;
    }
    std::memcpy(instruction, view.base + offset, sizeof(*instruction));
    return true;
}

bool DecodeAdrp(std::uint32_t instruction, std::uintptr_t pc,
                std::uint32_t reg, std::uintptr_t* target_page) {
    if (target_page == nullptr || reg > 31u ||
        (instruction & 0x9f00001fu) != (0x90000000u | reg)) {
        return false;
    }
    std::int64_t immediate = static_cast<std::int64_t>(
            ((instruction >> 29u) & 0x3u) |
            (((instruction >> 5u) & 0x7ffffu) << 2u));
    if ((immediate & (std::int64_t{1} << 20u)) != 0) {
        immediate -= std::int64_t{1} << 21u;
    }
    const std::int64_t page = static_cast<std::int64_t>(pc & ~std::uintptr_t{0xfffu});
    const std::int64_t target = page + immediate * std::int64_t{4096};
    if (target < 0 || static_cast<std::uint64_t>(target) > UINTPTR_MAX) return false;
    *target_page = static_cast<std::uintptr_t>(target);
    return true;
}

bool DecodeAddImmediate(std::uint32_t instruction, std::uint32_t destination,
                        std::uint32_t source, std::uintptr_t* immediate) {
    if (immediate == nullptr ||
        (instruction & 0xffc003ffu) !=
                (0x91000000u | (source << 5u) | destination)) {
        return false;
    }
    *immediate = (instruction >> 10u) & 0xfffu;
    return true;
}

bool DecodeLdr64Immediate(std::uint32_t instruction, std::uint32_t destination,
                          std::uint32_t source, std::uintptr_t* immediate) {
    if (immediate == nullptr ||
        (instruction & 0xffc003ffu) !=
                (0xf9400000u | (source << 5u) | destination)) {
        return false;
    }
    *immediate = ((instruction >> 10u) & 0xfffu) * sizeof(std::uintptr_t);
    return true;
}

bool DecodeBlTarget(const ElfView& view, std::uintptr_t instruction_offset,
                    std::uintptr_t* target) {
    std::uint32_t instruction = 0;
    if (target == nullptr || !ReadInstruction(view, instruction_offset, &instruction) ||
        (instruction & 0xfc000000u) != 0x94000000u) {
        return false;
    }
    std::int64_t immediate = instruction & 0x03ffffffu;
    if ((immediate & (std::int64_t{1} << 25u)) != 0) {
        immediate -= std::int64_t{1} << 26u;
    }
    const std::int64_t destination =
            static_cast<std::int64_t>(instruction_offset) + immediate * 4;
    if (destination < 0 || static_cast<std::uint64_t>(destination) > UINTPTR_MAX) {
        return false;
    }
    const std::uintptr_t decoded = static_cast<std::uintptr_t>(destination);
    if (!Contains(view, decoded, 16u, PF_R | PF_X)) return false;
    *target = decoded;
    return true;
}

bool DecodePltGotAt(const ElfView& view, std::uintptr_t plt_offset,
                    std::uintptr_t* got_offset) {
    std::uint32_t adrp = 0;
    std::uint32_t ldr = 0;
    std::uint32_t add = 0;
    std::uint32_t branch = 0;
    std::uintptr_t page = 0;
    std::uintptr_t load_immediate = 0;
    std::uintptr_t add_immediate = 0;

    return got_offset != nullptr &&
           ReadInstruction(view, plt_offset, &adrp) &&
           ReadInstruction(view, plt_offset + 4u, &ldr) &&
           ReadInstruction(view, plt_offset + 8u, &add) &&
           ReadInstruction(view, plt_offset + 12u, &branch) &&
           DecodeAdrp(adrp, plt_offset, 16u, &page) &&
           DecodeLdr64Immediate(ldr, 17u, 16u, &load_immediate) &&
           DecodeAddImmediate(add, 16u, 16u, &add_immediate) &&
           load_immediate == add_immediate && branch == 0xd61f0220u &&
           !AddOverflows(page, load_immediate) &&
           ((*got_offset = page + load_immediate), true);
}

bool DecodePltGot(const ElfView& view, std::uintptr_t plt_offset,
                  std::uintptr_t* got_offset) {
    if (DecodePltGotAt(view, plt_offset, got_offset)) return true;

    std::uint32_t first = 0;
    if (!ReadInstruction(view, plt_offset, &first)) return false;
    // Some Android linkers prepend BTI c to PLT entries.
    if (first == 0xd503245fu) {
        return DecodePltGotAt(view, plt_offset + 4u, got_offset);
    }
    return false;
}

bool CallTargetsImport(const ElfView& view, std::uintptr_t call_offset,
                       std::uintptr_t expected_got) {
    std::uintptr_t plt = 0;
    std::uintptr_t got = 0;
    return DecodeBlTarget(view, call_offset, &plt) &&
           DecodePltGot(view, plt, &got) && got == expected_got;
}

bool Matches(std::uint32_t instruction, std::uint32_t expected,
             std::uint32_t mask) {
    return (instruction & mask) == expected;
}

FrameShape MatchFrameShape(const ElfView& view, std::uintptr_t offset) {
    std::uint32_t words[4]{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!ReadInstruction(view, offset + i * 4u, &words[i])) {
            return FrameShape::Unknown;
        }
    }

    const bool sub_sp = Matches(words[0], 0xd10003ffu, 0xffc003ffu);
    const bool fp_lr_at_3 = Matches(words[3], 0xa9007bfdu, 0xffc07fffu);
    if (sub_sp && fp_lr_at_3 &&
        Matches(words[1], 0xfd0003eau, 0xffc003ffu) &&
        Matches(words[2], 0x6d0023e9u, 0xffc07fffu)) {
        return FrameShape::Legacy;
    }
    if (sub_sp && fp_lr_at_3 &&
        Matches(words[1], 0x6d002bebu, 0xffc07fffu) &&
        Matches(words[2], 0x6d0023e9u, 0xffc07fffu)) {
        return FrameShape::Modern;
    }

    if (Matches(words[0], 0x6d802bebu, 0xffc07fffu) &&
        Matches(words[1], 0x6d0023e9u, 0xffc07fffu) &&
        Matches(words[2], 0xa9007bfdu, 0xffc07fffu)) {
        return FrameShape::Compact;
    }
    return FrameShape::Unknown;
}

bool HasMotionGraph(const ElfView& view, const MotionImports& imports,
                    std::uintptr_t offset) {
    const std::uintptr_t sequence[] = {
            imports.action_masked,
            imports.action_index,
            imports.raw_x,
            imports.action_index,
            imports.raw_y,
    };
    std::size_t next = 0;

    for (std::uintptr_t cursor = kGraphSearchStart;
         cursor <= kGraphSearchEnd && next < kMotionGraphLength;
         cursor += 4u) {
        std::uint32_t instruction = 0;
        if (!ReadInstruction(view, offset + cursor, &instruction)) break;
        if ((instruction & 0xfc000000u) != 0x94000000u) continue;
        if (CallTargetsImport(view, offset + cursor, sequence[next])) {
            ++next;
        }
    }
    return next == kMotionGraphLength;
}

bool IsCandidate(const ElfView& view, const MotionImports& imports,
                 std::uintptr_t offset, FrameShape* shape) {
    const FrameShape matched = MatchFrameShape(view, offset);
    if (matched == FrameShape::Unknown || !HasMotionGraph(view, imports, offset)) {
        return false;
    }
    if (shape != nullptr) *shape = matched;
    return true;
}

}  // namespace

Resolution Resolve(std::uintptr_t library_base) {
    ElfView view{};
    MotionImports imports{};
    if (!ParseElf(library_base, &view) || !ResolveMotionImports(view, &imports)) {
        return {};
    }

    Resolution result{};
    for (std::size_t segment_index = 0; segment_index < view.load_count;
         ++segment_index) {
        const LoadSegment& load = view.loads[segment_index];
        if ((load.flags & (PF_R | PF_X)) != (PF_R | PF_X) ||
            load.end <= load.start || load.end - load.start < 32u) {
            continue;
        }
        const std::uintptr_t start = (load.start + 3u) & ~std::uintptr_t{3u};
        const std::uintptr_t last = load.end - 16u;
        for (std::uintptr_t offset = start; offset <= last; offset += 4u) {
            FrameShape shape = FrameShape::Unknown;
            if (!IsCandidate(view, imports, offset, &shape)) continue;
            result.target = library_base + offset;
            result.shape = shape;
            ++result.candidate_count;
            if (result.candidate_count > 1) {
                result.target = 0;
                result.shape = FrameShape::Unknown;
                return result;
            }
        }
    }
    return result;
}

bool ValidateTarget(std::uintptr_t library_base, std::uintptr_t target) {
    if (library_base == 0 || target < library_base) return false;
    ElfView view{};
    MotionImports imports{};
    if (!ParseElf(library_base, &view) || !ResolveMotionImports(view, &imports)) {
        return false;
    }
    const std::uintptr_t offset = target - library_base;
    return IsCandidate(view, imports, offset, nullptr);
}

const char* FrameShapeName(FrameShape shape) {
    switch (shape) {
        case FrameShape::Legacy: return "legacy";
        case FrameShape::Modern: return "modern";
        case FrameShape::Compact: return "compact";
        case FrameShape::Unknown: break;
    }
    return "unknown";
}

}  // namespace swipe_semantic
