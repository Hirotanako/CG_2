// Geometry: tessellation + displacement + normal map → G-buffer. Lighting pass ниже.

cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 TimeCamPos;     // x=time, yzw=camera world pos
    float4 UvAnimAndPad;
    float4 TessParams;     // x=maxFactor, y=minDist, z=maxDist
    float4 DebugView;      // x=1 — каркас (wireframe), без текстур
    float _PadRest[16];
};

cbuffer MatCB : register(b1)
{
    float4 Kd;
    float2 UvScale;
    float2 UvOffset;
    float4 MatFlags;       // x=dispScale, y=normalStrength, z=useUvAnim, w=hasNormalMap
    float4 MatFlags2;      // x=hasDispMap, y=invert (roughness как height)
    float _PadMat[48];
};

Texture2D Albedo : register(t0);
Texture2D NormalMap : register(t1);
Texture2D DispMap : register(t2);
SamplerState Samp : register(s0);

struct GeoVsIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct HSControlPoint
{
    float3 posW : POSITION0;
    float3 nrmW : NORMAL;
    float4 tanW : TANGENT;
    float2 uv : TEXCOORD0;
};

struct HSConstantOutput
{
    float TessFactor[3] : SV_TessFactor;
    float InsideTessFactor : SV_InsideTessFactor;
};

struct DSOut
{
    float4 clipPos : SV_POSITION;
    float3 posW : POSITION0;
    float3 nrmW : NORMAL;
    float4 tanW : TANGENT;
    float2 uv : TEXCOORD0;
};

float2 MaterialUv(float2 uv, float time)
{
    float2 u = uv * UvScale + UvOffset;
    if (MatFlags.z > 0.5f)
        u += UvAnimAndPad.xy * time;
    return u;
}

bool IsFrameDebug()
{
    return DebugView.x > 0.5f;
}

float SampleDisplacement(float2 uv)
{
    if (IsFrameDebug() || MatFlags2.x < 0.5f)
        return 0.5f;
    float h = DispMap.SampleLevel(Samp, uv, 0).r;
    if (MatFlags2.y > 0.5f)
        h = 1.0f - h;
    return h;
}

float3 ApplyNormalMap(float3 nrmW, float4 tanW, float2 uv)
{
    float3 N = normalize(nrmW);
    if (IsFrameDebug() || MatFlags.w < 0.5f)
        return N;

    float3 nTS = NormalMap.SampleLevel(Samp, uv, 0).rgb * 2.0f - 1.0f;
    nTS.y = -nTS.y;
    nTS.xy *= MatFlags.y;
    nTS = normalize(nTS);

    float3 T = normalize(tanW.xyz);
    float3 B = normalize(cross(N, T) * tanW.w);
    T = normalize(cross(B, N));
    return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}

HSControlPoint GeometryVS(GeoVsIn input)
{
    HSControlPoint o;
    float4 wpos = mul(float4(input.pos, 1.0f), World);
    float3x3 W = (float3x3)World;
    o.posW = wpos.xyz;
    o.nrmW = normalize(mul(input.normal, W));
    o.tanW = float4(normalize(mul(input.tangent.xyz, W)), input.tangent.w);
    o.uv = input.uv;
    return o;
}

float ComputeTessFactor(float3 patchCenterW)
{
    float maxF = TessParams.x;
    float minD = TessParams.y;
    float maxD = max(TessParams.z, minD + 1e-3);
    float dist = distance(patchCenterW, TimeCamPos.yzw);
    float t = saturate((maxD - dist) / (maxD - minD));
    t *= t;
    return max(1.0f, lerp(1.0f, maxF, t));
}

HSConstantOutput HSConstant(InputPatch<HSControlPoint, 3> patch, uint pid : SV_PrimitiveID)
{
    HSConstantOutput o;
    float3 c = (patch[0].posW + patch[1].posW + patch[2].posW) / 3.0f;
    float tess = ComputeTessFactor(c);
    o.TessFactor[0] = tess;
    o.TessFactor[1] = tess;
    o.TessFactor[2] = tess;
    o.InsideTessFactor = tess;
    return o;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[patchconstantfunc("HSConstant")]
[outputcontrolpoints(3)]
[maxtessfactor(8.0)]
HSControlPoint HSMain(InputPatch<HSControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

[domain("tri")]
DSOut DSMain(HSConstantOutput tess, const OutputPatch<HSControlPoint, 3> patch, float3 bary : SV_DomainLocation)
{
    DSOut o;
    float3 posW = patch[0].posW * bary.x + patch[1].posW * bary.y + patch[2].posW * bary.z;
    float3 nrmW = patch[0].nrmW * bary.x + patch[1].nrmW * bary.y + patch[2].nrmW * bary.z;
    float4 tanW = patch[0].tanW * bary.x + patch[1].tanW * bary.y + patch[2].tanW * bary.z;
    float2 uv = patch[0].uv * bary.x + patch[1].uv * bary.y + patch[2].uv * bary.z;

    nrmW = normalize(nrmW);
    float2 muv = MaterialUv(uv, TimeCamPos.x);
    float h = SampleDisplacement(muv);
    posW += nrmW * ((h - 0.5f) * 2.0f * MatFlags.x);

    o.posW = posW;
    o.nrmW = nrmW;
    o.tanW = tanW;
    o.uv = uv;
    o.clipPos = mul(float4(posW, 1.0f), ViewProj);
    return o;
}

struct GeoRtOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

GeoRtOut GeometryPS(DSOut input)
{
    GeoRtOut o;
    float3 N = normalize(input.nrmW);

    if (IsFrameDebug())
    {
        o.albedo = float4(0.15f, 0.92f, 1.0f, 1);
        o.normal = float4(N * 0.5f + 0.5f, 0);
        o.position = float4(input.posW, 1);
        return o;
    }

    float2 muv = MaterialUv(input.uv, TimeCamPos.x);
    float3 a = Albedo.Sample(Samp, muv).rgb * Kd.rgb;
    N = ApplyNormalMap(input.nrmW, input.tanW, muv);
    o.albedo = float4(a, 1);
    o.normal = float4(N, 0);
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

    if (LightCount == 0)
        return float4(alb, 1.f);

    float3 color = alb * 0.035f;

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
