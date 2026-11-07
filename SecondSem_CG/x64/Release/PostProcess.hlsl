Texture2D InputTexture : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer PostProcessConstants : register(b0)
{
    float2 TexelSize;
    float AberrationStrength;
    float Padding;
};

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FullscreenOutput PostProcessVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = float2(uv.x, 1.0 - uv.y);
    return output;
}

float4 ChromaticAberrationPS(FullscreenOutput input) : SV_Target0
{
    const float2 fromCenter = input.uv - 0.5;
    const float radius = length(fromCenter);

    // Keep the image sharp in the center and separate the color channels
    // progressively towards the edges. AberrationStrength is driven by the
    // smoothed camera speed on the CPU.
    const float edgeMask = smoothstep(0.0, 0.62, radius);
    const float2 radialDirection = fromCenter / max(radius, 1e-4);
    const float2 offset = radialDirection * AberrationStrength * edgeMask;

    const float red = InputTexture.Sample(LinearClampSampler, input.uv + offset).r;
    const float green = InputTexture.Sample(LinearClampSampler, input.uv).g;
    const float blue = InputTexture.Sample(LinearClampSampler, input.uv - offset).b;
    return float4(red, green, blue, 1.0);
}
