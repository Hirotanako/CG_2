#include "TerrainSystem.h"
#include "TextureUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
template <class T>
ComPtr<ID3D12Resource> MakeUploadBuffer(ID3D12Device* device, const T* data, size_t count)
{
    const UINT64 byteSize = static_cast<UINT64>((std::max)(size_t{1}, count * sizeof(T)));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&result))))
        return {};
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    if (FAILED(result->Map(0, &noRead, &mapped)))
        return {};
    if (data && count)
        std::memcpy(mapped, data, count * sizeof(T));
    else
        std::memset(mapped, 0, static_cast<size_t>(byteSize));
    result->Unmap(0, nullptr);
    return result;
}

bool Compile(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& blob, std::wstring& error)
{
    ComPtr<ID3DBlob> messages;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &blob, &messages);
    if (SUCCEEDED(hr))
        return true;
    if (messages)
    {
        const char* text = static_cast<const char*>(messages->GetBufferPointer());
        const int chars = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (chars > 0)
        {
            std::vector<wchar_t> converted(static_cast<size_t>(chars));
            MultiByteToWideChar(CP_UTF8, 0, text, -1, converted.data(), chars);
            error.assign(converted.data());
        }
    }
    if (error.empty())
        error = L"Не удалось скомпилировать Terrain.hlsl";
    return false;
}
} // namespace

bool TerrainSystem::LoadHighMaps(const std::filesystem::path& directory, std::wstring& error)
{
    struct Image
    {
        std::vector<uint8_t> pixels;
        UINT width = 0;
        UINT height = 0;
    };
    auto load = [&](const wchar_t* name, Image& image) {
        std::wstring decodeError;
        if (!Tex::LoadImageRgba8(directory / name, image.pixels, image.width, image.height, decodeError))
        {
            error = L"High/" + std::wstring(name) + L": " + decodeError;
            return false;
        }
        return true;
    };

    Image height, colormap, diffuse, displacement, mask, roughness, splat;
    if (!load(L"New Project Height Output-1025.png", height) ||
        !load(L"New Project Colormap only 1025.png", colormap) ||
        !load(L"New Project Material Output 1025_diffuse.png", diffuse) ||
        !load(L"New Project Material Output 1025_disp.png", displacement) ||
        !load(L"New Project Material Output 1025_mask.png", mask) ||
        !load(L"New Project Material Output 1025_roughness.png", roughness) ||
        !load(L"New Project Splatmap 1025.png", splat))
        return false;

    const Image* maps[] = {&colormap, &diffuse, &displacement, &mask, &roughness, &splat};
    if (height.width != height.height || height.width < kPatchSegments + 1)
    {
        error = L"Height Output должен быть квадратным и достаточно большим";
        return false;
    }
    for (const Image* image : maps)
        if (image->width != height.width || image->height != height.height)
        {
            error = L"Все карты в High должны иметь одинаковое разрешение";
            return false;
        }

    m_sampleCount = height.width;
    const uint32_t totalSegments = m_sampleCount - 1;
    if ((totalSegments & (totalSegments - 1)) != 0 || totalSegments < kPatchSegments)
    {
        error = L"Размер High heightmap должен быть 2^N + 1";
        return false;
    }
    m_maxLevel = 0;
    for (uint32_t span = totalSegments; span > kPatchSegments; span >>= 1)
        ++m_maxLevel;

    const size_t pixelCount = static_cast<size_t>(m_sampleCount) * m_sampleCount;
    m_heights.resize(pixelCount);
    m_baseColors.resize(pixelCount * 4);
    m_roughness.resize(pixelCount);
    m_masks.resize(pixelCount);
    m_splat.resize(pixelCount * 3);
    auto gray = [](const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) * 54u + static_cast<uint32_t>(p[1]) * 183u
            + static_cast<uint32_t>(p[2]) * 19u) >> 8u;
    };
    for (size_t i = 0; i < pixelCount; ++i)
    {
        const uint8_t* h = height.pixels.data() + i * 4;
        const uint8_t* d = displacement.pixels.data() + i * 4;
        const uint8_t* mk = mask.pixels.data() + i * 4;
        const uint8_t* cm = colormap.pixels.data() + i * 4;
        const uint8_t* df = diffuse.pixels.data() + i * 4;
        const uint8_t* ro = roughness.pixels.data() + i * 4;
        const uint8_t* sp = splat.pixels.data() + i * 4;
        const float maskValue = static_cast<float>(gray(mk)) / 255.0f;
        const float macroHeight = static_cast<float>(gray(h)) / 255.0f;
        const float detailHeight = static_cast<float>(gray(d)) / 255.0f;
        const float combinedHeight = (std::clamp)(
            macroHeight + detailHeight * maskValue * 0.055f, 0.0f, 1.0f);
        m_heights[i] = static_cast<uint16_t>(combinedHeight * 65535.0f + 0.5f);
        for (size_t channel = 0; channel < 3; ++channel)
        {
            m_baseColors[i * 4 + channel] = static_cast<uint8_t>(
                static_cast<float>(df[channel]) * 0.72f + static_cast<float>(cm[channel]) * 0.28f + 0.5f);
            m_splat[i * 3 + channel] = sp[channel];
        }
        m_baseColors[i * 4 + 3] = static_cast<uint8_t>(maskValue * 255.0f + 0.5f);
        m_masks[i] = m_baseColors[i * 4 + 3];
        m_roughness[i] = static_cast<uint8_t>(gray(ro));
    }

    m_worldSize = 128.0f;
    m_heightScale = 22.0f;
    m_baseHeight = -11.0f;
    auto [lo, hi] = std::minmax_element(m_heights.begin(), m_heights.end());
    m_minHeight = m_baseHeight + (static_cast<float>(*lo) / 65535.0f) * m_heightScale;
    m_maxHeight = m_baseHeight + (static_cast<float>(*hi) / 65535.0f) * m_heightScale;
    return true;
}

float TerrainSystem::HeightAt(uint32_t x, uint32_t y) const
{
    x = (std::min)(x, m_sampleCount - 1);
    y = (std::min)(y, m_sampleCount - 1);
    return m_baseHeight + (static_cast<float>(m_heights[static_cast<size_t>(y) * m_sampleCount + x]) / 65535.0f) * m_heightScale;
}

XMFLOAT3 TerrainSystem::NormalAt(uint32_t x, uint32_t y) const
{
    const uint32_t xm = x > 0 ? x - 1 : x;
    const uint32_t xp = (std::min)(x + 1, m_sampleCount - 1);
    const uint32_t ym = y > 0 ? y - 1 : y;
    const uint32_t yp = (std::min)(y + 1, m_sampleCount - 1);
    const float cell = m_worldSize / static_cast<float>(m_sampleCount - 1);
    const float dx = HeightAt(xp, y) - HeightAt(xm, y);
    const float dz = HeightAt(x, yp) - HeightAt(x, ym);
    XMVECTOR n = XMVector3Normalize(XMVectorSet(-dx, cell * static_cast<float>(xp - xm), -dz, 0.0f));
    XMFLOAT3 result{};
    XMStoreFloat3(&result, n);
    return result;
}

void TerrainSystem::AppendNodeMesh(Node& node)
{
    const uint32_t firstVertex = static_cast<uint32_t>(m_vertices.size());
    const uint32_t step = node.sampleSpan / kPatchSegments;
    const float cell = m_worldSize / static_cast<float>(m_sampleCount - 1);
    const float half = m_worldSize * 0.5f;
    auto appendVertex = [&](uint32_t sx, uint32_t sy, float yOffset) {
        const float h = HeightAt(sx, sy) + yOffset;
        const size_t sample = static_cast<size_t>(sy) * m_sampleCount + sx;
        const float inv255 = 1.0f / 255.0f;
        m_vertices.push_back({
            {static_cast<float>(sx) * cell - half, h, static_cast<float>(sy) * cell - half},
            NormalAt(sx, sy),
            {static_cast<float>(sx) / static_cast<float>(m_sampleCount - 1),
             static_cast<float>(sy) / static_cast<float>(m_sampleCount - 1)},
            {m_baseColors[sample * 4 + 0] * inv255,
             m_baseColors[sample * 4 + 1] * inv255,
             m_baseColors[sample * 4 + 2] * inv255,
             m_masks[sample] * inv255},
            {m_roughness[sample] * inv255,
             m_splat[sample * 3 + 0] * inv255,
             m_splat[sample * 3 + 1] * inv255,
             m_splat[sample * 3 + 2] * inv255}});
    };

    for (uint32_t y = 0; y <= kPatchSegments; ++y)
        for (uint32_t x = 0; x <= kPatchSegments; ++x)
            appendVertex(node.sampleX + x * step, node.sampleY + y * step, 0.0f);

    node.firstIndex = static_cast<uint32_t>(m_indices.size());
    const uint32_t row = kPatchSegments + 1;
    for (uint32_t y = 0; y < kPatchSegments; ++y)
    {
        for (uint32_t x = 0; x < kPatchSegments; ++x)
        {
            const uint32_t a = firstVertex + y * row + x;
            const uint32_t b = a + row;
            m_indices.insert(m_indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }

    // Four independent edge strips. Their top vertices reference the regular
    // grid and the duplicated lower vertices form a vertical crack-covering skirt.
    const float skirtDepth = (std::max)(2.0f, m_heightScale * 0.12f);
    auto addSkirt = [&](auto gridIndex, auto samplePosition) {
        const uint32_t lowerStart = static_cast<uint32_t>(m_vertices.size());
        for (uint32_t i = 0; i <= kPatchSegments; ++i)
        {
            const auto [sx, sy] = samplePosition(i);
            appendVertex(sx, sy, -skirtDepth);
        }
        for (uint32_t i = 0; i < kPatchSegments; ++i)
        {
            const uint32_t top0 = firstVertex + gridIndex(i);
            const uint32_t top1 = firstVertex + gridIndex(i + 1);
            const uint32_t low0 = lowerStart + i;
            const uint32_t low1 = low0 + 1;
            m_indices.insert(m_indices.end(), {top0, low0, top1, top1, low0, low1});
        }
    };
    addSkirt([&](uint32_t i) { return i; }, [&](uint32_t i) { return std::pair{node.sampleX + i * step, node.sampleY}; });
    addSkirt([&](uint32_t i) { return i * row + kPatchSegments; }, [&](uint32_t i) { return std::pair{node.sampleX + node.sampleSpan, node.sampleY + i * step}; });
    addSkirt([&](uint32_t i) { return kPatchSegments * row + (kPatchSegments - i); }, [&](uint32_t i) { return std::pair{node.sampleX + node.sampleSpan - i * step, node.sampleY + node.sampleSpan}; });
    addSkirt([&](uint32_t i) { return (kPatchSegments - i) * row; }, [&](uint32_t i) { return std::pair{node.sampleX, node.sampleY + node.sampleSpan - i * step}; });

    node.indexCount = static_cast<uint32_t>(m_indices.size()) - node.firstIndex;
}

int32_t TerrainSystem::BuildNode(uint32_t x, uint32_t y, uint32_t span, uint32_t level)
{
    const int32_t index = static_cast<int32_t>(m_nodes.size());
    m_nodes.emplace_back();
    Node& node = m_nodes.back();
    node.sampleX = x;
    node.sampleY = y;
    node.sampleSpan = span;
    node.level = level;
    node.worldSize = m_worldSize * static_cast<float>(span) / static_cast<float>(m_sampleCount - 1);

    float lo = (std::numeric_limits<float>::max)();
    float hi = -(std::numeric_limits<float>::max)();
    for (uint32_t sy = y; sy <= y + span; ++sy)
        for (uint32_t sx = x; sx <= x + span; ++sx)
        {
            const float h = HeightAt(sx, sy);
            lo = (std::min)(lo, h);
            hi = (std::max)(hi, h);
        }
    const float cell = m_worldSize / static_cast<float>(m_sampleCount - 1);
    const float halfWorld = m_worldSize * 0.5f;
    node.boundsMin = {x * cell - halfWorld, lo - (std::max)(2.0f, m_heightScale * 0.12f), y * cell - halfWorld};
    node.boundsMax = {(x + span) * cell - halfWorld, hi, (y + span) * cell - halfWorld};
    AppendNodeMesh(node);

    if (level < m_maxLevel)
    {
        const uint32_t half = span / 2;
        // Building children can reallocate m_nodes, so write through the index.
        const std::array<int32_t, 4> children = {
            BuildNode(x, y, half, level + 1),
            BuildNode(x + half, y, half, level + 1),
            BuildNode(x, y + half, half, level + 1),
            BuildNode(x + half, y + half, half, level + 1)};
        m_nodes[static_cast<size_t>(index)].children = children;
    }
    return index;
}

void TerrainSystem::BuildQuadtreeAndMeshes()
{
    m_nodes.clear();
    m_vertices.clear();
    m_indices.clear();
    BuildNode(0, 0, m_sampleCount - 1, 0);
}

void TerrainSystem::CreateGpuResources(ID3D12Device* device)
{
    m_vertexBuffer = MakeUploadBuffer(device, m_vertices.data(), m_vertices.size());
    m_indexBuffer = MakeUploadBuffer(device, m_indices.data(), m_indices.size());
    m_vertexView = {m_vertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(m_vertices.size() * sizeof(Vertex)), sizeof(Vertex)};
    m_indexView = {m_indexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(m_indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        m_frameConstants[i] = MakeUploadBuffer<FrameConstants>(device, nullptr, 1);
        D3D12_RANGE noRead{0, 0};
        m_frameConstants[i]->Map(0, &noRead, reinterpret_cast<void**>(&m_frameConstantsMapped[i]));
    }
}

bool TerrainSystem::CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath, std::wstring& error)
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized, messages;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &messages)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature))))
    {
        error = L"Не удалось создать root signature ландшафта";
        return false;
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!Compile(shaderPath, "TerrainVS", "vs_5_0", vs, error) || !Compile(shaderPath, "TerrainPS", "ps_5_0", ps, error))
        return false;
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = m_rootSignature.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    for (uint32_t i = 0; i < 3; ++i)
        desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // skirts are intentionally two-sided
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 3;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.InputLayout = {layout, _countof(layout)};
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_solidPipeline))))
    {
        error = L"Не удалось создать terrain PSO";
        return false;
    }
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_wirePipeline))))
    {
        error = L"Не удалось создать wireframe terrain PSO";
        return false;
    }
    return true;
}

bool TerrainSystem::Init(ID3D12Device* device, const std::filesystem::path& highDirectory, const wchar_t* shaderPath, std::wstring& error)
{
    if (!LoadHighMaps(highDirectory, error))
        return false;
    BuildQuadtreeAndMeshes();
    CreateGpuResources(device);
    if (!m_vertexBuffer || !m_indexBuffer)
    {
        error = L"Не удалось создать GPU-буферы ландшафта";
        return false;
    }
    return CreatePipeline(device, shaderPath, error);
}

std::array<TerrainSystem::Plane, 6> TerrainSystem::ExtractFrustum(const XMMATRIX& viewProjection) const
{
    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, viewProjection);
    std::array<Plane, 6> result{{
        {{m._14 + m._11, m._24 + m._21, m._34 + m._31}, m._44 + m._41},
        {{m._14 - m._11, m._24 - m._21, m._34 - m._31}, m._44 - m._41},
        {{m._14 + m._12, m._24 + m._22, m._34 + m._32}, m._44 + m._42},
        {{m._14 - m._12, m._24 - m._22, m._34 - m._32}, m._44 - m._42},
        {{m._13, m._23, m._33}, m._43},
        {{m._14 - m._13, m._24 - m._23, m._34 - m._33}, m._44 - m._43},
    }};
    for (Plane& plane : result)
    {
        const float length = std::sqrt(plane.normal.x * plane.normal.x + plane.normal.y * plane.normal.y + plane.normal.z * plane.normal.z);
        if (length > 1e-6f)
        {
            plane.normal.x /= length;
            plane.normal.y /= length;
            plane.normal.z /= length;
            plane.d /= length;
        }
    }
    return result;
}

bool TerrainSystem::IsOutsideFrustum(const Node& node, const std::array<Plane, 6>& planes) const
{
    for (const Plane& p : planes)
    {
        // Farthest AABB point along the plane normal. If even this point is
        // outside, the complete node (and therefore its subtree) is invisible.
        const XMFLOAT3 positive{
            p.normal.x >= 0.0f ? node.boundsMax.x : node.boundsMin.x,
            p.normal.y >= 0.0f ? node.boundsMax.y : node.boundsMin.y,
            p.normal.z >= 0.0f ? node.boundsMax.z : node.boundsMin.z};
        if (p.normal.x * positive.x + p.normal.y * positive.y + p.normal.z * positive.z + p.d < 0.0f)
            return true;
    }
    return false;
}

void TerrainSystem::SelectNode(int32_t nodeIndex, const XMFLOAT3& camera, const std::array<Plane, 6>& planes, std::vector<int32_t>& selection)
{
    const Node& node = m_nodes[static_cast<size_t>(nodeIndex)];
    ++m_stats.testedNodes;
    if (m_culling && IsOutsideFrustum(node, planes))
    {
        ++m_stats.culledSubtrees;
        return;
    }
    const float closestX = (std::clamp)(camera.x, node.boundsMin.x, node.boundsMax.x);
    const float closestZ = (std::clamp)(camera.z, node.boundsMin.z, node.boundsMax.z);
    const float dx = camera.x - closestX;
    const float dz = camera.z - closestZ;
    const float distance = std::sqrt(dx * dx + dz * dz);
    const bool hasChildren = node.children[0] >= 0;
    const bool split = hasChildren && distance < node.worldSize * 1.65f;
    if (split)
    {
        for (int32_t child : node.children)
            SelectNode(child, camera, planes, selection);
    }
    else
    {
        selection.push_back(nodeIndex);
    }
}

void TerrainSystem::Draw(ID3D12GraphicsCommandList* commandList, uint32_t frameIndex, const XMMATRIX& view, const XMMATRIX& projection, const XMFLOAT3& cameraPosition)
{
    if (!m_rootSignature || m_nodes.empty())
        return;
    frameIndex %= kFrameCount;
    const XMMATRIX viewProjection = view * projection;
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.viewProjection, viewProjection);
    constants.cameraAndMinHeight = {cameraPosition.x, cameraPosition.y, cameraPosition.z, m_minHeight};
    constants.heightRangeAndPad = {(std::max)(0.001f, m_maxHeight - m_minHeight), 0.0f, 0.0f, 0.0f};
    std::memcpy(m_frameConstantsMapped[frameIndex], &constants, sizeof(constants));

    m_stats = {};
    std::vector<int32_t> selection;
    selection.reserve(256);
    const auto planes = ExtractFrustum(viewProjection);
    SelectNode(0, cameraPosition, planes, selection);

    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->SetPipelineState(m_wireframe ? m_wirePipeline.Get() : m_solidPipeline.Get());
    commandList->SetGraphicsRootConstantBufferView(0, m_frameConstants[frameIndex]->GetGPUVirtualAddress());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexView);
    commandList->IASetIndexBuffer(&m_indexView);
    for (int32_t index : selection)
    {
        const Node& node = m_nodes[static_cast<size_t>(index)];
        commandList->DrawIndexedInstanced(node.indexCount, 1, node.firstIndex, 0, 0);
        ++m_stats.submittedNodes;
        m_stats.submittedTriangles += node.indexCount / 3;
    }
}
