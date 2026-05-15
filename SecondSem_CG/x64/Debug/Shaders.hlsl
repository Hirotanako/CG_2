cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float _Pad[32];
};

cbuffer MatCB : register(b1)
{
    float4 Kd;
};

Texture2D Albedo : register(t0);
SamplerState Samp : register(s0);

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 clipPos : SV_POSITION;
    float3 nrmW : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 w = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(w, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 albedo = Albedo.Sample(Samp, input.uv).rgb * Kd.rgb;
    float3 L = normalize(float3(0.35f, 0.85f, 0.35f));
    float nd = saturate(dot(normalize(input.nrmW), L));
    float3 col = albedo * (0.12f + nd * 0.88f);
    return float4(col, 1.0f);
}
