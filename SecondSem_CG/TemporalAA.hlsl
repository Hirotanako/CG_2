Texture2D CurrentColor : register(t0);
Texture2D HistoryColor : register(t1);
Texture2D<float2> MotionVectors : register(t2);
SamplerState LinearClamp : register(s0);

cbuffer TemporalConstants : register(b0)
{
    float2 InvResolution;
    float2 CurrentJitterUv;
    float2 PreviousJitterUv;
    float HistoryValid;
    float DebugMotion;
    float HistoryWeight;
    float3 Padding;
};

struct FullscreenOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

FullscreenOutput TemporalVS(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}

struct TemporalOutput { float4 display : SV_Target0; float4 history : SV_Target1; };

TemporalOutput TemporalPS(FullscreenOutput input)
{
    TemporalOutput output;
    const int2 size = int2(round(1.0 / InvResolution));
    const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), size - 1);
    const float3 current = CurrentColor.Load(int3(pixel, 0)).rgb;
    const float2 motion = MotionVectors.Load(int3(pixel, 0));

    if (DebugMotion > 0.5)
    {
        const float3 visual = float3(motion * 24.0 + 0.5, saturate(length(motion) * 48.0));
        output.display = output.history = float4(visual, 1.0);
        return output;
    }

    float3 neighborhoodMin = current;
    float3 neighborhoodMax = current;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const int2 samplePixel = clamp(pixel + int2(x, y), int2(0, 0), size - 1);
        const float3 sampleColor = CurrentColor.Load(int3(samplePixel, 0)).rgb;
        neighborhoodMin = min(neighborhoodMin, sampleColor);
        neighborhoodMax = max(neighborhoodMax, sampleColor);
    }

    const float2 historyUv = input.uv - motion + PreviousJitterUv - CurrentJitterUv;
    const bool onScreen = all(historyUv >= 0.0) && all(historyUv <= 1.0);
    float3 result = current;
    if (HistoryValid > 0.5 && onScreen)
    {
        float3 history = HistoryColor.SampleLevel(LinearClamp, historyUv, 0).rgb;
        history = clamp(history, neighborhoodMin, neighborhoodMax);
        const float motionRejection = saturate(length(motion) * 12.0);
        result = lerp(current, history, HistoryWeight * (1.0 - motionRejection * 0.65));
    }
    output.display = output.history = float4(result, 1.0);
    return output;
}
