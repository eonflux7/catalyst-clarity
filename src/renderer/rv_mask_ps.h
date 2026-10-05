#pragma once

// HLSL port of the Runner's Vision mask resolve in the engine's TAA pixel shader (docs/runners-vision.md;
// numbers = instruction indices in the original). Runs at the TAA draw
// with the engine's bindings as they are (b0 ShaderConstants slice, t0 depth, t1 velocity, t5 mask,
// t6 mask history, s1 trilinear) and RTV3 (resolved mask) / RTV4 (mask history) as SV_Target0/1.
// Every size-dependent input is render size and already in the engine's cb0, and its neighbour weights
// match the engine jitter, which DLSS also gets, so nothing is patched.
namespace cs::dlss_pass {

constexpr const char* kRvMaskPs = R"(
cbuffer ShaderConstants : register(b0) {        // engine slice of the 1 MB ring, reflection offsets
    uint  g_width, g_height;                     // 0   render size
    float g_invWidth, g_invHeight;               // 8
    uint  g_debugMode; float g_debugParams;      // 16
    float g_minHistoryBlendFactor;               // 24  0.0
    float g_maxHistoryBlendFactor;               // 28  1.0
    float g_exposureMultiplier;                  // 32  E_t (0.2748)
    float g_unexposureMultiplier;                // 36  1/E_t
    float g_historyUnexposureMultiplier;         // 40  1/E_{t-1}
    float g_disocclusionRejectionFactor;         // 44  (unused)
    float g_motionSharpeningFactor;              // 48  1.0
    float g_antiflickerMultiplier;               // 52  0.1
    float g_antiflickerInDistance;               // 56  0.0
    float g_antiflickerOutDistance;              // 60  1.0
    float4x4 g_currentToPrevFrameTransform;      // 64  (unused)
    float4x4 g_invJitteredViewProjection;        // 128 (unused)
    float4x4 g_invUnjitteredViewProjection;      // 192 (unused)
    float4 g_neighborWeights[3];                 // 256 3x3 taps NW,N,NE,W,C,E,SW,S,SE; per-frame, jitter dependent, sum 1
    float4 g_neighborLowWeights[3];              // 304 colour only
    float4 g_outputQuantizationBias;             // 352 colour only
    float  g_lumaContrastFactor;                 // 368 colour only
};
Texture2D g_depthTexture : register(t0);
Texture2D g_velocityTexture : register(t1);
Texture2D g_inputTexture1 : register(t5);           // RV mask R8 (120151)
Texture2D g_historyInputTexture1 : register(t6);    // RV mask history R8
SamplerState g_pointSampler : register(s0);
SamplerState g_linearSampler : register(s1);

float M(int2 p) { return g_inputTexture1.Load(int3(p, 0)).x; }   // engine: 4 point gather4 at texel corners (266-271)
float D(int2 p) { return g_depthTexture.Load(int3(p, 0)).x; }

struct Out { float mask : SV_Target0; float history : SV_Target1; };  // engine o3 (RTV3), o4 (RTV4)

Out main(float4 pos : SV_Position) {
    int2 p = int2(pos.xy);                                                        // 0
    float2 size = float2(g_width, g_height), inv = float2(g_invWidth, g_invHeight);
    float2 uv = (float2(p) + 0.5) * inv;                                          // 1-3
    // velocity: nearest diagonal (largest depth value) if it beats the centre     // 5-36
    float dNW = D(p + int2(-1,-1)), dNE = D(p + int2(1,-1)), dSW = D(p + int2(-1,1)), dSE = D(p + int2(1,1));
    float top = max(dNW, dNE), bot = max(dSW, dSE);
    int2 vp = p;
    if (D(p) < max(top, bot)) {
        float2 o = top < bot ? (dSW < dSE ? float2(1,1) : float2(-1,1))
                             : (dNW < dNE ? float2(1,-1) : float2(-1,-1));
        vp = int2((uv + o * inv) * size);
    }
    float2 v = g_velocityTexture.Load(int3(vp, 0)).xy * float2(0.5, -0.5);        // 38-39 NDC cur-prev -> UV
    float2 prevUv = uv - v;                                                       // 40
    float2 prevNdc = abs(prevUv * 2 - 1) + 2 * inv;                               // 41,44
    bool offscreen = max(prevNdc.x, prevNdc.y) >= 1;                              // 45-46
    float2 vpx = v * size;                                                        // 43 (pixels)
    float motion = saturate((abs(vpx.x) + abs(vpx.y)) * g_motionSharpeningFactor); // 47-48
    prevUv = saturate(prevUv);                                                    // 49

    float NW = M(p + int2(-1,-1)), N = M(p + int2(0,-1)), NE = M(p + int2(1,-1));
    float W  = M(p + int2(-1, 0)), C = M(p),               E  = M(p + int2(1, 0));
    float SW = M(p + int2(-1, 1)), S = M(p + int2(0, 1)),  SE = M(p + int2(1, 1));

    float hs = g_historyInputTexture1.SampleLevel(g_linearSampler, prevUv, 0).x   // 272 bilinear
             * (g_exposureMultiplier * g_historyUnexposureMultiplier);            // 155,273 (~1; shared with colour)
    float cur = offscreen ? C                                                     // 274-286
        : dot(g_neighborWeights[0], float4(NW, N, NE, W))
        + dot(g_neighborWeights[1], float4(C, E, SW, S)) + g_neighborWeights[2].x * SE;
    float crossMin = min(C, min(min(S, N), min(E, W)));                           // 293-296
    float crossMax = max(C, max(max(S, N), max(E, W)));                           // 297-300
    float cornerMin = min(min(NW, NE), min(SE, SW)), cornerMax = max(max(NW, NE), max(SE, SW)); // 287-292
    float mn = 0.5 * crossMin + 0.5 * min(crossMin, cornerMin);                   // 301-303
    float mx = 0.5 * crossMax + 0.5 * max(crossMax, cornerMax);                   // 304-306
    float contrast = 2 * max(mx - mn, 0) / max(mn + mx, 1e-5);                    // 307-311
    float d = min(abs(mx - hs), abs(mn - hs));                                    // 312-314
    float t = saturate((d - g_antiflickerInDistance) /
                       (g_antiflickerOutDistance - g_antiflickerInDistance));     // 221-223,315-316
    float a = (t * t * (3 - 2 * t) + motion + g_antiflickerMultiplier) * 0.125;   // 317-321
    a *= 1 + 8 * a * motion;                                                      // 322-324
    a = saturate(a / (1 + contrast));                                             // 325-326
    a = clamp(a, g_minHistoryBlendFactor, g_maxHistoryBlendFactor);               // 327-328 (weight of current)
    float hc = offscreen ? cur : min(mx, max(hs, mn));                            // 329-334
    float r = lerp(hc, cur, a);                                                   // 335-336
    if (isnan(r)) r = 0;                                                          // 337-338
    Out o; o.mask = r; o.history = r; return o;                                   // 342-343
}
)";

}  // namespace cs::dlss_pass
