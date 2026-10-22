// ShaderTool container test: ray-tracing entry points, compiled for optix/dxil/spirv.
// tests/ContainerTests.cpp checks the layout of these parameters; keep them in sync.
//
// CUDA SLANG_globalParams: g_Scene 0 (8)  g_Output 8 (8)  g_Camera 16 (8)
//                          g_Colors 24 (16)  g_Time 40 (4)  -> 48 bytes
// closestHit SBT record data: hitColor 0 (16)  materialIndex 16 (4)  -> 32 bytes

struct Payload
{
    float3 color;
};

struct Camera
{
    float4x4 viewInverse;
    float3 origin;
    float tMax;
};

RaytracingAccelerationStructure g_Scene : register(t0, space2);
RWTexture2D<float4> g_Output;
ConstantBuffer<Camera> g_Camera;
StructuredBuffer<float4> g_Colors;
uniform float g_Time;

[shader("raygeneration")]
void rayGen(uniform float exposure)
{
    uint2 index = DispatchRaysIndex().xy;
    RayDesc ray;
    ray.Origin = g_Camera.origin;
    ray.Direction = normalize(mul(g_Camera.viewInverse, float4(float(index.x), float(index.y), 1.0, 0.0)).xyz);
    ray.TMin = 0.001;
    ray.TMax = g_Camera.tMax;
    Payload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    TraceRay(g_Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
    g_Output[index] = float4(payload.color * exposure, g_Time);
}

[shader("miss")]
void miss(inout Payload payload)
{
    payload.color = float3(0.1, 0.2, 0.3);
}

[shader("closesthit")]
void closestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes,
                uniform float4 hitColor, uniform uint materialIndex)
{
    payload.color = hitColor.xyz * g_Colors[materialIndex].xyz * attributes.barycentrics.x;
}

[shader("anyhit")]
void anyHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    if (attributes.barycentrics.y > 0.9)
        IgnoreHit();
}
