// ShaderTool container test: one compute kernel, compiled for ptx/dxil/spirv.
// tests/ContainerTests.cpp checks the layout of these parameters; keep them in sync.
//
// CUDA SLANG_globalParams (natural layout, resources are handles/pointers):
//   g_Params 0 (8)  g_Particles 8 (16)  g_Output 24 (16)  g_Raw 40 (16)
//   g_Albedo 56 (2 x 8)  g_Image 72 (8)  g_Sampler 80 (8)  g_Scale 88 (4)  -> 96 bytes

struct Particle
{
    float3 position;
    float mass;
};

struct SimParams
{
    float4x4 transform;
    float3 gravity;
    uint count;
};

ConstantBuffer<SimParams> g_Params : register(b1);
StructuredBuffer<Particle> g_Particles : register(t3, space1);
RWStructuredBuffer<float4> g_Output : register(u0);
ByteAddressBuffer g_Raw;
Texture2D<float4> g_Albedo[2];
RWTexture2D<float4> g_Image;
SamplerState g_Sampler;
uniform float g_Scale;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uniform float entryScale, uniform uint entryOffset)
{
    Particle particle = g_Particles[tid.x];
    float4 value = mul(g_Params.transform, float4(particle.position, 1.0));
    value.xyz += g_Params.gravity * particle.mass * g_Scale * entryScale;
    value += g_Albedo[tid.x & 1].SampleLevel(g_Sampler, float2(0.5, 0.5), 0);
    value.w += asfloat(g_Raw.Load((tid.x + entryOffset) * 4));
    if (tid.x < g_Params.count)
        g_Output[tid.x] = value;
    g_Image[uint2(tid.x & 63, tid.x >> 6)] = value;
}
