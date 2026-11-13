cbuffer FrameConstants : register(b0)
{
    row_major float4x4 viewProjection;
};

struct VsIn
{
    float3 position : POSITION;
    float4 color : COLOR;
};

struct VsOut
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VsOut FrustumVS(VsIn input)
{
    VsOut output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    return output;
}

float4 FrustumPS(VsOut input) : SV_TARGET
{
    return input.color;
}
