Texture2D CurrentColor : register(t0);
Texture2D HistoryColor : register(t1);
Texture2D<float2> MotionVectors : register(t2);
Texture2D<float> CurrentDepth : register(t3);
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

float3 SampleHistoryCatmullRom(float2 uv)
{
    // Nine bilinear taps implementing the 4x4 Catmull-Rom reconstruction from
    // the lecture. It preserves high-frequency detail noticeably better than a
    // single bilinear history lookup while keeping the sample count practical.
    const float2 textureSize = 1.0 / InvResolution;
    const float2 samplePosition = uv * textureSize;
    const float2 texelCenter1 = floor(samplePosition - 0.5) + 0.5;
    const float2 f = samplePosition - texelCenter1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / max(w12, 1e-5);

    const float2 texel0 = (texelCenter1 - 1.0) * InvResolution;
    const float2 texel3 = (texelCenter1 + 2.0) * InvResolution;
    const float2 texel12 = (texelCenter1 + offset12) * InvResolution;

    float3 result = 0.0;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel0.x, texel0.y), 0).rgb * w0.x * w0.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel12.x, texel0.y), 0).rgb * w12.x * w0.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel3.x, texel0.y), 0).rgb * w3.x * w0.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel0.x, texel12.y), 0).rgb * w0.x * w12.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel12.x, texel12.y), 0).rgb * w12.x * w12.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel3.x, texel12.y), 0).rgb * w3.x * w12.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel0.x, texel3.y), 0).rgb * w0.x * w3.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel12.x, texel3.y), 0).rgb * w12.x * w3.y;
    result += HistoryColor.SampleLevel(LinearClamp, float2(texel3.x, texel3.y), 0).rgb * w3.x * w3.y;
    return max(result, 0.0);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

TemporalOutput TemporalPS(FullscreenOutput input)
{
    TemporalOutput output;
    const int2 size = int2(round(1.0 / InvResolution));
    const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), size - 1);
    const float3 current = CurrentColor.Load(int3(pixel, 0)).rgb;

    // Depth dilation from the lecture: at geometry silhouettes use the motion
    // of the closest surface in a 3x3 area instead of a background vector that
    // may leak through a sub-pixel edge. This depth buffer uses conventional
    // DirectX depth, so the nearest sample has the smallest value.
    int2 motionPixel = pixel;
    float closestDepth = CurrentDepth.Load(int3(pixel, 0));
    [unroll] for (int depthY = -1; depthY <= 1; ++depthY)
    [unroll] for (int depthX = -1; depthX <= 1; ++depthX)
    {
        const int2 samplePixel = clamp(pixel + int2(depthX, depthY), int2(0, 0), size - 1);
        const float sampleDepth = CurrentDepth.Load(int3(samplePixel, 0));
        if (sampleDepth < closestDepth)
        {
            closestDepth = sampleDepth;
            motionPixel = samplePixel;
        }
    }
    const float2 motion = MotionVectors.Load(int3(motionPixel, 0));

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

    // The history texture stores the already resolved image on the stable
    // output grid. Jitter supplies a different sub-pixel sample for the same
    // output pixel and must not spatially shift that resolved history again.
    // The old PreviousJitter-CurrentJitter term made the output follow the
    // sampling pattern and was the source of the visible full-screen shake.
    const float2 historyUv = input.uv - motion;
    const bool onScreen = all(historyUv >= 0.0) && all(historyUv <= 1.0);
    float3 result = current;
    if (HistoryValid > 0.5 && onScreen)
    {
        const float3 rawHistory = SampleHistoryCatmullRom(historyUv);
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
        float motionConfidence = exp2(-0.12 * velocityPixels);
        if (velocityPixels > 32.0)
            motionConfidence = 0.0;

        const float historyBlend = HistoryWeight * motionConfidence * colorConfidence;
        // Inverse-luminance weighting from the lecture prevents a bright
        // outlier from dominating several following frames and reduces flicker.
        const float currentWeight = (1.0 - historyBlend) / (1.0 + Luminance(current));
        const float historyWeight = historyBlend / (1.0 + Luminance(history));
        result = (current * currentWeight + history * historyWeight) /
            max(currentWeight + historyWeight, 1e-5);
    }
    output.display = output.history = float4(result, 1.0);
    return output;
}
