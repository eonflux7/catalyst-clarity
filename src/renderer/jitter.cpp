#include "renderer/jitter.h"

#include <windows.h>

#include "core/state.h"

namespace cs::jitter {

constexpr uintptr_t kTableBeginRva = 0x2565280;
constexpr uintptr_t kTableEndRva = 0x2565288;

Sample for_frame(uint32_t frame_index) {
    Sample s;
    if (!state().code_matched)
        return s;
    __try {
        auto begin = *reinterpret_cast<const float* const*>(state().game_base + kTableBeginRva);
        auto end = *reinterpret_cast<const float* const*>(state().game_base + kTableEndRva);
        if (!begin || end <= begin)
            return s;
        s.count = static_cast<uint32_t>((end - begin) / 2);
        const float* entry = begin + 2 * (frame_index % s.count);
        s.x = entry[0];
        s.y = entry[1];
        s.valid = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.valid = false;
    }
    return s;
}

}  // namespace cs::jitter
