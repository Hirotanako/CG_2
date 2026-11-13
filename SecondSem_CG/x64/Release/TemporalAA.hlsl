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
    float HistoryClamping;
    float2 Padding;
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
        const float3 rawHistory = HistoryColor.SampleLevel(LinearClamp, historyUv, 0).rgb;
        float3 history = rawHistory;
        float colorConfidence = 1.0;
        // H toggles this protection for the required A/B ghosting test.
        // Keeping reprojection active in both modes isolates the effect of the
        // 3x3 neighborhood constraint itself.
        if (HistoryClamping > 0.5)
        {
            history = clamp(history, neighborhoodMin, neighborhoodMax);

            // A newly revealed background pixel often reprojects into the old
            // foreground object. Merely clamping that old colour still leaves
            // a visible silhouette, so measure how far the sample had to be
            // clipped and reject history when it does not belong to the current
            // 3x3 distribution. The small floor keeps flat dark regions stable.
            const float3 neighborhoodExtent =
                max(neighborhoodMax - neighborhoodMin, 2.0 / 255.0);
            const float3 outsideDistance =
                abs(rawHistory - history) / (neighborhoodExtent + 0.02);
            const float3 centerDistance =
                abs(history - current) / (neighborhoodExtent + 0.05);
            const float disocclusion = saturate(max(
                max(outsideDistance.r, max(outsideDistance.g, outsideDistance.b)),
                0.45 * max(centerDistance.r, max(centerDistance.g, centerDistance.b))));
            colorConfidence = 1.0 - disocclusion;
        }

        // Motion is stored in UV units. Convert it to pixels before choosing
        // the temporal weight; the old UV-space multiplier barely reacted even
        // to several pixels of movement and caused long object afterimages.
        const float velocityPixels = length(motion / InvResolution);
        float motionConfidence = exp2(-0.35 * velocityPixels);
        if (velocityPixels > 24.0)
            motionConfidence = 0.0;

        const float historyBlend = HistoryWeight * motionConfidence * colorConfidence;
        result = lerp(current, history, historyBlend);
    }
    output.display = output.history = float4(result, 1.0);
    return output;
}
