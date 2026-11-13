#include "TemporalAA.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include <cstdlib>

using Microsoft::WRL::ComPtr;

namespace
{
void Check(HRESULT hr, const wchar_t* message)
{
    if (SUCCEEDED(hr)) return;
    MessageBoxW(nullptr, message, L"Temporal AA", MB_OK | MB_ICONERROR);
    std::exit(static_cast<int>(hr));
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

ComPtr<ID3DBlob> Compile(const wchar_t* path, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> result, errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &result, &errors);
    if (FAILED(hr) && errors)
        OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    Check(hr, L"Не удалось скомпилировать TemporalAA.hlsl");
    return result;
}
} // namespace

void TemporalAA::WriteSrv(ID3D12Device* device, ID3D12Resource* resource, ID3D12DescriptorHeap* heap, UINT slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(slot) * m_srvIncrement;
    device->CreateShaderResourceView(resource, &srv, handle);
}

void TemporalAA::CreateTargets(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap)
{
    m_current.Reset();
    m_history[0].Reset();
    m_history[1].Reset();
    m_width = width;
    m_height = height;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = desc.Format;
    clear.Color[3] = 1.0f;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_current)), L"TAA current target");
    for (UINT i = 0; i < 2; ++i)
        Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_history[i])), L"TAA history target");

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(m_current.Get(), nullptr, rtv);
    for (UINT i = 0; i < 2; ++i)
    {
        rtv.ptr += m_rtvIncrement;
        device->CreateRenderTargetView(m_history[i].Get(), nullptr, rtv);
    }
    WriteSrv(device, m_current.Get(), srvHeap, m_currentSrvSlot);
    WriteSrv(device, m_history[0].Get(), srvHeap, m_historySrvSlot[0]);
    WriteSrv(device, m_history[1].Get(), srvHeap, m_historySrvSlot[1]);
    m_historyRead = 0;
    Invalidate();
}

void TemporalAA::CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath)
{
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    D3D12_ROOT_PARAMETER params[5]{};
    for (UINT i = 0; i < 4; ++i)
    {
        ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors = 1;
        ranges[i].BaseShaderRegister = i;
        ranges[i].OffsetInDescriptorsFromTableStart = 0;
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable.NumDescriptorRanges = 1;
        params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[4].Constants.ShaderRegister = 0;
    params[4].Constants.Num32BitValues = 12;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = _countof(params);
    root.pParameters = params;
    root.NumStaticSamplers = 1;
    root.pStaticSamplers = &sampler;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized, errors;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), L"TAA root signature");
    Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature)), L"TAA root signature creation");

    const ComPtr<ID3DBlob> vs = Compile(shaderPath, "TemporalVS", "vs_5_0");
    const ComPtr<ID3DBlob> ps = Compile(shaderPath, "TemporalPS", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 2;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipeline)), L"TAA pipeline");
}

void TemporalAA::Init(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap,
    UINT currentSrvSlot, UINT historySrvSlot0, UINT historySrvSlot1, UINT srvIncrement, const wchar_t* shaderPath)
{
    m_currentSrvSlot = currentSrvSlot;
    m_historySrvSlot[0] = historySrvSlot0;
    m_historySrvSlot[1] = historySrvSlot1;
    m_srvIncrement = srvIncrement;
    m_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 3;
    Check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_rtvHeap)), L"TAA RTV heap");
    CreatePipeline(device, shaderPath);
    CreateTargets(device, width, height, srvHeap);
}

void TemporalAA::Resize(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap)
{
    if (width != m_width || height != m_height)
        CreateTargets(device, width, height, srvHeap);
}

D3D12_CPU_DESCRIPTOR_HANDLE TemporalAA::BeginCurrent(ID3D12GraphicsCommandList* cmd)
{
    const D3D12_RESOURCE_BARRIER barrier = Transition(
        m_current.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &barrier);
    return m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
}

void TemporalAA::Resolve(ID3D12GraphicsCommandList* cmd, ID3D12DescriptorHeap* srvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv, UINT motionSrvSlot, UINT depthSrvSlot,
    const DirectX::XMFLOAT2& currentJitterUv, const DirectX::XMFLOAT2& previousJitterUv)
{
    const UINT writeIndex = 1u - m_historyRead;
    D3D12_RESOURCE_BARRIER barriers[2] = {
        Transition(m_current.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_history[writeIndex].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    cmd->ResourceBarrier(2, barriers);
    D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {backbufferRtv, m_rtvHeap->GetCPUDescriptorHandleForHeapStart()};
    targets[1].ptr += static_cast<SIZE_T>(writeIndex + 1) * m_rtvIncrement;
    cmd->OMSetRenderTargets(2, targets, FALSE, nullptr);
    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    cmd->SetPipelineState(m_pipeline.Get());
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    auto gpu = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srvHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot) * m_srvIncrement;
        return handle;
    };
    cmd->SetGraphicsRootDescriptorTable(0, gpu(m_currentSrvSlot));
    cmd->SetGraphicsRootDescriptorTable(1, gpu(m_historySrvSlot[m_historyRead]));
    cmd->SetGraphicsRootDescriptorTable(2, gpu(motionSrvSlot));
    cmd->SetGraphicsRootDescriptorTable(3, gpu(depthSrvSlot));
    const float constants[12] = {
        1.0f / static_cast<float>(m_width), 1.0f / static_cast<float>(m_height),
        currentJitterUv.x, currentJitterUv.y, previousJitterUv.x, previousJitterUv.y,
        m_historyValid ? 1.0f : 0.0f, m_debugMotion ? 1.0f : 0.0f,
        0.90f, m_historyClamping ? 1.0f : 0.0f, 0.0f, 0.0f};
    cmd->SetGraphicsRoot32BitConstants(4, 12, constants, 0);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(3, 1, 0, 0);
    const D3D12_RESOURCE_BARRIER historyToSrv = Transition(
        m_history[writeIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &historyToSrv);
    m_historyRead = writeIndex;
    m_historyValid = true;
}
