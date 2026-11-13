#include "FrustumDebugRenderer.h"

#include <cstring>

#include <d3dcompiler.h>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT kCbStride = 256;

bool Compile(
    const wchar_t* path,
    const char* entry,
    const char* target,
    ComPtr<ID3DBlob>& bytecode,
    std::wstring& error)
{
    ComPtr<ID3DBlob> messages;
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &messages);
    if (SUCCEEDED(hr))
        return true;
    error = L"Не удалось скомпилировать FrustumDebug.hlsl";
    if (messages && messages->GetBufferPointer())
    {
        const char* text = static_cast<const char*>(messages->GetBufferPointer());
        error += L" (см. вывод отладчика)";
        OutputDebugStringA(text);
    }
    return false;
}

D3D12_HEAP_PROPERTIES UploadHeap()
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    return heap;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
}

bool FrustumDebugRenderer::Init(ID3D12Device* device, const wchar_t* shaderPath, std::wstring& error)
{
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> rootErrors;
    if (FAILED(D3D12SerializeRootSignature(
            &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors)) ||
        FAILED(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature))))
    {
        error = L"Не удалось создать root signature визуализации frustum";
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    if (!Compile(shaderPath, "FrustumVS", "vs_5_0", vs, error) ||
        !Compile(shaderPath, "FrustumPS", "ps_5_0", ps, error))
        return false;

    const D3D12_INPUT_ELEMENT_DESC input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.InputLayout = {input, _countof(input)};
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline))))
    {
        error = L"Не удалось создать pipeline визуализации frustum";
        return false;
    }

    const auto heap = UploadHeap();
    const auto vbDesc = BufferDesc(sizeof(Vertex) * kVertexCount * kFrameCount);
    const auto cbDesc = BufferDesc(kCbStride * kFrameCount);
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_vertexUpload))) ||
        FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &cbDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&m_constantUpload))))
    {
        error = L"Не удалось создать upload buffers визуализации frustum";
        return false;
    }
    m_vertexUpload->Map(0, nullptr, reinterpret_cast<void**>(&m_verticesMapped));
    m_constantUpload->Map(0, nullptr, reinterpret_cast<void**>(&m_constantsMapped));
    return true;
}

void FrustumDebugRenderer::Draw(
    ID3D12GraphicsCommandList* commandList,
    uint32_t frameIndex,
    const XMMATRIX& observerViewProjection,
    const XMMATRIX& sourceViewProjection)
{
    if (!m_pipeline)
        return;
    frameIndex %= kFrameCount;

    const XMMATRIX inverseSource = XMMatrixInverse(nullptr, sourceViewProjection);
    XMFLOAT3 corners[8]{};
    uint32_t corner = 0;
    for (float z : {0.0f, 1.0f})
        for (float y : {-1.0f, 1.0f})
            for (float x : {-1.0f, 1.0f})
                XMStoreFloat3(&corners[corner++], XMVector3TransformCoord(XMVectorSet(x, y, z, 1.0f), inverseSource));

    // Corner order per plane: (-,-), (+,-), (-,+), (+,+).
    constexpr uint32_t edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    Vertex vertices[kVertexCount]{};
    const XMFLOAT4 nearColor{1.0f, 0.85f, 0.1f, 0.95f};
    const XMFLOAT4 farColor{1.0f, 0.25f, 0.05f, 0.90f};
    for (uint32_t edge = 0; edge < 12; ++edge)
    {
        const XMFLOAT4 color = edge < 4 ? nearColor : farColor;
        vertices[edge * 2 + 0] = {corners[edges[edge][0]], color};
        vertices[edge * 2 + 1] = {corners[edges[edge][1]], color};
    }
    const UINT64 vertexFrameBytes = sizeof(Vertex) * kVertexCount;
    std::memcpy(m_verticesMapped + frameIndex * vertexFrameBytes, vertices, sizeof(vertices));
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.viewProjection, observerViewProjection);
    std::memcpy(m_constantsMapped + frameIndex * kCbStride, &constants, sizeof(constants));

    D3D12_VERTEX_BUFFER_VIEW view{};
    view.BufferLocation = m_vertexUpload->GetGPUVirtualAddress() + frameIndex * vertexFrameBytes;
    view.SizeInBytes = static_cast<UINT>(vertexFrameBytes);
    view.StrideInBytes = sizeof(Vertex);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    commandList->SetPipelineState(m_pipeline.Get());
    commandList->SetGraphicsRootConstantBufferView(
        0, m_constantUpload->GetGPUVirtualAddress() + frameIndex * kCbStride);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    commandList->IASetVertexBuffers(0, 1, &view);
    commandList->DrawInstanced(kVertexCount, 1, 0, 0);
}
