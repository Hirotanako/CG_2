// Debug: wireframe AABB узлов octree. Геометрия генерируется в VS (без vertex buffer).
// DrawInstanced(24, nodeCount) — 12 рёбер × 2 вершины на узел.

cbuffer OctreeVizCB : register(b0)
{
    row_major float4x4 ViewProj;
    float4 LineColor;
    uint NodeCount;
    uint3 _Pad;
};

struct OctreeNodeGpu
{
    float4 minW;
    float4 maxW;
};

StructuredBuffer<OctreeNodeGpu> gOctreeNodes : register(t0);

static const uint kEdgeEnds[24] =
{
    0, 1, 1, 2, 2, 3, 3, 0,
    4, 5, 5, 6, 6, 7, 7, 4,
    0, 4, 1, 5, 2, 6, 3, 7
};

float3 AabbCorner(float3 bmin, float3 bmax, uint cornerIndex)
{
    return float3(
        (cornerIndex & 1) ? bmax.x : bmin.x,
        (cornerIndex & 2) ? bmax.y : bmin.y,
        (cornerIndex & 4) ? bmax.z : bmin.z);
}

struct VsOut
{
    float4 pos : SV_POSITION;
    float4 color : COLOR0;
};

// Вся геометрия — в vertex shader: угол AABB → clip space, цвет из CB
VsOut OctreeWireVS(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    VsOut o;
    o.pos = float4(0, 0, 0, 0);
    o.color = LineColor;
    if (iid >= NodeCount)
        return o;

    OctreeNodeGpu node = gOctreeNodes[iid];
    float3 bmin = node.minW.xyz;
    float3 bmax = node.maxW.xyz;

    uint cornerIdx = kEdgeEnds[vid];
    float3 p = AabbCorner(bmin, bmax, cornerIdx);

    float4 clip = mul(float4(p, 1.0f), ViewProj);
    clip.y = -clip.y;
    o.pos = clip;
    return o;
}

// PS только выводит интерполированный цвет (cb0 не нужен в PS)
float4 OctreeWirePS(VsOut input) : SV_Target0
{
    return input.color;
}
