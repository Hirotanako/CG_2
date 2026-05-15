// Этап геометрии отложенного рендера (заполнение G-buffer) и полноэкранный проход света.

cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 TimeCamPos;
    float4 UvAnimAndPad;
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

struct GeoVsIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct GeoVsOut
{
    float4 clipPos : SV_POSITION;
    float3 nrmW : NORMAL;
    float3 posW : POSITION0;
    float2 uv : TEXCOORD0;
};

GeoVsOut GeometryVS(GeoVsIn input)
{
    GeoVsOut o;
    float4 wpos = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(wpos, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.posW = wpos.xyz;
    o.uv = input.uv;
    return o;
}

struct GeoRtOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

GeoRtOut GeometryPS(GeoVsOut input)
{
    GeoRtOut o;
    float time = TimeCamPos.x;
    float2 uv = input.uv * UvScale + UvOffset;
    if (UseUvAnim != 0)
        uv += UvAnimAndPad.xy * time;
    float3 a = Albedo.Sample(Samp, uv).rgb * Kd.rgb;
    o.albedo = float4(a, 1);
    o.normal = float4(normalize(input.nrmW), 0);
    o.position = float4(input.posW, 1);
    return o;
}

// --- Lighting pass ---

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GPos : register(t2);
SamplerState GSamp : register(s0);

#define LIGHT_DIR 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define MAX_LIGHTS 8

struct GpuLight
{
    float4 position_range;
    float4 direction_cosOuter;
    float4 color_intensity;
    uint type;
    float spotCosInner;
    uint2 pad;
};

cbuffer LightingCB : register(b0)
{
    float4 CameraPos_pad;
    float4 InvScreen_pad;
    uint LightCount;
    uint3 padHdr;
    GpuLight Lights[MAX_LIGHTS];
};

struct FsOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FsOut LightingFullscreenVS(uint vid : SV_VertexID)
{
    FsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
    o.uv = float2(uv.x, 1.f - uv.y);
    return o;
}

float4 LightingPS(FsOut pin) : SV_Target0
{
    float3 alb = GAlbedo.Sample(GSamp, pin.uv).rgb;
    float3 N = GNormal.Sample(GSamp, pin.uv).xyz;
    float3 P = GPos.Sample(GSamp, pin.uv).xyz;

    float3 color = alb * 0.055f;

    if (dot(N, N) < 1e-6f)
        return float4(color, 1.f);

    N = normalize(N);
    float3 V = normalize(CameraPos_pad.xyz - P);

    for (uint i = 0; i < LightCount; ++i)
    {
        GpuLight Lg = Lights[i];
        float3 Lc = Lg.color_intensity.xyz;
        float I = Lg.color_intensity.w;
        float3 Ldir = float3(0, 0, 0);
        float att = 1.f;

        if (Lg.type == LIGHT_DIR)
        {
            Ldir = normalize(-Lg.direction_cosOuter.xyz);
        }
        else if (Lg.type == LIGHT_POINT)
        {
            float3 toL = Lg.position_range.xyz - P;
            float dist = length(toL);
            if (dist > Lg.position_range.w)
                continue;
            Ldir = toL / max(dist, 1e-5);
            float t = 1.f - saturate(dist / Lg.position_range.w);
            att = t * t;
        }
        else
        {
            float3 toL = Lg.position_range.xyz - P;
            float dist = length(toL);
            if (dist > Lg.position_range.w)
                continue;
            Ldir = toL / max(dist, 1e-5);
            float t = 1.f - saturate(dist / Lg.position_range.w);
            att = t * t;
            float3 axis = normalize(Lg.direction_cosOuter.xyz);
            float rho = dot(-Ldir, axis);
            float cosO = Lg.direction_cosOuter.w;
            float cosI = Lg.spotCosInner;
            float spot = saturate((rho - cosO) / max(cosI - cosO, 1e-4));
            att *= spot * spot;
        }

        float diff = saturate(dot(N, Ldir));
        float3 H = normalize(Ldir + V);
        float spec = pow(saturate(dot(N, H)), 48.f) * 0.28f;
        color += (alb * diff + spec) * Lc * I * att;
    }
    return float4(color, 1.f);
}
