cbuffer FrameCB : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 ViewProj;
    float4 LightDir;
    float4 CameraPos;
    float4 Ambient;
    float4 LightDiffuse;
    float4 LightSpecular;
    float Time;
    float _PadFrame[11];
};

cbuffer MaterialCB : register(b1)
{
    float4 Kd;
    float4 Ks;
    float Ns;
    float HasDiffuseTexture;
    float2 UvScale;
    float2 UvScrollSpeed;
    float2 _PadMat0;
    float _PadMat1[48];
};

Texture2D DiffuseTex : register(t0);
SamplerState LinearSampler : register(s0);

struct VSInput
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 clipPos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 wPos = mul(float4(input.pos, 1.0f), World);
    o.worldPos = wPos.xyz;
    o.worldNormal = normalize(mul(float4(input.normal, 0.0f), World).xyz);
    o.clipPos = mul(wPos, ViewProj);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uvTiled = input.uv * UvScale + Time * UvScrollSpeed;

    float3 texRgb = float3(1, 1, 1);
    if (HasDiffuseTexture > 0.5f)
        texRgb = DiffuseTex.Sample(LinearSampler, uvTiled).rgb;

    float3 kdTint = Kd.rgb;
    if (HasDiffuseTexture > 0.5f && dot(kdTint, kdTint) < 1e-8f)
        kdTint = float3(1, 1, 1);
    float3 albedo = texRgb * kdTint;

    float3 N = normalize(input.worldNormal);
    float3 L = normalize(LightDir.xyz);
    float3 V = normalize(CameraPos.xyz - input.worldPos);
    float3 R = reflect(-L, N);

    float ndl = saturate(dot(N, L));
    float rv = saturate(dot(R, V));
    float shininess = clamp(Ns, 1.0f, 128.0f);
    float spec = pow(rv, shininess);

    float3 ambient = Ambient.rgb * albedo;
    float3 diffuse = LightDiffuse.rgb * albedo * ndl;
    float3 specular = LightSpecular.rgb * Ks.rgb * spec;

    return float4(ambient + diffuse + specular, 1.0f);
}
