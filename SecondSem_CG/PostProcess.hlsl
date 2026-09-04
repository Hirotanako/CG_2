Texture2D InputTexture : register(t0);
SamplerState LinearClampSampler : register(s0);

cbuffer PostProcessConstants : register(b0)
{
    float2 TexelSize;
    float2 Padding;
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

float4 GrayscalePS(FullscreenOutput input) : SV_Target0
{
    float3 color = InputTexture.Sample(LinearClampSampler, input.uv).rgb;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    return float4(luminance.xxx, 1.0);
}

float4 BlurPS(FullscreenOutput input) : SV_Target0
{
    // A visibly wide 9x9 Gaussian. Sampling every two texels gives an
    // effective radius of eight pixels while bilinear filtering fills the
    // gaps between samples.
    static const float weights[9] =
    {
        1.0 / 256.0, 8.0 / 256.0, 28.0 / 256.0, 56.0 / 256.0,
        70.0 / 256.0,
        56.0 / 256.0, 28.0 / 256.0, 8.0 / 256.0, 1.0 / 256.0
    };

    float3 color = 0.0;
    [unroll]
    for (int y = -4; y <= 4; ++y)
    {
        [unroll]
        for (int x = -4; x <= 4; ++x)
        {
            const float2 offset = float2(x, y) * TexelSize * 2.0;
            color += InputTexture.Sample(LinearClampSampler, input.uv + offset).rgb
                * weights[x + 4] * weights[y + 4];
        }
    }
    return float4(color, 1.0);
}
