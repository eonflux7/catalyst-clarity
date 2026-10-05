#pragma once
#include <cstdint>
#include <string>

namespace cs {

// The build the mod was made and tested on: the Steam release with PE timestamp 1466098615. Other builds
// are tried anyway; game-code hooks and fixed addresses are used only if the TAA code matches.
constexpr uint32_t kTestedBuildTimestamp = 1466098615;

struct State {
    uintptr_t game_base = 0;
    uint32_t game_timestamp = 0;
    bool tested_build = false;  // PE timestamp is the tested build (informational)
    bool code_matched = false;  // TAA entry bytes matched at the expected RVA: addresses are valid
    bool hooks_installed = false;
    std::wstring mod_dir;  // folder of our DLL (game folder), with trailing separator
};

State& state();

}  // namespace cs
