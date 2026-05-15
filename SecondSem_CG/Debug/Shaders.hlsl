cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float _Pad[32];
};

cbuffer MatCB : register(b1)
{
    float3 Kd;
    float _MatPad;
};

Texture2D Albedo : register(t0);
SamplerState Samp : register(s0);

struct VSInputSponza
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOutputSponza
{
    float4 clipPos : SV_POSITION;
    float3 nrmW : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutputSponza VSMainSponza(VSInputSponza input)
{
    VSOutputSponza o;
    float4 w = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(w, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.uv = input.uv;
    return o;
}

float4 PSMainSponza(VSOutputSponza input) : SV_TARGET
{
    float3 albedo = Albedo.Sample(Samp, input.uv).rgb * Kd;
    float3 L = normalize(float3(0.35f, 0.85f, 0.35f));
    float nd = saturate(dot(normalize(input.nrmW), L));
    float3 amb = 0.12f;
    float3 col = albedo * (amb + nd * 0.88f);
    return float4(col, 1.0f);
}
