#pragma once
#include <cstdint>

namespace cs::jitter {

struct Sample {
    bool valid = false;
    float x = 0.0f;  // pixels, as stored in the engine table (within +-0.5)
    float y = 0.0f;
    uint32_t count = 0;
};

// The engine's CMJ jitter for a frame: table[frame_index % size]. The table is a
// std::vector<float2> whose begin/end pointers are globals at RVA 0x2565280 / 0x2565288.
Sample for_frame(uint32_t frame_index);

}  // namespace cs::jitter
