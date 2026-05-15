// Минимальный пайплайн: цветной треугольник с интерполяцией цвета по вершинам.

struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.pos = float4(input.pos, 1.0f);
    o.color = input.color;
    return o;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
