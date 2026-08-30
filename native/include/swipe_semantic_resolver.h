#pragma once

#include <cstddef>
#include <cstdint>

namespace swipe_semantic {

enum class FrameShape : std::uint8_t {
    Unknown = 0,
    Legacy = 1,
    Modern = 2,
    Compact = 3,
};

struct Resolution {
    std::uintptr_t target = 0;
    std::size_t candidate_count = 0;
    FrameShape shape = FrameShape::Unknown;
};

Resolution Resolve(std::uintptr_t library_base);
bool ValidateTarget(std::uintptr_t library_base, std::uintptr_t target);
const char* FrameShapeName(FrameShape shape);

}  // namespace swipe_semantic
