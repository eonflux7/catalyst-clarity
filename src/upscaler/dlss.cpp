#include "upscaler/dlss.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <iterator>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include "core/config.h"
#include "core/log.h"
#include "core/state.h"

namespace cs::dlss {

// Any stable GUID identifies the project to NGX when there is no NVIDIA-issued app id.
constexpr const char* kProjectId = "5d0e6b9c-7a43-4c1e-9b8f-2f6f0c3a91d4";

static Settings g_settings;
static Status g_status;
static NVSDK_NGX_Parameter* g_params;
static NVSDK_NGX_Handle* g_feature;
static Settings g_feature_settings;
static int g_feature_preset;
static bool g_recreate;

Settings& settings() { return g_settings; }
const Status& status() { return g_status; }
void request_recreate() { g_recreate = true; }

static void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    // NGX lines end with a newline already.
    size_t n = strlen(message);
    while (n && (message[n - 1] == '\n' || message[n - 1] == '\r'))
        --n;
    logf("ngx: %.*s", (int)n, message);
}

// True when the module's version resource says OptiScaler; fills the file version. Reads the resource
// directly: GetFileVersionInfo would make the mod import version.dll.
static bool is_optiscaler(HMODULE module, char* version, size_t version_size) {
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION);
    HGLOBAL data = res ? LoadResource(module, res) : nullptr;
    const auto* blob = data ? static_cast<const BYTE*>(LockResource(data)) : nullptr;
    if (!blob)
        return false;
    DWORD size = SizeofResource(module, res);
    const wchar_t name[] = L"OptiScaler";
    const size_t name_bytes = sizeof(name) - sizeof(wchar_t);
    bool found = false;
    for (DWORD i = 0; i + name_bytes <= size && !found; i += 2)
        found = memcmp(blob + i, name, name_bytes) == 0;
    if (!found)
        return false;
    for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const auto* fixed = reinterpret_cast<const VS_FIXEDFILEINFO*>(blob + i);
        if (fixed->dwSignature == 0xFEEF04BD) {
            snprintf(version, version_size, "%u.%u.%u.%u", HIWORD(fixed->dwFileVersionMS),
                     LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
            break;
        }
    }
    return true;
}

static void detect_optiscaler() {
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < std::size(modules); ++i) {
        if (!is_optiscaler(modules[i], g_status.optiscaler_version, sizeof(g_status.optiscaler_version)))
            continue;
        char path[MAX_PATH] = {};
        GetModuleFileNameA(modules[i], path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        snprintf(g_status.optiscaler_module, sizeof(g_status.optiscaler_module), "%s", base ? base + 1 : path);
        logf("dlss: OptiScaler %s loaded as %s; NGX calls go through it", g_status.optiscaler_version, path);
        return;
    }
    logf("dlss: no OptiScaler in the process");
}

static bool init(ID3D11Device* device) {
    g_status.init_tried = true;
    detect_optiscaler();
    const std::wstring& dir = state().mod_dir;
    const wchar_t* paths[] = {dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = &ngx_log;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, CS_VERSION,
                                                              dir.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        g_status.init_result = r;
        logf("dlss: NGX init failed 0x%08x", r);
        return false;
    }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r)) {
        g_status.init_result = r;
        logf("dlss: GetCapabilityParameters failed 0x%08x", r);
        return false;
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    g_status.needs_driver = needs_driver != 0;
    g_status.available = available != 0;
    if (!g_status.available) {
        int init_result = 0;
        NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
        g_status.init_result = init_result;
    }
    logf("dlss: NGX ready, DLSS available %d, needs driver update %d, feature init result 0x%08x", available,
         needs_driver, g_status.init_result);
    return g_status.available;
}

// Nearest standard mode for the render/output ratio (DLSS picks its model preset by mode).
static NVSDK_NGX_PerfQuality_Value quality_for(UINT in_width, UINT out_width) {
    double r = double(in_width) / double(out_width);
    if (r >= 0.99)
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (r >= 0.62)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;  // 0.667
    if (r >= 0.54)
        return NVSDK_NGX_PerfQuality_Value_Balanced;    // 0.58
    if (r >= 0.42)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;     // 0.5
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;  // 0.333
}

bool ensure(ID3D11DeviceContext* ctx, UINT in_width, UINT in_height, UINT out_width, UINT out_height,
            bool* recreated) {
    *recreated = false;
    if (!g_status.init_tried) {
        ID3D11Device* device = nullptr;
        ctx->GetDevice(&device);
        init(device);
        device->Release();
    }
    if (!g_status.available)
        return false;

    bool same = g_feature && !g_recreate && in_width == g_status.in_width && in_height == g_status.in_height &&
                out_width == g_status.out_width && out_height == g_status.out_height &&
                g_settings.auto_exposure == g_feature_settings.auto_exposure &&
                config().dlss_preset == g_feature_preset;
    if (same)
        return true;

    if (g_feature) {
        NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    g_recreate = false;
    g_feature_settings = g_settings;
    g_feature_preset = config().dlss_preset;
    // One preset for every mode: our render size is continuous, the mode is only the nearest match.
    for (const char* hint : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                             NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
        NVSDK_NGX_Parameter_SetUI(g_params, hint, static_cast<unsigned>(g_feature_preset));

    NVSDK_NGX_DLSS_Create_Params create{};
    create.Feature.InWidth = in_width;
    create.Feature.InHeight = in_height;
    create.Feature.InTargetWidth = out_width;
    create.Feature.InTargetHeight = out_height;
    create.Feature.InPerfQualityValue = quality_for(in_width, out_width);
    // Engine inputs (docs/taa.md): linear HDR colour, MVs at render res and jitter-free,
    // reversed-Z depth.
    create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  (g_settings.auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature, g_params, &create);
    g_status.create_result = r;
    g_status.feature_created = NVSDK_NGX_SUCCEED(r) && g_feature;
    g_status.in_width = in_width;
    g_status.in_height = in_height;
    g_status.out_width = out_width;
    g_status.out_height = out_height;
    logf("dlss: create %ux%u -> %ux%u flags 0x%x preset %d: 0x%08x", in_width, in_height, out_width, out_height,
         create.InFeatureCreateFlags, g_feature_preset, r);
    *recreated = g_status.feature_created;
    return g_status.feature_created;
}

// 1x1 R32F holding 1.0: the exposure texture while auto exposure is off.
static ID3D11Texture2D* unit_exposure(ID3D11DeviceContext* ctx) {
    static ID3D11Texture2D* tex;
    static bool tried;
    if (tried)
        return tex;
    tried = true;
    ID3D11Device* device = nullptr;
    ctx->GetDevice(&device);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 1;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const float one = 1.0f;
    D3D11_SUBRESOURCE_DATA data{&one, sizeof(one), 0};
    if (FAILED(device->CreateTexture2D(&d, &data, &tex)))
        tex = nullptr;
    device->Release();
    logf("dlss: exposure texture (1.0) %s", tex ? "ready" : "FAILED");
    return tex;
}

bool evaluate(ID3D11DeviceContext* ctx, const EvalInputs& in) {
    if (!g_feature)
        return false;
    NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
    eval.Feature.pInColor = in.colour;
    eval.Feature.pInOutput = in.output;
    eval.pInDepth = in.depth;
    eval.pInMotionVectors = in.motion;
    eval.InJitterOffsetX = in.jitter_x;
    eval.InJitterOffsetY = in.jitter_y;
    eval.InRenderSubrectDimensions = {in.render_width, in.render_height};
    eval.InReset = in.reset ? 1 : 0;
    eval.InMVScaleX = in.mv_scale_x;
    eval.InMVScaleY = in.mv_scale_y;
    eval.InPreExposure = 1.0f;
    eval.InExposureScale = 1.0f;
    if (!g_feature_settings.auto_exposure)
        eval.pInExposureTexture = unit_exposure(ctx);
    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature, g_params, &eval);
    if (r != g_status.last_eval_result)
        logf("dlss: evaluate -> 0x%08x", r);
    g_status.last_eval_result = r;
    ++g_status.evaluations;
    return NVSDK_NGX_SUCCEED(r);
}

}  // namespace cs::dlss
