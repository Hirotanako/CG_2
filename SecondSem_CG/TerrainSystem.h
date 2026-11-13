#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// Height-map terrain renderer. The class owns the height data, quadtree,
// prebuilt meshes for every quadtree node and the small terrain-only pipeline.
class TerrainSystem
{
public:
    struct Stats
    {
        uint32_t submittedNodes = 0;
        uint32_t submittedTriangles = 0;
        uint32_t testedNodes = 0;
        uint32_t culledSubtrees = 0;
    };

    bool Init(
        ID3D12Device* device,
        const std::filesystem::path& heightMapDirectory,
        const wchar_t* shaderPath,
        std::wstring& error);

    void Draw(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& projection,
        const DirectX::XMMATRIX& viewProjectionNoJitter,
        const DirectX::XMMATRIX& previousViewProjectionNoJitter,
        uint32_t viewportHeight,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMMATRIX& cullingView,
        const DirectX::XMMATRIX& cullingProjection,
        const DirectX::XMFLOAT3& cullingCameraPosition);

    void ToggleWireframe() { m_wireframe = !m_wireframe; }
    void ToggleCulling() { m_culling = !m_culling; }
    bool WireframeEnabled() const { return m_wireframe; }
    bool CullingEnabled() const { return m_culling; }
    const Stats& LastStats() const { return m_stats; }

private:
    static constexpr uint32_t kFrameCount = 2;
    // Twice the previous tessellation density noticeably reduces polygonal
    // stair-stepping on distant and intermediate LODs without increasing the
    // maximum-detail triangle count (the quadtree becomes one level shallower).
    static constexpr uint32_t kPatchSegments = 32;

    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        // rgb = diffuse/colormap blend, a = displacement mask.
        DirectX::XMFLOAT4 baseMaterial;
        // x = roughness, yzw = RGB splat weights.
        DirectX::XMFLOAT4 surfaceMaterial;
    };

    struct Node
    {
        std::array<int32_t, 4> children{{-1, -1, -1, -1}};
        DirectX::XMFLOAT3 boundsMin{};
        DirectX::XMFLOAT3 boundsMax{};
        uint32_t sampleX = 0;
        uint32_t sampleY = 0;
        uint32_t sampleSpan = 0;
        uint32_t level = 0;
        uint32_t baseVertex = 0;
        uint32_t indexCount = 0;
        float worldSize = 0.0f;
        float geometricError = 0.0f;
    };

    struct Plane
    {
        DirectX::XMFLOAT3 normal{};
        float d = 0.0f;
    };

    struct alignas(256) FrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection{};
        DirectX::XMFLOAT4X4 viewProjectionNoJitter{};
        DirectX::XMFLOAT4X4 previousViewProjectionNoJitter{};
        DirectX::XMFLOAT4 cameraAndMinHeight{};
        DirectX::XMFLOAT4 heightRangeAndPad{};
        float padding[8]{};
    };

    bool LoadHighMaps(const std::filesystem::path& directory, std::wstring& error);
    void BuildQuadtreeAndMeshes();
    void BuildSharedIndices();
    int32_t BuildNode(uint32_t x, uint32_t y, uint32_t span, uint32_t level);
    void AppendNodeMesh(Node& node);
    float ComputeGeometricError(const Node& node) const;
    float HeightAt(uint32_t x, uint32_t y) const;
    DirectX::XMFLOAT3 NormalAt(uint32_t x, uint32_t y) const;
    void CreateGpuResources(ID3D12Device* device);
    bool CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath, std::wstring& error);
    std::array<Plane, 6> ExtractFrustum(const DirectX::XMMATRIX& viewProjection) const;
    bool IsOutsideFrustum(const Node& node, const std::array<Plane, 6>& planes) const;
    void SelectNode(
        int32_t nodeIndex,
        const DirectX::XMFLOAT3& camera,
        const DirectX::XMFLOAT3& cameraForward,
        float projectionScalePixels,
        const std::array<Plane, 6>& planes,
        std::vector<int32_t>& selection);

    uint32_t m_sampleCount = 0;
    uint32_t m_maxLevel = 0;
    float m_worldSize = 128.0f;
    float m_heightScale = 18.0f;
    float m_baseHeight = -4.0f;
    float m_minHeight = 0.0f;
    float m_maxHeight = 0.0f;
    std::vector<uint16_t> m_heights;
    std::vector<uint8_t> m_baseColors;
    std::vector<uint8_t> m_roughness;
    std::vector<uint8_t> m_masks;
    std::vector<uint8_t> m_splat;
    std::vector<Node> m_nodes;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_frameConstants[kFrameCount];
    uint8_t* m_frameConstantsMapped[kFrameCount]{};
    D3D12_VERTEX_BUFFER_VIEW m_vertexView{};
    D3D12_INDEX_BUFFER_VIEW m_indexView{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_solidPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_wirePipeline;
    bool m_wireframe = false;
    bool m_culling = true;
    Stats m_stats{};
};
