// Homework #6: GPU-only position update with Consume/Append buffers and
// point-to-billboard expansion in a geometry shader.

struct Particle
{
    float3 position;
    float age;
    float3 velocity;
    float lifetime;
    float4 color;
};

cbuffer ParticleConstants : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 CameraRight;
    float4 CameraUp;
    float4 CameraPosition;
    float4 EmitterAndDeltaTime;
    float4 TimeAndSize;
    float4 Padding[7];
};

ConsumeStructuredBuffer<Particle> ParticlesToUpdate : register(u0);
AppendStructuredBuffer<Particle> UpdatedParticles : register(u1);
StructuredBuffer<Particle> ParticlesToDraw : register(t0);

float Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return (value & 0x00ffffff) / 16777216.0;
}

[numthreads(64, 1, 1)]
void ParticleUpdateCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= 2048)
        return;

    Particle p = ParticlesToUpdate.Consume();
    const float dt = EmitterAndDeltaTime.w;
    p.age += dt;

    if (p.age >= p.lifetime)
    {
        const uint seed = dispatchThreadId.x + (uint)(TimeAndSize.x * 1000.0);
        const float angle = Hash(seed * 3 + 1) * 6.2831853;
        const float radius = sqrt(Hash(seed * 3 + 2)) * 0.22;
        const float speed = 0.85 + Hash(seed * 3 + 3) * 0.75;
        p.position = float3(
            EmitterAndDeltaTime.x + cos(angle) * radius,
            EmitterAndDeltaTime.y,
            EmitterAndDeltaTime.z + sin(angle) * radius);
        p.velocity = float3(-cos(angle) * 0.12, -speed, -sin(angle) * 0.12);
        p.age = 0.0;
        p.lifetime = 2.4 + Hash(seed + 17) * 1.2;
        const float tint = Hash(seed + 31);
        p.color = float4(1.0, 0.25 + tint * 0.45, 0.04, 1.0);
    }
    else
    {
        // The complete trajectory is inverted: initial velocity and
        // acceleration both point toward negative Y.
        p.velocity.y -= TimeAndSize.w * dt;
        p.position += p.velocity * dt;
    }

    UpdatedParticles.Append(p);
}

struct ParticleVsOut
{
    float3 position : POSITION0;
    float4 color : COLOR0;
    float size : SIZE0;
};

ParticleVsOut ParticleVS(uint vertexId : SV_VertexID)
{
    Particle p = ParticlesToDraw[vertexId];
    ParticleVsOut output;
    output.position = p.position;
    output.color = p.color;
    output.size = TimeAndSize.y * (0.7 + 0.3 * saturate(1.0 - p.age / p.lifetime));
    return output;
}

struct ParticleGsOut
{
    float4 clipPosition : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[maxvertexcount(4)]
void ParticleGS(point ParticleVsOut input[1], inout TriangleStream<ParticleGsOut> stream)
{
    const float3 center = input[0].position;
    const float3 right = normalize(CameraRight.xyz) * input[0].size;
    const float3 up = normalize(CameraUp.xyz) * input[0].size;
    const float3 normal = normalize(CameraPosition.xyz - center);
    const float2 uv[4] = {
        float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 1.0), float2(1.0, 0.0)
    };
    const float3 offsets[4] = {-right - up, -right + up, right - up, right + up};

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        ParticleGsOut output;
        output.worldPosition = center + offsets[i];
        output.clipPosition = mul(float4(output.worldPosition, 1.0), ViewProjection);
        output.normal = normal;
        output.uv = uv[i];
        output.color = input[0].color;
        stream.Append(output);
    }
}

struct ParticleGBufferOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

ParticleGBufferOut ParticlePS(ParticleGsOut input)
{
    // A hard edge is still opaque: surviving samples always write alpha = 1
    // and also write depth through the opaque particle PSO.
    clip(1.0 - dot(input.uv * 2.0 - 1.0, input.uv * 2.0 - 1.0));
    ParticleGBufferOut output;
    output.albedo = float4(input.color.rgb, 1.0);
    output.normal = float4(normalize(input.normal), 0.0);
    output.position = float4(input.worldPosition, 1.0);
    return output;
}
