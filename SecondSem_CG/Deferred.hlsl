// Этап геометрии отложенного рендера (G-buffer) + каскадные тени (CSM) + PCF.

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
    uint UseSwayAnim;
    float Metallic;
    float Roughness;
    uint HasNormalTex;
    float2 PadMaterial;
    float4 _PadMat[11];
};

Texture2D Albedo : register(t0);
Texture2D MetallicMap : register(t1);
Texture2D RoughnessMap : register(t2);
Texture2D NormalMap : register(t3);
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

float3 ApplyCurtainSway(float3 posL)
{
    float t = TimeCamPos.x;
    float hang = saturate(posL.y * 0.0065);
    float phase = posL.x * 0.028 + posL.z * 0.022;
    float s1 = sin(t * (1.05 * UvAnimAndPad.w) + phase);
    float s2 = sin(t * 1.55 + phase * 1.35 + 1.7) * 0.42;
    float sway = (s1 + s2) * UvAnimAndPad.z * hang;
    posL.x += sway;
    posL.z += sway * 0.28;
    return posL;
}

GeoVsOut GeometryVS(GeoVsIn input)
{
    GeoVsOut o;
    float3 posL = input.pos;
    if (UseSwayAnim != 0)
        posL = ApplyCurtainSway(posL);

    float4 wpos = mul(float4(posL, 1.0f), World);
    o.clipPos = mul(wpos, ViewProj);
    float3x3 W = (float3x3)World;
    o.nrmW = normalize(mul(input.normal, W));
    o.posW = wpos.xyz;
    o.uv = input.uv;
    return o;
}

GeoVsOut ShadowVS(GeoVsIn input)
{
    GeoVsOut o;
    float4 wpos = mul(float4(input.pos, 1.0f), World);
    o.clipPos = mul(wpos, ViewProj);
    o.nrmW = float3(0, 0, 0);
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
    float metallic = saturate(Metallic);
    if (HasSpecularTex != 0)
        metallic = saturate(metallic * MetallicMap.Sample(Samp, uv).r);
    float roughness = clamp(Roughness * RoughnessMap.Sample(Samp, uv).r, 0.045, 1.0);

    float3 normalW = normalize(input.nrmW);
    if (HasNormalTex != 0)
    {
        float3 tangentNormal = NormalMap.Sample(Samp, uv).xyz * 2.0 - 1.0;
        float3 dpdx = ddx(input.posW);
        float3 dpdy = ddy(input.posW);
        float2 duvdx = ddx(uv);
        float2 duvdy = ddy(uv);
        float3 tangent = normalize(dpdx * duvdy.y - dpdy * duvdx.y);
        float3 bitangent = normalize(-dpdx * duvdy.x + dpdy * duvdx.x);
        normalW = normalize(
            tangent * tangentNormal.x + bitangent * tangentNormal.y + normalW * tangentNormal.z);
    }

    // Albedo.a and Normal.a carry the PBR parameters through the G-buffer.
    o.albedo = float4(a, metallic);
    o.normal = float4(normalW, roughness);
    o.position = float4(input.posW, 1);
    return o;
}

// --- Lighting pass ---

Texture2D GAlbedo : register(t0);
Texture2D GNormal : register(t1);
Texture2D GPos : register(t2);
Texture2DArray ShadowMap : register(t3);
Texture2D EnvironmentMap : register(t4);
SamplerState GSamp : register(s0);
SamplerComparisonState ShadowSamp : register(s1);
SamplerState EnvironmentSamp : register(s2);

#define LIGHT_DIR 0
#define LIGHT_POINT 1
#define LIGHT_SPOT 2
#define MAX_LIGHTS 8
#define MAX_CASCADES 4

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

cbuffer ShadowCB : register(b1)
{
    row_major float4x4 LightViewProj[MAX_CASCADES];
    row_major float4x4 CameraView;
    float4 CascadeSplits;
    float4 ShadowParams;
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

uint SelectCascade(float viewDepth)
{
    if (viewDepth < CascadeSplits.x)
        return 0;
    if (viewDepth < CascadeSplits.y)
        return 1;
    if (viewDepth < CascadeSplits.z)
        return 2;
    return 3;
}

float SampleShadowPCF(float3 worldPos, float3 N, float3 Ldir)
{
    float3 samplePos = worldPos + N * ShadowParams.z;

    float viewDepth = mul(float4(samplePos, 1.f), CameraView).z;
    uint cascade = SelectCascade(viewDepth);

    float4 clip = mul(float4(samplePos, 1.f), LightViewProj[cascade]);
    float3 ndc = clip.xyz / clip.w;
    if (ndc.x < -1.f || ndc.x > 1.f || ndc.y < -1.f || ndc.y > 1.f)
        return 1.f;

    float2 uv = ndc.xy * 0.5f + 0.5f;
    uv.y = 1.f - uv.y;
    float depth = ndc.z;

    float ndotl = saturate(dot(N, Ldir));
    float slope = sqrt(1.f - ndotl * ndotl) / max(ndotl, 0.08f);
    float bias = ShadowParams.y + ShadowParams.w * slope;

    float texel = ShadowParams.x;
    float shadow = 0.f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            float2 offset = float2(dx, dy) * texel;
            shadow += ShadowMap.SampleCmpLevelZero(
                ShadowSamp,
                float3(uv + offset, cascade),
                depth + bias);
        }
    }
    shadow /= 9.f;
    return shadow * shadow;
}

static const float PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float nDotH = saturate(dot(N, H));
    float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denominator * denominator, 1e-5);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 1e-5);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    return GeometrySchlickGGX(saturate(dot(N, V)), roughness)
        * GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), f0) - f0)
        * pow(1.0 - saturate(cosTheta), 5.0);
}

float2 DirectionToLatLong(float3 direction)
{
    direction = normalize(direction);
    return float2(atan2(direction.z, direction.x) / (2.0 * PI) + 0.5,
        acos(clamp(direction.y, -1.0, 1.0)) / PI);
}

float3 SampleEnvironment(float3 direction)
{
    return EnvironmentMap.SampleLevel(EnvironmentSamp, DirectionToLatLong(direction), 0).rgb;
}

float4 LightingPS(FsOut pin) : SV_Target0
{
    float4 albedoMetallic = GAlbedo.Sample(GSamp, pin.uv);
    float3 alb = albedoMetallic.rgb;
    float metallic = saturate(albedoMetallic.a);
    float4 normalRoughness = GNormal.Sample(GSamp, pin.uv);
    float3 N = normalRoughness.xyz;
    float roughness = clamp(normalRoughness.a, 0.045, 1.0);
    float3 P = GPos.Sample(GSamp, pin.uv).xyz;

    if (dot(N, N) < 1e-6f)
        return float4(alb, 1.f);

    N = normalize(N);
    float3 V = normalize(CameraPos_pad.xyz - P);
    float nDotV = saturate(dot(N, V));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), alb, metallic);
    float3 directLighting = 0.0;

    for (uint i = 0; i < LightCount; ++i)
    {
        GpuLight Lg = Lights[i];
        float3 Lc = Lg.color_intensity.xyz;
        float I = Lg.color_intensity.w;
        float3 Ldir = float3(0, 0, 0);
        float att = 1.f;
        float shadow = 1.f;

        if (Lg.type == LIGHT_DIR)
        {
            Ldir = normalize(-Lg.direction_cosOuter.xyz);
            shadow = SampleShadowPCF(P, N, Ldir);
        }
        else if (Lg.type == LIGHT_POINT)
        {
            float3 toL = Lg.position_range.xyz - P;
            float dist = length(toL);
            if (dist > Lg.position_range.w)
                continue;
            Ldir = toL / max(dist, 1e-5);
            float t = 1.f - saturate(dist / Lg.position_range.w);
            att = t * t / max(dist * dist, 0.04);
        }
        else
        {
            float3 toL = Lg.position_range.xyz - P;
            float dist = length(toL);
            if (dist > Lg.position_range.w)
                continue;
            Ldir = toL / max(dist, 1e-5);
            float t = 1.f - saturate(dist / Lg.position_range.w);
            att = t * t / max(dist * dist, 0.04);
            float3 axis = normalize(Lg.direction_cosOuter.xyz);
            float rho = dot(-Ldir, axis);
            float cosO = Lg.direction_cosOuter.w;
            float cosI = Lg.spotCosInner;
            float spot = saturate((rho - cosO) / max(cosI - cosO, 1e-4));
            att *= spot * spot;
        }

        float nDotL = saturate(dot(N, Ldir));
        float3 H = normalize(Ldir + V);
        float3 fresnel = FresnelSchlick(saturate(dot(H, V)), f0);
        float distribution = DistributionGGX(N, H, roughness);
        float geometry = GeometrySmith(N, V, Ldir, roughness);
        float3 specular = distribution * geometry * fresnel
            / max(4.0 * nDotV * nDotL, 1e-4);
        float3 diffuseWeight = (1.0 - fresnel) * (1.0 - metallic);
        float3 radiance = Lc * I * att * shadow;
        directLighting += (diffuseWeight * alb / PI + specular) * radiance * nDotL;
    }

    // IBL ambient: the normal samples diffuse irradiance, while the reflected
    // view vector samples the specular environment. Rough surfaces blend the
    // reflection toward the low-frequency irradiance direction.
    float3 reflection = reflect(-V, N);
    float3 irradiance = SampleEnvironment(N);
    float3 reflectedEnvironment = SampleEnvironment(reflection);
    float3 prefilteredEnvironment = lerp(reflectedEnvironment, irradiance, roughness * roughness);
    float3 ambientFresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
    float3 ambientDiffuseWeight = (1.0 - ambientFresnel) * (1.0 - metallic);
    float2 environmentBrdf = float2(1.0 - roughness * 0.55, roughness * 0.04);
    float3 ambient = ambientDiffuseWeight * irradiance * alb
        + prefilteredEnvironment * (ambientFresnel * environmentBrdf.x + environmentBrdf.y);

    float3 color = directLighting + ambient * 0.45;
    color = color / (color + 1.0);
    color = pow(saturate(color), 1.0 / 2.2);
    return float4(color, 1.f);
}
