// ShaderTool container test: CUDA-only code (pointers, descriptor handles), compiled for ptx only. It does not compile
// for the other targets, which is the point: no D3D register layout exists for it.
// tests/ContainerTests.cpp checks the reflection of these parameters and the floating-point mode of the PTX
// (the module is built twice, precise and with -fp-mode fast); keep them in sync.
//
// CUDA SLANG_globalParams: g_Constants 0 (8: the address of the push constants)  g_Plain 8 (4)  -> 16 bytes
// Entry point parameters (fill): values 0 (8)  surface 8 (8)  -> 16 bytes

struct Constants
{
    float4* output;
    float scale;
    uint count;
};

// A push-constant block needs no register: the backend finds it by its kind.
[[vk::push_constant]] ConstantBuffer<Constants> g_Constants;

// Plain data globals are allowed in CUDA-only code.
uniform float g_Plain;

[shader("compute")]
[numthreads(32, 1, 1)]
void fill(uint3 tid : SV_DispatchThreadID, uniform float* values, uniform RWTexture2D<float4>.Handle surface)
{
    if (tid.x >= g_Constants.count)
        return;
    const float v = values[tid.x];
    // A division and a square root: IEEE (div.rn / sqrt.rn) by default, approximate with -fp-mode fast.
    const float4 result = float4(sqrt(v) / g_Constants.scale + g_Plain, v, 0.0, 1.0);
    g_Constants.output[tid.x] = result;
    surface[uint2(tid.x, 0)] = result;
}
