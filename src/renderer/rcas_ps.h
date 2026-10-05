#pragma once

// Robust contrast-adaptive sharpening (AMD FidelityFX FSR 1 RCAS, MIT), on the upscaler's linear HDR output.
// RCAS expects [0, 1]: it runs on c / (1 + c) and maps back, so highlights keep their range. 5-tap cross;
// the per-channel lobe is limited so the result never leaves the neighbourhood's min/max (no ringing).
// The FSR noise-removal term is left out (the input is already temporally resolved).
namespace cs::dlss_pass {

constexpr const char* kRcasPs = R"(
cbuffer Rcas : register(b0) {
    float amount;   // 0 = off .. 1 = FSR's maximum (sharpness 0 stops)
    float3 pad;
};
Texture2D<float4> src : register(t0);

float3 fwd(float3 c) { c = max(c, 0); return c / (1 + c); }
float3 inv(float3 c) { return c / max(1 - c, 1e-4); }

float4 main(float4 pos : SV_Position) : SV_Target {
    uint w, h;
    src.GetDimensions(w, h);
    int2 p = int2(pos.xy);
    int2 hi = int2(w, h) - 1;
    float4 ce = src.Load(int3(p, 0));
    float3 e = fwd(ce.rgb);
    float3 b = fwd(src.Load(int3(clamp(p + int2(0, -1), 0, hi), 0)).rgb);
    float3 d = fwd(src.Load(int3(clamp(p + int2(-1, 0), 0, hi), 0)).rgb);
    float3 f = fwd(src.Load(int3(clamp(p + int2(1, 0), 0, hi), 0)).rgb);
    float3 g = fwd(src.Load(int3(clamp(p + int2(0, 1), 0, hi), 0)).rgb);
    float3 mn4 = min(min(b, d), min(f, g));
    float3 mx4 = max(max(b, d), max(f, g));
    // Largest negative lobe that keeps every channel inside [min, max] of the cross.
    float3 hit_min = min(mn4, e) / (4 * mx4 + 1e-5);
    float3 hit_max = (1 - max(mx4, e)) / (4 * mn4 - 4);
    float3 lobe3 = max(-hit_min, hit_max);
    float lobe = max(-0.1875, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0)) * amount;  // RCAS_LIMIT = 0.25 - 1/16
    float3 o = (lobe * (b + d + f + g) + e) / (4 * lobe + 1);
    return float4(inv(saturate(o)), ce.a);
}
)";

}  // namespace cs::dlss_pass
