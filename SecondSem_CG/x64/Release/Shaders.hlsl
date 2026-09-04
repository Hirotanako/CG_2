cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 TimeCamPos;     // x — время (сек), yzw — позиция камеры в мире
    float4 UvAnimAndPad;   // xy — скорость прокрутки UV (единиц текстуры / сек)
    float _PadRest[24];
};

cbuffer MatCB : register(b1)
{
    float4 Kd;
    float2 UvScale;
    float2 UvOffset;
    float3 Ks;
    float Ns;
    uint UseUvAnim;
    uint HasSpecularTex;
    float _PadMat[50];
};

Texture2D Albedo : register(t0);
Texture2D SpecMap : register(t1);
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
    float3 posW : POSITION1;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 wpos = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(wpos, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.posW = wpos.xyz;
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float time = TimeCamPos.x;
    float3 camPos = TimeCamPos.yzw;

    float2 uv = input.uv * UvScale + UvOffset;
    if (UseUvAnim != 0)
        uv += UvAnimAndPad.xy * time;

    float3 albedo = Albedo.Sample(Samp, uv).rgb * Kd.rgb;

    float3 specMask = HasSpecularTex != 0 ? SpecMap.Sample(Samp, uv).rgb : float3(1.0f, 1.0f, 1.0f);
    float3 specColor = specMask * Ks;

    float3 N = normalize(input.nrmW);
    float3 L = normalize(float3(0.35f, 0.85f, 0.35f));
    float3 V = normalize(camPos - input.posW);
    float3 H = normalize(L + V);

    float nh = saturate(dot(N, H));
    float specTerm = pow(nh, max(Ns / 64.0f, 4.0f));

    float ndl = saturate(dot(N, L));
    float3 ambient = albedo * 0.12f;
    float3 diffuse = albedo * ndl * 0.72f;
    float3 specular = specColor * specTerm * ndl * 0.45f;

    return float4(ambient + diffuse + specular, 1.0f);
}
