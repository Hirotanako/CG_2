#pragma once

// AABB, frustum culling, octree и процедурные меши (куб / сфера) для сцены с тысячами объектов.

#include "ObjLoader.h"

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace Scene
{

struct Aabb
{
    DirectX::XMFLOAT3 min{0, 0, 0};
    DirectX::XMFLOAT3 max{0, 0, 0};

    void Expand(const DirectX::XMFLOAT3& p);
    void Merge(const Aabb& other);
    DirectX::XMVECTOR Center() const;
    DirectX::XMVECTOR Extents() const;
};

struct Frustum
{
    DirectX::BoundingFrustum bounds{};

    void FromViewAndProjection(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection);
    bool IntersectsAabb(const Aabb& box) const;
};

enum class PrimitiveKind : uint8_t
{
    Cube = 0,
    Sphere = 1,
};

struct SceneObject
{
    DirectX::XMFLOAT3 position{};
    float uniformScale = 1.0f;
    DirectX::XMFLOAT4 color{1, 1, 1, 1};
    PrimitiveKind kind = PrimitiveKind::Cube;
    Aabb localBounds{};
    Aabb worldBounds{};
};

struct ProceduralMesh
{
    std::vector<Obj::MeshVertex> vertices;
    std::vector<uint32_t> indices;
    Aabb localBounds{};
};

class Octree
{
public:
    static constexpr uint32_t kMaxObjectsPerLeaf = 8;
    static constexpr uint32_t kMaxDepth = 8;

    void Build(const std::vector<SceneObject>& objects, const Aabb& sceneBounds);
    void QueryVisible(
        const Frustum& frustum,
        const std::vector<SceneObject>& objects,
        const DirectX::XMFLOAT3& cameraPos,
        float maxDrawDistance,
        bool useDistanceCull,
        std::vector<uint32_t>& outIndices) const;
    bool Empty() const { return !m_root; }

private:
    struct Node
    {
        Aabb bounds{};
        std::vector<uint32_t> objectIndices;
        std::unique_ptr<Node> children[8]{};
        bool IsLeaf() const;
    };

    void Subdivide(Node& node, const std::vector<SceneObject>& objects, uint32_t depth);
    void QueryNode(
        const Node& node,
        const Frustum& frustum,
        const std::vector<SceneObject>& objects,
        const DirectX::XMFLOAT3& cameraPos,
        float maxDrawDistance,
        bool useDistanceCull,
        std::vector<uint32_t>& out) const;

    std::unique_ptr<Node> m_root;
};

Aabb TransformAabb(const Aabb& local, const DirectX::XMMATRIX& world);
void BuildUnitCube(ProceduralMesh& out);
void BuildUvSphere(ProceduralMesh& out, uint32_t stacks, uint32_t slices, float radius);
void ScatterCubesAndSpheres(
    std::vector<SceneObject>& objects,
    uint32_t cubeCount,
    uint32_t sphereCount,
    const Aabb& spawnRegion,
    uint32_t randomSeed);
void CollectVisibleObjects(
    const std::vector<SceneObject>& objects,
    const Frustum& frustum,
    bool useFrustumCulling,
    bool useOctree,
    const Octree& octree,
    const DirectX::XMFLOAT3& cameraPos,
    float maxDrawDistance,
    bool useDistanceCull,
    std::vector<uint32_t>& outVisible);

// Ближайшая точка AABB к камере; если она дальше maxDrawDistance — объект не рисуем.
bool IsWithinDrawDistance(const DirectX::XMFLOAT3& camera, const Aabb& box, float maxDrawDistance);

} // namespace Scene
