#include "PostProcessSystem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include <cwchar>
#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace
{
[[noreturn]] void PostFail(HRESULT hr, const wchar_t* operation)
{
    wchar_t message[160]{};
    swprintf_s(message, L"%s failed (HRESULT 0x%08X)", operation, static_cast<unsigned>(hr));
    MessageBoxW(nullptr, message, L"Post process", MB_OK | MB_ICONERROR);
    std::exit(static_cast<int>(hr));
}

void Check(HRESULT hr, const wchar_t* operation)
{
    if (FAILED(hr))
        PostFail(hr, operation);
}

void Compile(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& result)
{
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &result, &errors);
    if (FAILED(hr))
    {
        if (errors)
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        PostFail(hr, L"Post-process shader compilation");
    }
}

D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}
} // namespace

void PostProcessSystem::CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath)
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &srvRange};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = _countof(parameters);
    rootDesc.pParameters = parameters;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errors;
    Check(D3D12SerializeRootSignature(
        &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
        L"Post-process root signature serialization");
    Check(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)), L"Post-process root signature creation");

    ComPtr<ID3DBlob> vs, grayscalePs, blurPs;
    Compile(shaderPath, "PostProcessVS", "vs_5_0", vs);
    Compile(shaderPath, "GrayscalePS", "ps_5_0", grayscalePs);
    Compile(shaderPath, "BlurPS", "ps_5_0", blurPs);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {grayscalePs->GetBufferPointer(), grayscalePs->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_grayscalePso)),
        L"Grayscale pipeline creation");

    pso.PS = {blurPs->GetBufferPointer(), blurPs->GetBufferSize()};
    Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_blurPso)),
        L"Blur pipeline creation");
}

void PostProcessSystem::CreateTargets(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap)
{
    m_sceneColor.Reset();
    m_grayscaleColor.Reset();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = width;
    texture.Height = height;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = texture.Format;
    clear.Color[3] = 1.f;

    Check(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&m_sceneColor)), L"Scene post-process target creation");
    Check(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        &clear, IID_PPV_ARGS(&m_grayscaleColor)), L"Grayscale target creation");

    D3D12_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = texture.Format;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(m_sceneColor.Get(), &rtv, rtvHandle);
    rtvHandle.ptr += m_rtvIncrement;
    device->CreateRenderTargetView(m_grayscaleColor.Get(), &rtv, rtvHandle);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = texture.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = shaderVisibleSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(m_srvBase) * m_srvIncrement;
    device->CreateShaderResourceView(m_sceneColor.Get(), &srv, srvHandle);
    srvHandle.ptr += m_srvIncrement;
    device->CreateShaderResourceView(m_grayscaleColor.Get(), &srv, srvHandle);
}

void PostProcessSystem::Init(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    UINT srvBase,
    UINT srvDescriptorIncrement,
    const wchar_t* shaderPath)
{
    m_srvBase = srvBase;
    m_srvIncrement = srvDescriptorIncrement;
    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = 2;
    Check(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)),
        L"Post-process RTV heap creation");
    CreatePipeline(device, shaderPath);
    CreateTargets(device, width, height, shaderVisibleSrvHeap);
}

void PostProcessSystem::Resize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap)
{
    CreateTargets(device, width, height, shaderVisibleSrvHeap);
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcessSystem::BeginScene(ID3D12GraphicsCommandList* cmd)
{
    const D3D12_RESOURCE_BARRIER toRenderTarget = Transition(
        m_sceneColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &toRenderTarget);
    return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
}

void PostProcessSystem::Apply(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* shaderVisibleSrvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
    UINT width,
    UINT height)
{
    D3D12_RESOURCE_BARRIER grayscaleBarriers[2] = {
        Transition(m_sceneColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_grayscaleColor.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(_countof(grayscaleBarriers), grayscaleBarriers);

    ID3D12DescriptorHeap* heaps[] = {shaderVisibleSrvHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);

    const float constants[4] = {
        width > 0 ? 1.f / static_cast<float>(width) : 1.f,
        height > 0 ? 1.f / static_cast<float>(height) : 1.f,
        0.f, 0.f};
    cmd->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE srv = shaderVisibleSrvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(m_srvBase) * m_srvIncrement;
    cmd->SetGraphicsRootDescriptorTable(0, srv);
    D3D12_CPU_DESCRIPTOR_HANDLE grayscaleRtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    grayscaleRtv.ptr += m_rtvIncrement;
    cmd->OMSetRenderTargets(1, &grayscaleRtv, FALSE, nullptr);
    cmd->SetPipelineState(m_grayscalePso.Get());
    cmd->DrawInstanced(3, 1, 0, 0);

    const D3D12_RESOURCE_BARRIER grayscaleToSrv = Transition(
        m_grayscaleColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &grayscaleToSrv);

    srv = shaderVisibleSrvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<SIZE_T>(m_srvBase + 1u) * m_srvIncrement;
    cmd->SetGraphicsRootDescriptorTable(0, srv);
    cmd->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    cmd->SetPipelineState(m_blurPso.Get());
    cmd->DrawInstanced(3, 1, 0, 0);
}
