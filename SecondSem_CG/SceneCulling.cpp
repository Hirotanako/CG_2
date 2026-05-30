#include "SceneCulling.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <random>

using namespace DirectX;

namespace Scene
{

namespace
{

float Rand01(std::mt19937& rng)
{
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

XMFLOAT3 RandPointInBox(std::mt19937& rng, const Aabb& region)
{
    return XMFLOAT3{
        std::lerp(region.min.x, region.max.x, Rand01(rng)),
        std::lerp(region.min.y, region.max.y, Rand01(rng)),
        std::lerp(region.min.z, region.max.z, Rand01(rng))};
}

void PushVertex(
    ProceduralMesh& mesh,
    const XMFLOAT3& p,
    const XMFLOAT3& n,
    const XMFLOAT2& uv,
    const XMFLOAT3& t,
    float tw)
{
    Obj::MeshVertex v{};
    v.px = p.x;
    v.py = p.y;
    v.pz = p.z;
    v.nx = n.x;
    v.ny = n.y;
    v.nz = n.z;
    v.u = uv.x;
    v.v = uv.y;
    v.tx = t.x;
    v.ty = t.y;
    v.tz = t.z;
    v.tw = tw;
    mesh.vertices.push_back(v);
    mesh.localBounds.Expand(p);
}

void AddTriangle(
    ProceduralMesh& mesh,
    const XMFLOAT3& a,
    const XMFLOAT3& b,
    const XMFLOAT3& c,
    const XMFLOAT2& uva,
    const XMFLOAT2& uvb,
    const XMFLOAT2& uvc)
{
    const XMVECTOR va = XMLoadFloat3(&a);
    const XMVECTOR vb = XMLoadFloat3(&b);
    const XMVECTOR vc = XMLoadFloat3(&c);
    XMVECTOR n = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(vb, va), XMVectorSubtract(vc, va)));
    XMFLOAT3 fn{};
    XMStoreFloat3(&fn, n);

    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    PushVertex(mesh, a, fn, uva, XMFLOAT3(1, 0, 0), 1.0f);
    PushVertex(mesh, b, fn, uvb, XMFLOAT3(1, 0, 0), 1.0f);
    PushVertex(mesh, c, fn, uvc, XMFLOAT3(1, 0, 0), 1.0f);
    mesh.indices.push_back(base);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
}

int ChildIndexForPoint(const Aabb& nodeBounds, const XMFLOAT3& p)
{
    const XMFLOAT3 center{
        (nodeBounds.min.x + nodeBounds.max.x) * 0.5f,
        (nodeBounds.min.y + nodeBounds.max.y) * 0.5f,
        (nodeBounds.min.z + nodeBounds.max.z) * 0.5f};

    int idx = 0;
    if (p.x >= center.x)
        idx |= 1;
    if (p.y >= center.y)
        idx |= 2;
    if (p.z >= center.z)
        idx |= 4;
    return idx;
}

Aabb ChildBounds(const Aabb& parent, int childIndex)
{
    const XMFLOAT3 center{
        (parent.min.x + parent.max.x) * 0.5f,
        (parent.min.y + parent.max.y) * 0.5f,
        (parent.min.z + parent.max.z) * 0.5f};

    Aabb child{};
    child.min = parent.min;
    child.max = parent.max;

    if (childIndex & 1)
        child.min.x = center.x;
    else
        child.max.x = center.x;

    if (childIndex & 2)
        child.min.y = center.y;
    else
        child.max.y = center.y;

    if (childIndex & 4)
        child.min.z = center.z;
    else
        child.max.z = center.z;

    return child;
}

} // namespace

void Aabb::Expand(const XMFLOAT3& p)
{
    min.x = (std::min)(min.x, p.x);
    min.y = (std::min)(min.y, p.y);
    min.z = (std::min)(min.z, p.z);
    max.x = (std::max)(max.x, p.x);
    max.y = (std::max)(max.y, p.y);
    max.z = (std::max)(max.z, p.z);
}

void Aabb::Merge(const Aabb& other)
{
    Expand(other.min);
    Expand(other.max);
}

XMVECTOR Aabb::Center() const
{
    return XMVectorSet(
        (min.x + max.x) * 0.5f,
        (min.y + max.y) * 0.5f,
        (min.z + max.z) * 0.5f,
        0.0f);
}

XMVECTOR Aabb::Extents() const
{
    return XMVectorSet(
        (max.x - min.x) * 0.5f,
        (max.y - min.y) * 0.5f,
        (max.z - min.z) * 0.5f,
        0.0f);
}

bool IsWithinDrawDistance(const XMFLOAT3& camera, const Aabb& box, float maxDrawDistance)
{
    const float cx = (std::clamp)(camera.x, box.min.x, box.max.x);
    const float cy = (std::clamp)(camera.y, box.min.y, box.max.y);
    const float cz = (std::clamp)(camera.z, box.min.z, box.max.z);
    const float dx = camera.x - cx;
    const float dy = camera.y - cy;
    const float dz = camera.z - cz;
    const float maxSq = maxDrawDistance * maxDrawDistance;
    return dx * dx + dy * dy + dz * dz <= maxSq;
}
bool AabbIntersects(const Aabb& a, const Aabb& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
        a.min.z <= b.max.z && a.max.z >= b.min.z;
}

void Frustum::FromViewAndProjection(const XMMATRIX& view, const XMMATRIX& projection)
{
    BoundingFrustum viewFrustum{};
    BoundingFrustum::CreateFromMatrix(viewFrustum, projection, false);

    XMVECTOR det{};
    const XMMATRIX invView = XMMatrixInverse(&det, view);
    viewFrustum.Transform(bounds, invView);
}

bool Frustum::IntersectsAabb(const Aabb& box) const
{
    const XMVECTOR c = XMVectorScale(XMVectorAdd(XMLoadFloat3(&box.min), XMLoadFloat3(&box.max)), 0.5f);
    const XMVECTOR e = XMVectorScale(XMVectorSubtract(XMLoadFloat3(&box.max), XMLoadFloat3(&box.min)), 0.5f);
    XMFLOAT3 center{};
    XMFLOAT3 extents{};
    XMStoreFloat3(&center, c);
    XMStoreFloat3(&extents, e);

    BoundingBox bb{};
    bb.Center = center;
    bb.Extents = extents;
    return bounds.Contains(bb) != DISJOINT;
}

Aabb TransformAabb(const Aabb& local, const XMMATRIX& world)
{
    const XMFLOAT3 corners[8] = {
        {local.min.x, local.min.y, local.min.z},
        {local.max.x, local.min.y, local.min.z},
        {local.min.x, local.max.y, local.min.z},
        {local.max.x, local.max.y, local.min.z},
        {local.min.x, local.min.y, local.max.z},
        {local.max.x, local.min.y, local.max.z},
        {local.min.x, local.max.y, local.max.z},
        {local.max.x, local.max.y, local.max.z},
    };

    Aabb out{};
    out.min = XMFLOAT3{FLT_MAX, FLT_MAX, FLT_MAX};
    out.max = XMFLOAT3{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (const XMFLOAT3& c : corners)
    {
        const XMVECTOR p = XMVector3TransformCoord(XMLoadFloat3(&c), world);
        XMFLOAT3 wp{};
        XMStoreFloat3(&wp, p);
        out.Expand(wp);
    }
    return out;
}

void BuildUnitCube(ProceduralMesh& out)
{
    out.vertices.clear();
    out.indices.clear();
    out.localBounds = Aabb{};
    out.localBounds.min = XMFLOAT3{FLT_MAX, FLT_MAX, FLT_MAX};
    out.localBounds.max = XMFLOAT3{-FLT_MAX, -FLT_MAX, -FLT_MAX};

    auto face = [&](
        XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2, XMFLOAT3 p3,
        XMFLOAT3 n,
        XMFLOAT3 tangent,
        float tw) {
        const uint32_t base = static_cast<uint32_t>(out.vertices.size());
        const XMFLOAT2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const XMFLOAT3 pts[4] = {p0, p1, p2, p3};
        for (int i = 0; i < 4; ++i)
        {
            PushVertex(out, pts[i], n, uv[i], tangent, tw);
            out.vertices.back().nx = n.x;
            out.vertices.back().ny = n.y;
            out.vertices.back().nz = n.z;
            out.vertices.back().tx = tangent.x;
            out.vertices.back().ty = tangent.y;
            out.vertices.back().tz = tangent.z;
            out.vertices.back().tw = tw;
        }
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
        out.indices.push_back(base);
        out.indices.push_back(base + 2);
        out.indices.push_back(base + 3);
    };

    const float h = 0.5f;
    face({-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}, {0, 0, 1}, {1, 0, 0}, 1.0f);
    face({h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}, {0, 0, -1}, {-1, 0, 0}, 1.0f);
    face({-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}, {0, 1, 0}, {1, 0, 0}, 1.0f);
    face({-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}, {0, -1, 0}, {1, 0, 0}, 1.0f);
    face({h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}, {1, 0, 0}, {0, 0, -1}, 1.0f);
    face({-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-1, 0, 0}, {0, 0, 1}, 1.0f);
}

void BuildUvSphere(ProceduralMesh& out, uint32_t stacks, uint32_t slices, float radius)
{
    out.vertices.clear();
    out.indices.clear();
    out.localBounds = Aabb{};
    out.localBounds.min = XMFLOAT3{-radius, -radius, -radius};
    out.localBounds.max = XMFLOAT3{radius, radius, radius};

    stacks = (std::max)(stacks, 3u);
    slices = (std::max)(slices, 3u);

    for (uint32_t i = 0; i <= stacks; ++i)
    {
        const float v = static_cast<float>(i) / static_cast<float>(stacks);
        const float phi = v * XM_PI;
        const float sinPhi = sinf(phi);
        const float cosPhi = cosf(phi);

        for (uint32_t j = 0; j <= slices; ++j)
        {
            const float u = static_cast<float>(j) / static_cast<float>(slices);
            const float theta = u * XM_2PI;
            const float sinTheta = sinf(theta);
            const float cosTheta = cosf(theta);

            const XMFLOAT3 n{sinPhi * cosTheta, cosPhi, sinPhi * sinTheta};
            const XMFLOAT3 p{n.x * radius, n.y * radius, n.z * radius};
            const XMFLOAT3 t{-sinTheta, 0.0f, cosTheta};
            PushVertex(out, p, n, XMFLOAT2(u, v), t, 1.0f);
        }
    }

    const uint32_t row = slices + 1;
    for (uint32_t i = 0; i < stacks; ++i)
    {
        for (uint32_t j = 0; j < slices; ++j)
        {
            const uint32_t i0 = i * row + j;
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + row;
            const uint32_t i3 = i2 + 1;
            out.indices.push_back(i0);
            out.indices.push_back(i2);
            out.indices.push_back(i1);
            out.indices.push_back(i1);
            out.indices.push_back(i2);
            out.indices.push_back(i3);
        }
    }
}

void ScatterCubesAndSpheres(
    std::vector<SceneObject>& objects,
    uint32_t cubeCount,
    uint32_t sphereCount,
    const Aabb& spawnRegion,
    uint32_t randomSeed)
{
    objects.clear();
    objects.reserve(static_cast<size_t>(cubeCount) + sphereCount);

    std::mt19937 rng(randomSeed);

    ProceduralMesh cubeProbe{};
    ProceduralMesh sphereProbe{};
    BuildUnitCube(cubeProbe);
    BuildUvSphere(sphereProbe, 12, 18, 0.5f);

    auto spawnOne = [&](PrimitiveKind kind) {
        SceneObject obj{};
        obj.kind = kind;
        obj.position = RandPointInBox(rng, spawnRegion);
        obj.uniformScale = std::lerp(0.35f, 1.15f, Rand01(rng));
        obj.color = XMFLOAT4(
            std::lerp(0.25f, 1.0f, Rand01(rng)),
            std::lerp(0.25f, 1.0f, Rand01(rng)),
            std::lerp(0.25f, 1.0f, Rand01(rng)),
            1.0f);
        obj.localBounds = kind == PrimitiveKind::Cube ? cubeProbe.localBounds : sphereProbe.localBounds;

        const XMMATRIX world = XMMatrixScaling(obj.uniformScale, obj.uniformScale, obj.uniformScale) *
            XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);
        obj.worldBounds = TransformAabb(obj.localBounds, world);
        objects.push_back(obj);
    };

    for (uint32_t i = 0; i < cubeCount; ++i)
        spawnOne(PrimitiveKind::Cube);
    for (uint32_t i = 0; i < sphereCount; ++i)
        spawnOne(PrimitiveKind::Sphere);
}

bool Octree::Node::IsLeaf() const
{
    for (const auto& child : children)
    {
        if (child)
            return false;
    }
    return true;
}

void Octree::Build(const std::vector<SceneObject>& objects, const Aabb& sceneBounds)
{
    m_root = std::make_unique<Node>();
    m_root->bounds = sceneBounds;

    for (uint32_t i = 0; i < objects.size(); ++i)
        m_root->objectIndices.push_back(i);

    Subdivide(*m_root, objects, 0);
}

void Octree::Subdivide(Node& node, const std::vector<SceneObject>& objects, uint32_t depth)
{
    if (depth >= kMaxDepth || node.objectIndices.size() <= kMaxObjectsPerLeaf)
        return;

    std::vector<uint32_t> buckets[8];
    for (uint32_t idx : node.objectIndices)
    {
        const Aabb& wb = objects[idx].worldBounds;
        for (int c = 0; c < 8; ++c)
        {
            const Aabb childBounds = ChildBounds(node.bounds, c);
            if (AabbIntersects(wb, childBounds))
                buckets[c].push_back(idx);
        }
    }

    bool anySplit = false;
    for (int c = 0; c < 8; ++c)
    {
        if (!buckets[c].empty())
        {
            anySplit = true;
            break;
        }
    }
    if (!anySplit)
        return;

    node.objectIndices.clear();
    for (int c = 0; c < 8; ++c)
    {
        if (buckets[c].empty())
            continue;

        node.children[c] = std::make_unique<Node>();
        node.children[c]->bounds = ChildBounds(node.bounds, c);
        node.children[c]->objectIndices = std::move(buckets[c]);
        Subdivide(*node.children[c], objects, depth + 1);
    }
}

void Octree::QueryVisible(
    const Frustum& frustum,
    const std::vector<SceneObject>& objects,
    const XMFLOAT3& cameraPos,
    float maxDrawDistance,
    bool useDistanceCull,
    std::vector<uint32_t>& outIndices) const
{
    outIndices.clear();
    if (m_root)
        QueryNode(*m_root, frustum, objects, cameraPos, maxDrawDistance, useDistanceCull, outIndices);

    std::sort(outIndices.begin(), outIndices.end());
    outIndices.erase(std::unique(outIndices.begin(), outIndices.end()), outIndices.end());
}

void Octree::QueryNode(
    const Node& node,
    const Frustum& frustum,
    const std::vector<SceneObject>& objects,
    const XMFLOAT3& cameraPos,
    float maxDrawDistance,
    bool useDistanceCull,
    std::vector<uint32_t>& out) const
{
    if (!frustum.IntersectsAabb(node.bounds))
        return;

    if (useDistanceCull && !IsWithinDrawDistance(cameraPos, node.bounds, maxDrawDistance))
        return;

    if (node.IsLeaf())
    {
        for (uint32_t idx : node.objectIndices)
        {
            const Aabb& wb = objects[idx].worldBounds;
            if (!frustum.IntersectsAabb(wb))
                continue;
            if (useDistanceCull && !IsWithinDrawDistance(cameraPos, wb, maxDrawDistance))
                continue;
            out.push_back(idx);
        }
        return;
    }

    for (const auto& child : node.children)
    {
        if (child)
            QueryNode(*child, frustum, objects, cameraPos, maxDrawDistance, useDistanceCull, out);
    }
}

void CollectVisibleObjects(
    const std::vector<SceneObject>& objects,
    const Frustum& frustum,
    bool useFrustumCulling,
    bool useOctree,
    const Octree& octree,
    const XMFLOAT3& cameraPos,
    float maxDrawDistance,
    bool useDistanceCull,
    std::vector<uint32_t>& outVisible)
{
    outVisible.clear();
    outVisible.reserve(objects.size());

    auto passesCull = [&](uint32_t i) -> bool {
        const Aabb& wb = objects[i].worldBounds;
        if (useFrustumCulling && !frustum.IntersectsAabb(wb))
            return false;
        if (useDistanceCull && !IsWithinDrawDistance(cameraPos, wb, maxDrawDistance))
            return false;
        return true;
    };

    if (!useFrustumCulling && !useDistanceCull)
    {
        for (uint32_t i = 0; i < objects.size(); ++i)
            outVisible.push_back(i);
        return;
    }

    if (useFrustumCulling && useOctree && !octree.Empty())
    {
        octree.QueryVisible(frustum, objects, cameraPos, maxDrawDistance, useDistanceCull, outVisible);
        return;
    }

    for (uint32_t i = 0; i < objects.size(); ++i)
    {
        if (passesCull(i))
            outVisible.push_back(i);
    }
}

} // namespace Scene
