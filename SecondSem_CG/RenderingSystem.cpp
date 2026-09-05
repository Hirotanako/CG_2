#include "RenderingSystem.h"
#include "ShadowSystem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3dcompiler.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{

enum LightType : UINT
{
    LIGHT_DIR = 0,
    LIGHT_POINT = 1,
    LIGHT_SPOT = 2,
};

struct LightGpu
{
    XMFLOAT4 position_range{};
    XMFLOAT4 direction_cosOuter{};
    XMFLOAT4 color_intensity{};
    UINT type = LIGHT_DIR;
    float spotCosInner = 0.f;
    UINT pad[2]{};
};

static_assert(sizeof(LightGpu) == 64);

struct LightingCBGPU
{
    XMFLOAT4 cameraPos_pad{};
    XMFLOAT4 invScreen_pad{};
    UINT lightCount = 0;
    UINT padHdr[3]{};
    LightGpu lights[8]{};
    float tailPad[52]{}; // до 768 байт (выравнивание CB D3D12)
};

static_assert(sizeof(LightingCBGPU) == 768);

void RSCompile(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &out, &err);
    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        wchar_t b[96];
        swprintf_s(b, L"Deferred shader compile failed 0x%08X", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, b, L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
}

static ComPtr<ID3D12Resource> CreateUploadCb(ID3D12Device* device, UINT64 size)
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
    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf));
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"Lighting CB", L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
    return buf;
}

} // namespace

void RenderingSystem::WriteDefaultLights()
{
    LightingCBGPU cb{};
    cb.lightCount = 3;

    XMVECTOR sund = XMVector3Normalize(XMVectorSet(0.42f, 0.72f, 0.52f, 0.f));
    cb.lights[0].type = LIGHT_DIR;
    XMStoreFloat4(&cb.lights[0].direction_cosOuter, sund);
    cb.lights[0].direction_cosOuter.w = 0.f;
    cb.lights[0].color_intensity = XMFLOAT4(1.f, 0.96f, 0.88f, 1.35f);
    XMStoreFloat3(&m_sunDirection, sund);

    cb.lights[1].type = LIGHT_POINT;
    cb.lights[1].position_range = XMFLOAT4(-2.5f, 3.2f, 1.0f, 10.0f);
    cb.lights[1].color_intensity = XMFLOAT4(1.0f, 0.22f, 0.08f, 32.0f);

    cb.lights[2].type = LIGHT_SPOT;
    cb.lights[2].position_range = XMFLOAT4(3.0f, 4.5f, -1.5f, 14.0f);
    cb.lights[2].direction_cosOuter = XMFLOAT4(-0.42f, -0.82f, 0.38f, 0.78f);
    cb.lights[2].spotCosInner = 0.91f;
    cb.lights[2].color_intensity = XMFLOAT4(0.16f, 0.42f, 1.0f, 52.0f);

    std::memcpy(m_lightingCBMapped, &cb, sizeof(cb));
}

void RenderingSystem::CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE environmentRange{};
    environmentRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environmentRange.NumDescriptors = 1;
    environmentRange.BaseShaderRegister = 4;
    environmentRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2;
    params[2].DescriptorTable.pDescriptorRanges = ranges;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &environmentRange;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[2].ShaderRegister = 2;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 4;
    rs.pParameters = params;
    rs.NumStaticSamplers = 3;
    rs.pStaticSamplers = samplers;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr);
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));
    hr = device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSigLight));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));

    ComPtr<ID3DBlob> vs, ps;
    RSCompile(hlslPath, "LightingFullscreenVS", "vs_5_0", vs);
    RSCompile(hlslPath, "LightingPS", "ps_5_0", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSigLight.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    pso.InputLayout.NumElements = 0;
    pso.InputLayout.pInputElementDescs = nullptr;

    hr = device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_psoLight));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));
}

void RenderingSystem::Init(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT gbufferSrvStartIndex,
    UINT shadowSrvStartIndex,
    UINT iblSrvIndex,
    UINT srvDescriptorIncrement,
    const wchar_t* deferredHlslPath)
{
    m_gbufferSrvBase = gbufferSrvStartIndex;
    m_shadowSrvBase = shadowSrvStartIndex;
    m_iblSrvIndex = iblSrvIndex;
    m_srvDescriptorIncrement = srvDescriptorIncrement;

    m_gbuffer.Init(device, width, height);
    m_gbuffer.CreateShaderResourceViews(
        device, shaderVisibleSrvHeap, gbufferSrvStartIndex, srvDescriptorIncrement);

    CreateLightingPipeline(device, deferredHlslPath);

    m_lightingCB = CreateUploadCb(device, sizeof(LightingCBGPU));
    D3D12_RANGE rr{0, 0};
    HRESULT hr = m_lightingCB->Map(0, &rr, reinterpret_cast<void**>(&m_lightingCBMapped));
    if (FAILED(hr))
        std::exit(static_cast<int>(hr));

    WriteDefaultLights();
}

void RenderingSystem::Resize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT srvDescriptorIncrement)
{
    m_srvDescriptorIncrement = srvDescriptorIncrement;
    m_gbuffer.Resize(device, width, height);
    m_gbuffer.CreateShaderResourceViews(
        device, shaderVisibleSrvHeap, m_gbufferSrvBase, srvDescriptorIncrement);
}

void RenderingSystem::UploadFrameConstants(const XMFLOAT3& cameraPos, UINT screenW, UINT screenH)
{
    auto* cb = reinterpret_cast<LightingCBGPU*>(m_lightingCBMapped);
    cb->cameraPos_pad = XMFLOAT4(cameraPos.x, cameraPos.y, cameraPos.z, 0.f);
    const float iw = screenW > 0 ? 1.f / static_cast<float>(screenW) : 1.f;
    const float ih = screenH > 0 ? 1.f / static_cast<float>(screenH) : 1.f;
    cb->invScreen_pad = XMFLOAT4(iw, ih, 0.f, 0.f);
}

void RenderingSystem::DrawLightingPass(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* srvHeapShaderVisible,
    ShadowSystem& shadows,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    UINT screenW,
    UINT screenH)
{
    ID3D12DescriptorHeap* heaps[] = {srvHeapShaderVisible};
    cmd->SetDescriptorHeaps(1, heaps);

    cmd->SetGraphicsRootSignature(m_rootSigLight.Get());
    cmd->SetPipelineState(m_psoLight.Get());

    cmd->SetGraphicsRootConstantBufferView(0, m_lightingCB->GetGPUVirtualAddress());
    cmd->SetGraphicsRootConstantBufferView(1, shadows.ShadowCBAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapShaderVisible->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<SIZE_T>(m_gbufferSrvBase) * static_cast<SIZE_T>(m_srvDescriptorIncrement);
    cmd->SetGraphicsRootDescriptorTable(2, table);

    D3D12_GPU_DESCRIPTOR_HANDLE environment =
        srvHeapShaderVisible->GetGPUDescriptorHandleForHeapStart();
    environment.ptr += static_cast<SIZE_T>(m_iblSrvIndex) * m_srvDescriptorIncrement;
    cmd->SetGraphicsRootDescriptorTable(3, environment);

    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(screenW);
    vp.Height = static_cast<float>(screenH);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sr{0, 0, static_cast<LONG>(screenW), static_cast<LONG>(screenH)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);

    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(3, 1, 0, 0);
}
