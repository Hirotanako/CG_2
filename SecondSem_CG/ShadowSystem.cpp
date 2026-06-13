#include "ShadowSystem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{

struct ShadowCBGPU
{
    XMFLOAT4X4 lightViewProj[ShadowSystem::kCascadeCount]{};
    XMFLOAT4X4 cameraView{};
    XMFLOAT4 cascadeSplits{};
    XMFLOAT4 shadowParams{};
    float _pad[40]{};
};

static_assert(sizeof(ShadowCBGPU) == 512);

void SSCompile(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &out, &err);
    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        std::exit(static_cast<int>(hr));
    }
}

ComPtr<ID3D12Resource> CreateUploadCb(ID3D12Device* device, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> buf;
    if (FAILED(device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf))))
        std::exit(static_cast<int>(E_FAIL));
    return buf;
}

void ComputeSplitDistances(float nearPlane, float farPlane, float lambda, float* splits, UINT count)
{
    for (UINT i = 0; i < count; ++i)
    {
        const float p = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logSplit = nearPlane * powf(farPlane / nearPlane, p);
        const float uniSplit = nearPlane + (farPlane - nearPlane) * p;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniSplit;
    }
}

std::array<XMFLOAT3, 8> FrustumCornersWorld(const XMMATRIX& invViewProj)
{
    static const XMFLOAT3 ndcCorners[8] = {
        {-1.f, -1.f, 0.f}, {1.f, -1.f, 0.f}, {1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f},
        {-1.f, -1.f, 1.f}, {1.f, -1.f, 1.f}, {1.f, 1.f, 1.f}, {-1.f, 1.f, 1.f},
    };

    std::array<XMFLOAT3, 8> world{};
    for (UINT i = 0; i < 8; ++i)
    {
        const XMVECTOR p = XMVectorSet(ndcCorners[i].x, ndcCorners[i].y, ndcCorners[i].z, 1.f);
        XMVECTOR w = XMVector4Transform(p, invViewProj);
        w = XMVectorScale(w, 1.f / XMVectorGetW(w));
        XMStoreFloat3(&world[i], w);
    }
    return world;
}

float ExtractAspect(const XMMATRIX& cameraProj)
{
    XMFLOAT4X4 m{};
    XMStoreFloat4x4(&m, cameraProj);
    if (fabsf(m._11) > 1e-6f && fabsf(m._22) > 1e-6f)
        return (1.f / m._11) / (1.f / m._22);
    return 16.f / 9.f;
}

} // namespace

void ShadowSystem::CreateResources(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap)
{
    D3D12_HEAP_PROPERTIES hpDefault{};
    hpDefault.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kMapSize;
    texDesc.Height = kMapSize;
    texDesc.DepthOrArraySize = ShadowSystem::kCascadeCount;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearVal{};
    clearVal.Format = DXGI_FORMAT_D32_FLOAT;
    clearVal.DepthStencil.Depth = 1.0f;

    if (FAILED(device->CreateCommittedResource(
            &hpDefault, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal,
            IID_PPV_ARGS(&m_shadowMap))))
        std::exit(static_cast<int>(E_FAIL));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = ShadowSystem::kCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap))))
        std::exit(static_cast<int>(E_FAIL));

    m_dsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT c = 0; c < ShadowSystem::kCascadeCount; ++c)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = c;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.MipSlice = 0;
        device->CreateDepthStencilView(m_shadowMap.Get(), &dsv, dsvHandle);
        dsvHandle.ptr += m_dsvDescriptorSize;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(m_shadowSrvSlot) * static_cast<SIZE_T>(m_srvIncrement);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2DArray.MostDetailedMip = 0;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.FirstArraySlice = 0;
    srv.Texture2DArray.ArraySize = ShadowSystem::kCascadeCount;
    device->CreateShaderResourceView(m_shadowMap.Get(), &srv, srvCpu);

    m_shadowCB = CreateUploadCb(device, sizeof(ShadowCBGPU));
    D3D12_RANGE rr{0, 0};
    if (FAILED(m_shadowCB->Map(0, &rr, reinterpret_cast<void**>(&m_shadowCBMapped))))
        std::exit(static_cast<int>(E_FAIL));
    std::memset(m_shadowCBMapped, 0, sizeof(ShadowCBGPU));
}

void ShadowSystem::CreatePipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &param;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr)))
        std::exit(static_cast<int>(E_FAIL));
    if (FAILED(device->CreateRootSignature(
            0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig))))
        std::exit(static_cast<int>(E_FAIL));

    ComPtr<ID3DBlob> vs;
    SSCompile(hlslPath, "ShadowVS", "vs_5_0", vs);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSig.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.RasterizerState.DepthBias = 25000;
    pso.RasterizerState.DepthBiasClamp = 0.0f;
    pso.RasterizerState.SlopeScaledDepthBias = 2.5f;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 0;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    pso.InputLayout = {layout, _countof(layout)};

    if (FAILED(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoShadow))))
        std::exit(static_cast<int>(E_FAIL));
}

void ShadowSystem::Init(
    ID3D12Device* device,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT shadowSrvSlot,
    UINT srvDescriptorIncrement,
    const wchar_t* deferredHlslPath)
{
    m_shadowSrvSlot = shadowSrvSlot;
    m_srvIncrement = srvDescriptorIncrement;
    CreateResources(device, shaderVisibleSrvHeap);
    CreatePipeline(device, deferredHlslPath);
}

XMMATRIX ShadowSystem::ComputeCascadeMatrix(
    const XMMATRIX& cameraView,
    const XMMATRIX& cameraProj,
    float splitNear,
    float splitFar,
    const XMVECTOR& lightDir,
    const XMVECTOR& /*sceneCenter*/) const
{
    const float aspect = ExtractAspect(cameraProj);
    const XMMATRIX cascadeProj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, splitNear, splitFar);
    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, cascadeProj * cameraView);

    const auto corners = FrustumCornersWorld(invViewProj);

    XMVECTOR center = XMVectorZero();
    for (const XMFLOAT3& c : corners)
        center = XMVectorAdd(center, XMLoadFloat3(&c));
    center = XMVectorScale(center, 1.f / 8.f);

    const XMVECTOR lightDirN = XMVector3Normalize(lightDir);
    const XMVECTOR lightPos = XMVectorSubtract(center, XMVectorScale(lightDirN, 80.f));
    const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    const XMMATRIX lightView = XMMatrixLookAtLH(lightPos, center, up);

    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    float minZ = FLT_MAX, maxZ = -FLT_MAX;

    for (const XMFLOAT3& c : corners)
    {
        const XMVECTOR ls = XMVector3TransformCoord(XMLoadFloat3(&c), lightView);
        XMFLOAT3 lsF{};
        XMStoreFloat3(&lsF, ls);
        minX = (std::min)(minX, lsF.x);
        maxX = (std::max)(maxX, lsF.x);
        minY = (std::min)(minY, lsF.y);
        maxY = (std::max)(maxY, lsF.y);
        minZ = (std::min)(minZ, lsF.z);
        maxZ = (std::max)(maxZ, lsF.z);
    }

    const float margin = 6.f;
    minX -= margin;
    maxX += margin;
    minY -= margin;
    maxY += margin;
    minZ -= 25.f;
    maxZ += 25.f;

    const float worldUnitsPerTexelX = (maxX - minX) / static_cast<float>(kMapSize);
    const float worldUnitsPerTexelY = (maxY - minY) / static_cast<float>(kMapSize);
    minX = floorf(minX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
    maxX = floorf(maxX / worldUnitsPerTexelX) * worldUnitsPerTexelX;
    minY = floorf(minY / worldUnitsPerTexelY) * worldUnitsPerTexelY;
    maxY = floorf(maxY / worldUnitsPerTexelY) * worldUnitsPerTexelY;

    const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);
    return lightView * lightProj;
}

void ShadowSystem::UpdateCascades(
    const XMMATRIX& cameraView,
    const XMMATRIX& cameraProj,
    const XMFLOAT3& /*cameraPos*/,
    const XMFLOAT3& lightDir,
    const XMFLOAT3& sceneCenter,
    float /*sceneRadius*/)
{
    auto* cb = reinterpret_cast<ShadowCBGPU*>(m_shadowCBMapped);

    float splits[ShadowSystem::kCascadeCount]{};
    ComputeSplitDistances(kCameraNear, kCameraFar, kSplitLambda, splits, ShadowSystem::kCascadeCount);

    const XMVECTOR lightDirV = XMVector3Normalize(XMLoadFloat3(&lightDir));
    const XMVECTOR sceneCenterV = XMLoadFloat3(&sceneCenter);

    float prevSplit = kCameraNear;
    for (UINT i = 0; i < ShadowSystem::kCascadeCount; ++i)
    {
        const XMMATRIX cascade = ComputeCascadeMatrix(
            cameraView, cameraProj, prevSplit, splits[i], lightDirV, sceneCenterV);
        XMStoreFloat4x4(&cb->lightViewProj[i], cascade);
        prevSplit = splits[i];
    }

    XMStoreFloat4x4(&cb->cameraView, cameraView);
    cb->cascadeSplits = XMFLOAT4(splits[0], splits[1], splits[2], splits[3]);
    cb->shadowParams = XMFLOAT4(
        1.f / static_cast<float>(kMapSize),
        kShadowBias,
        kNormalBias,
        kSlopeScale);
}

void ShadowSystem::DrawShadowPass(
    ID3D12GraphicsCommandList* cmd,
    const std::function<void(const XMMATRIX& lightViewProj)>& drawScene)
{
    if (m_shadowIsSrv)
    {
        D3D12_RESOURCE_BARRIER toDepth = {};
        toDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDepth.Transition.pResource = m_shadowMap.Get();
        toDepth.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        toDepth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &toDepth);
    }

    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    cmd->SetPipelineState(m_psoShadow.Get());

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(kMapSize);
    vp.Height = static_cast<float>(kMapSize);
    vp.MaxDepth = 1.f;
    D3D12_RECT sr{0, 0, static_cast<LONG>(kMapSize), static_cast<LONG>(kMapSize)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);

    const auto* cb = reinterpret_cast<const ShadowCBGPU*>(m_shadowCBMapped);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvBase = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT c = 0; c < ShadowSystem::kCascadeCount; ++c)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsvBase;
        dsv.ptr += static_cast<SIZE_T>(c) * m_dsvDescriptorSize;

        cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);

        const XMMATRIX lightViewProj = XMLoadFloat4x4(&cb->lightViewProj[c]);
        drawScene(lightViewProj);
    }
}

void ShadowSystem::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER toSrv = {};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = m_shadowMap.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &toSrv);
    m_shadowIsSrv = true;
}

D3D12_GPU_DESCRIPTOR_HANDLE ShadowSystem::ShadowSrvGpu(ID3D12DescriptorHeap* srvHeap) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_shadowSrvSlot) * static_cast<SIZE_T>(m_srvIncrement);
    return h;
}

D3D12_GPU_VIRTUAL_ADDRESS ShadowSystem::ShadowCBAddress() const
{
    return m_shadowCB ? m_shadowCB->GetGPUVirtualAddress() : 0;
}
