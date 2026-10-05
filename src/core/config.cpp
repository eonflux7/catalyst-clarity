#include "core/config.h"

#include <string>

namespace cs {

static Config g_config;
static std::wstring g_path;

Config& config() { return g_config; }

void config_load(const std::wstring& path) {
    g_path = path;
    const wchar_t* f = g_path.c_str();
    g_config.aa_mode = GetPrivateProfileIntW(L"general", L"mode", g_config.aa_mode, f);
    if (g_config.aa_mode < 0 || g_config.aa_mode > 2)
        g_config.aa_mode = 2;
    g_config.toggle_key = GetPrivateProfileIntW(L"ui", L"toggle_key", g_config.toggle_key, f);
    g_config.debug_key = GetPrivateProfileIntW(L"ui", L"debug_key", g_config.debug_key, f);
    g_config.show_on_start = GetPrivateProfileIntW(L"ui", L"show_on_start", g_config.show_on_start, f) != 0;
    g_config.show_fps = GetPrivateProfileIntW(L"ui", L"show_fps", g_config.show_fps, f) != 0;
    g_config.dlss_preset = GetPrivateProfileIntW(L"dlss", L"preset", g_config.dlss_preset, f);
    g_config.dlss_quality_mode = GetPrivateProfileIntW(L"dlss", L"quality_mode", g_config.dlss_quality_mode, f);
    g_config.dlss_sharpness = GetPrivateProfileIntW(L"dlss", L"sharpness", g_config.dlss_sharpness, f);
    g_config.d3d_debug = GetPrivateProfileIntW(L"debug", L"d3d_debug", g_config.d3d_debug, f) != 0;
    config_save();
}

void config_save() {
    if (g_path.empty())
        return;
    const wchar_t* f = g_path.c_str();
    WritePrivateProfileStringW(L"general", L"mode", std::to_wstring(g_config.aa_mode).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"toggle_key", std::to_wstring(g_config.toggle_key).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"debug_key", std::to_wstring(g_config.debug_key).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"show_on_start", g_config.show_on_start ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"ui", L"show_fps", g_config.show_fps ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"preset", std::to_wstring(g_config.dlss_preset).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"quality_mode", std::to_wstring(g_config.dlss_quality_mode).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"sharpness", std::to_wstring(g_config.dlss_sharpness).c_str(), f);
    WritePrivateProfileStringW(L"debug", L"d3d_debug", g_config.d3d_debug ? L"1" : L"0", f);
}

}  // namespace cs
