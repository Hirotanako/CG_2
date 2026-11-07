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
    if (dispatchThreadId.x >= 512)
        return;

    Particle p = ParticlesToUpdate.Consume();
    const float dt = EmitterAndDeltaTime.w;
    p.age += dt;

    if (p.age >= p.lifetime)
    {
        const uint seed = dispatchThreadId.x + (uint)(TimeAndSize.x * 1000.0);
        const float speed = 0.75 + Hash(seed * 3 + 3) * 0.65;
        const float sideDrift = (Hash(seed + 57) - 0.5) * 0.12;
        const float depthDrift = (Hash(seed + 73) - 0.5) * 0.10;
        const uint direction = (uint)TimeAndSize.z;
        p.position = EmitterAndDeltaTime.xyz;
        if (direction == 0)
            p.velocity = float3(sideDrift, speed, depthDrift);
        else if (direction == 1)
            p.velocity = float3(sideDrift, -speed, depthDrift);
        else if (direction == 2)
            p.velocity = float3(-speed, sideDrift, depthDrift);
        else
            p.velocity = float3(speed, sideDrift, depthDrift);
        p.age = 0.0;
        p.lifetime = 2.4 + Hash(seed + 17) * 1.2;
        const float tint = Hash(seed + 31);
        p.color = float4(0.72 + tint * 0.20, 0.90 + tint * 0.08, 1.0, 1.0);
    }
    else
    {
        const uint direction = (uint)TimeAndSize.z;
        const float2 directionXY[4] = {
            float2(0.0, 1.0), float2(0.0, -1.0),
            float2(-1.0, 0.0), float2(1.0, 0.0)
        };
        p.velocity.xy += directionXY[direction] * TimeAndSize.w * dt;
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
    const float2 p = input.uv * 2.0 - 1.0;
    clip(1.0 - dot(p, p));

    // Eight radial triangular cut-outs turn the circular billboard into a
    // stylized snowflake while keeping its outside contour round.
    [unroll]
    for (uint hole = 0; hole < 8; ++hole)
    {
        const float angle = (hole + 0.5) * 0.785398163;
        const float2 radial = float2(cos(angle), sin(angle));
        const float2 tangent = float2(-radial.y, radial.x);
        const float2 a = radial * 0.30;
        const float2 b = radial * 0.72 + tangent * 0.13;
        const float2 c = radial * 0.72 - tangent * 0.13;
        const float sideAB = cross(float3(b - a, 0.0), float3(p - a, 0.0)).z;
        const float sideBC = cross(float3(c - b, 0.0), float3(p - b, 0.0)).z;
        const float sideCA = cross(float3(a - c, 0.0), float3(p - c, 0.0)).z;
        if ((sideAB >= 0.0 && sideBC >= 0.0 && sideCA >= 0.0) ||
            (sideAB <= 0.0 && sideBC <= 0.0 && sideCA <= 0.0))
            clip(-1.0);
    }
    ParticleGBufferOut output;
    output.albedo = float4(input.color.rgb, 0.0); // dielectric metallic value
    output.normal = float4(normalize(input.normal), 0.65); // PBR roughness
    output.position = float4(input.worldPosition, 1.0);
    return output;
}
