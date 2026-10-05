#pragma once

// HLSL port of the engine's motion blur pixel shader, line for line, with two changes:
//  - invPixelSize comes from b1 (ours, 1/output) instead of $Globals (1/render), so the pass can run at
//    output size without touching the engine's constant ring buffer (copying it hung the GPU);
//  - the sample count is clamped to [1, 256] and NaN-guarded, so it cannot spin the GPU whatever the
//    inputs (the engine clamps it only by motionBlurParams.w).
// The engine's $Globals stays bound as b0 with its own offsets; the layout below is its reflection.
namespace cs::post {

constexpr const char* kMotionBlurPs = R"(
cbuffer Globals : register(b0) {
    float2 engineInvPixelSize;                           // 0 (1/render: not used)
    float4x4 motionBlurFastStableVelocityParams_Vertex;  // 16
    float4 motionBlurFastStableVelocityParams_Pixel;     // 80
    float4x4 motionBlurFastPreciseVelocityParams_Pixel;  // 96
    float4 motionBlurParams;                             // 160: x strength, y length scale, z jitter, w max samples
    float4 motionBlurParams2;                            // 176: x depth bias, y depth cap, z H/W
    float4x4 invViewProjectionMatrix;                    // 192
    float4x4 prevViewProjectionMatrix;                   // 256
    float4 cameraPosAndDepthCutoff;                      // 320
    float4 radialBlurParams;                             // 336: xy centre, z scale, w radius
    float cutoffGradientScale;                           // 352
};
cbuffer Mod : register(b1) { float2 invPixelSize; };    // 1/output

Texture2D depthTexture : register(t0);
Texture2D mainTexture : register(t1);
Texture2D randomTexture : register(t2);
Texture2D velocityTexture : register(t3);
SamplerState depthTextureSampler : register(s0);
SamplerState mainTextureSampler : register(s1);
SamplerState randomTextureSampler : register(s2);
SamplerState velocityTextureSampler : register(s3);

float4 main(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * invPixelSize;                                           // 0
    float2 vel = velocityTexture.Sample(velocityTextureSampler, uv).xy;          // 1
    vel *= motionBlurParams.xx * float2(-1, 1);                                  // 2-3
    float len = length(vel) * motionBlurParams.y;                                // 4-6
    if (1 < len)                                                                 // 7-9
        vel /= len;
    float2 d = -pos.xy * invPixelSize + radialBlurParams.xy;                     // 10
    float2 dz = float2(d.x, d.y * motionBlurParams2.z);                          // 11
    float dl = length(dz);                                                       // 12-13
    float r = (dl - radialBlurParams.w) * radialBlurParams.z;                    // 14-15
    float occluded;
    if (0 < r) {                                                                 // 16-17
        vel += r * (dz / dl);                                                    // 18-19
        float t = saturate((abs(r) + abs(r)) / len);                             // 21-22
        occluded = 0 < len ? t : 1.0;                                            // 20, 23
    } else {
        occluded = 0;                                                            // 25
    }
    float3 colour = mainTexture.SampleLevel(mainTextureSampler, uv, 0).xyz;      // 27
    float depth = depthTexture.SampleLevel(depthTextureSampler, uv, 0).x;        // 28
    depth = min(depth - motionBlurParams2.x, motionBlurParams2.y);               // 29-30
    float n = max(length(vel / invPixelSize), 1);                                // 31-34
    float count = 1 < n ? ceil(n * 0.25) * 4 : 1;                                // 35-39
    count = min(count, motionBlurParams.w);                                      // 40
    count = count >= 1 ? min(count, 256) : 1;                                    // mod: cap + NaN guard
    float2 stp = vel * (1 / count);                                              // 41-42
    float jitter = randomTexture.SampleLevel(randomTextureSampler, pos.xy * 0.03125, 0).x *
                   motionBlurParams.z;                                           // 43-45
    float2 p = stp * jitter - count * stp * 0.5 + pos.xy * invPixelSize;         // 46-49
    float3 sum = colour;                                                         // 50
    float weight = 1;                                                            // 52
    [loop] for (float i = 1; i < count; i += 4) {                                // 53-56, 85
        [unroll] for (int k = 0; k < 4; ++k) {                                   // 57-84
            p += stp;
            float3 c = mainTexture.SampleLevel(mainTextureSampler, p, 0).xyz;
            float z = depthTexture.SampleLevel(depthTextureSampler, p, 0).x;
            float w = depth < z ? 1.0 : occluded;
            sum += c * w;
            weight += w;
        }
    }
    return float4(sum / weight, 1);                                              // 87-88
}
)";

}  // namespace cs::post
