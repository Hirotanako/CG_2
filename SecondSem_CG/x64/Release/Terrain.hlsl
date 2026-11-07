// Terrain geometry pass. Height samples are decoded on the CPU as linear R16
// data, so no sRGB conversion is ever applied to elevations.
cbuffer TerrainFrame : register(b0)
{
    row_major float4x4 ViewProjection;
    float4 CameraAndMinHeight;
    float4 HeightRangeAndPad;
    float4 Padding[10];
};

struct VsIn
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 baseMaterial : COLOR0;
    float4 surfaceMaterial : COLOR1;
};

struct VsOut
{
    float4 clipPosition : SV_POSITION;
    float3 worldPosition : POSITION0;
    float3 worldNormal : NORMAL;
    float4 baseMaterial : COLOR0;
    float4 surfaceMaterial : COLOR1;
};

VsOut TerrainVS(VsIn input)
{
    VsOut output;
    output.clipPosition = mul(float4(input.position, 1.0), ViewProjection);
    output.worldPosition = input.position;
    output.worldNormal = input.normal;
    output.baseMaterial = input.baseMaterial;
    output.surfaceMaterial = input.surfaceMaterial;
    return output;
}

struct GBufferOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

GBufferOut TerrainPS(VsOut input)
{
    GBufferOut output;
    float slope = 1.0 - saturate(input.worldNormal.y);

    // Diffuse and colormap are baked into COLOR0 on the CPU. Convert the
    // authored sRGB colour to linear before it enters the lighting equations.
    float3 albedo = pow(saturate(input.baseMaterial.rgb), 2.2);
    float3 splat = saturate(input.surfaceMaterial.yzw);
    float splatSum = max(dot(splat, float3(1.0, 1.0, 1.0)), 1e-4);
    splat /= splatSum;
    const float3 splatTint =
        splat.x * float3(0.82, 0.76, 0.69) +
        splat.y * float3(0.90, 0.86, 0.72) +
        splat.z * float3(0.72, 0.88, 0.62);
    albedo *= lerp(float3(1.0, 1.0, 1.0), splatTint, 0.14 * input.baseMaterial.a);
    albedo = saturate((albedo - 0.16) * 1.10 + 0.16);
    output.albedo = float4(albedo, 0.0); // dielectric
    const float roughness = clamp(input.surfaceMaterial.x, 0.18, 1.0);
    output.normal = float4(normalize(input.worldNormal), lerp(roughness, roughness * 0.82, slope));
    output.position = float4(input.worldPosition, 1.0);
    return output;
}
