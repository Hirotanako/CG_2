#include "ParticleSystem.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
struct ParticleGpu
{
    XMFLOAT3 position;
    float age;
    XMFLOAT3 velocity;
    float lifetime;
    XMFLOAT4 color;
};
static_assert(sizeof(ParticleGpu) == 48);

struct alignas(256) ParticleConstantsGpu
{
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 cameraRight;
    XMFLOAT4 cameraUp;
    XMFLOAT4 cameraPosition;
    XMFLOAT4 emitterAndDeltaTime;
    XMFLOAT4 timeAndSize;
    float padding[28]{};
};
static_assert(sizeof(ParticleConstantsGpu) == 256);

[[noreturn]] void ParticleFail(HRESULT hr, const wchar_t* operation)
{
    wchar_t text[160]{};
    swprintf_s(text, L"%s failed (HRESULT 0x%08X)", operation, static_cast<unsigned>(hr));
    MessageBoxW(nullptr, text, L"Particle system", MB_OK | MB_ICONERROR);
    std::exit(static_cast<int>(hr));
}

void Check(HRESULT hr, const wchar_t* operation)
{
    if (FAILED(hr))
        ParticleFail(hr, operation);
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
        ParticleFail(hr, L"Particle shader compilation");
    }
}

ComPtr<ID3D12Resource> MakeBuffer(
    ID3D12Device* device,
    UINT64 bytes,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&result)),
        L"Particle buffer creation");
    return result;
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

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

float Hash01(UINT value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return static_cast<float>(value & 0x00ffffffu) / 16777216.0f;
}
} // namespace

void ParticleSystem::CreatePipelines(ID3D12Device* device, const wchar_t* shaderPath)
{
    D3D12_DESCRIPTOR_RANGE inputRange{};
    inputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    inputRange.NumDescriptors = 1;
    inputRange.BaseShaderRegister = 0;
    inputRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE outputRange = inputRange;
    outputRange.BaseShaderRegister = 1;

    D3D12_ROOT_PARAMETER computeParameters[3]{};
    computeParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    computeParameters[0].Descriptor.ShaderRegister = 0;
    computeParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[1].DescriptorTable = {1, &inputRange};
    computeParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    computeParameters[2].DescriptorTable = {1, &outputRange};
    computeParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC computeDesc{};
    computeDesc.NumParameters = _countof(computeParameters);
    computeDesc.pParameters = computeParameters;

    ComPtr<ID3DBlob> serialized, errors;
    Check(D3D12SerializeRootSignature(
        &computeDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), L"Compute root signature serialization");
    Check(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_computeRootSignature)), L"Compute root signature creation");

    ComPtr<ID3DBlob> cs;
    Compile(shaderPath, "ParticleUpdateCS", "cs_5_0", cs);
    D3D12_COMPUTE_PIPELINE_STATE_DESC computePso{};
    computePso.pRootSignature = m_computeRootSignature.Get();
    computePso.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    Check(device->CreateComputePipelineState(&computePso, IID_PPV_ARGS(&m_computePso)),
        L"Particle compute pipeline creation");

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER renderParameters[2]{};
    renderParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    renderParameters[0].Descriptor.ShaderRegister = 0;
    renderParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    renderParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    renderParameters[1].DescriptorTable = {1, &srvRange};
    renderParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC renderDesc{};
    renderDesc.NumParameters = _countof(renderParameters);
    renderDesc.pParameters = renderParameters;
    renderDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serialized.Reset();
    Check(D3D12SerializeRootSignature(
        &renderDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors), L"Render root signature serialization");
    Check(device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&m_renderRootSignature)), L"Render root signature creation");

    ComPtr<ID3DBlob> vs, gs, ps;
    Compile(shaderPath, "ParticleVS", "vs_5_0", vs);
    Compile(shaderPath, "ParticleGS", "gs_5_0", gs);
    Compile(shaderPath, "ParticlePS", "ps_5_0", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC renderPso{};
    renderPso.pRootSignature = m_renderRootSignature.Get();
    renderPso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    renderPso.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    renderPso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    for (UINT i = 0; i < 3; ++i)
        renderPso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    renderPso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    renderPso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    renderPso.RasterizerState.DepthClipEnable = TRUE;
    renderPso.DepthStencilState.DepthEnable = TRUE;
    renderPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    renderPso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    renderPso.SampleMask = UINT_MAX;
    renderPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    renderPso.NumRenderTargets = 3;
    renderPso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    renderPso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    renderPso.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    renderPso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    renderPso.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&renderPso, IID_PPV_ARGS(&m_renderPso)),
        L"Particle render pipeline creation");
}

void ParticleSystem::CreateBuffers(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmd, const XMFLOAT3& emitterPosition)
{
    const UINT64 particleBytes = static_cast<UINT64>(kParticleCount) * sizeof(ParticleGpu);
    for (UINT i = 0; i < 2; ++i)
    {
        m_particles[i] = MakeBuffer(device, particleBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        m_counters[i] = MakeBuffer(device, sizeof(UINT), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    std::vector<ParticleGpu> particles(kParticleCount);
    for (UINT i = 0; i < kParticleCount; ++i)
    {
        const float angle = Hash01(i * 3u + 1u) * XM_2PI;
        const float radius = std::sqrt(Hash01(i * 3u + 2u)) * 0.22f;
        const float speed = 0.85f + Hash01(i * 3u + 3u) * 0.75f;
        const float lifetime = 2.4f + Hash01(i + 131u) * 1.2f;
        const float age = Hash01(i + 91u) * lifetime;
        constexpr float accelerationMagnitude = 0.18f;
        ParticleGpu& p = particles[i];
        p.position = XMFLOAT3(
            emitterPosition.x - std::cos(angle) * (radius + 0.12f * age),
            emitterPosition.y - speed * age - 0.5f * accelerationMagnitude * age * age,
            emitterPosition.z - std::sin(angle) * (radius + 0.12f * age));
        p.age = age;
        p.velocity = XMFLOAT3(
            -std::cos(angle) * 0.12f, -speed - accelerationMagnitude * age,
            -std::sin(angle) * 0.12f);
        p.lifetime = lifetime;
        const float tint = Hash01(i + 171u);
        p.color = XMFLOAT4(1.0f, 0.25f + tint * 0.45f, 0.04f, 1.0f);
    }

    m_initialParticlesUpload = MakeBuffer(device, particleBytes, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    D3D12_RANGE noRead{0, 0};
    Check(m_initialParticlesUpload->Map(0, &noRead, &mapped), L"Particle upload map");
    std::memcpy(mapped, particles.data(), static_cast<size_t>(particleBytes));
    m_initialParticlesUpload->Unmap(0, nullptr);

    const UINT initialCounters[2] = {kParticleCount, 0};
    m_initialCountersUpload = MakeBuffer(device, sizeof(initialCounters), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    Check(m_initialCountersUpload->Map(0, &noRead, &mapped), L"Counter upload map");
    std::memcpy(mapped, initialCounters, sizeof(initialCounters));
    m_initialCountersUpload->Unmap(0, nullptr);

    cmd->CopyBufferRegion(m_particles[0].Get(), 0, m_initialParticlesUpload.Get(), 0, particleBytes);
    cmd->CopyBufferRegion(m_counters[0].Get(), 0, m_initialCountersUpload.Get(), 0, sizeof(UINT));
    cmd->CopyBufferRegion(m_counters[1].Get(), 0, m_initialCountersUpload.Get(), sizeof(UINT), sizeof(UINT));
    D3D12_RESOURCE_BARRIER barriers[4] = {
        Transition(m_particles[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(m_particles[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(m_counters[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(m_counters[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    cmd->ResourceBarrier(_countof(barriers), barriers);

    for (UINT frame = 0; frame < kFrameCount; ++frame)
    {
        m_constants[frame] = MakeBuffer(device, sizeof(ParticleConstantsGpu), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        Check(m_constants[frame]->Map(
            0, &noRead, reinterpret_cast<void**>(&m_constantsMapped[frame])), L"Particle constants map");
    }
}

void ParticleSystem::CreateDescriptors(ID3D12Device* device, ID3D12DescriptorHeap* heap)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE start = heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = start;
        handle.ptr += static_cast<SIZE_T>(m_descriptorBase + i) * m_descriptorIncrement;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.Buffer.NumElements = kParticleCount;
        uav.Buffer.StructureByteStride = sizeof(ParticleGpu);
        uav.Buffer.CounterOffsetInBytes = 0;
        device->CreateUnorderedAccessView(m_particles[i].Get(), m_counters[i].Get(), &uav, handle);

        handle = start;
        handle.ptr += static_cast<SIZE_T>(m_descriptorBase + 2 + i) * m_descriptorIncrement;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements = kParticleCount;
        srv.Buffer.StructureByteStride = sizeof(ParticleGpu);
        device->CreateShaderResourceView(m_particles[i].Get(), &srv, handle);
    }
}

void ParticleSystem::Init(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* shaderVisibleHeap,
    UINT descriptorBase,
    UINT descriptorIncrement,
    const wchar_t* shaderPath,
    const XMFLOAT3& emitterPosition)
{
    m_descriptorBase = descriptorBase;
    m_descriptorIncrement = descriptorIncrement;
    CreatePipelines(device, shaderPath);
    CreateBuffers(device, cmd, emitterPosition);
    CreateDescriptors(device, shaderVisibleHeap);
}

void ParticleSystem::UpdateAndDraw(
    ID3D12GraphicsCommandList* cmd,
    ID3D12DescriptorHeap* shaderVisibleHeap,
    UINT frameIndex,
    float deltaTime,
    float totalTime,
    const XMMATRIX& view,
    const XMMATRIX& viewProj,
    const XMFLOAT3& cameraPosition,
    const XMFLOAT3& emitterPosition,
    float floorHeight)
{
    frameIndex %= kFrameCount;
    ParticleConstantsGpu constants{};
    XMStoreFloat4x4(&constants.viewProjection, viewProj);
    XMFLOAT4X4 cameraWorld{};
    XMStoreFloat4x4(&cameraWorld, XMMatrixInverse(nullptr, view));
    constants.cameraRight = XMFLOAT4(cameraWorld._11, cameraWorld._12, cameraWorld._13, 0.0f);
    constants.cameraUp = XMFLOAT4(cameraWorld._21, cameraWorld._22, cameraWorld._23, 0.0f);
    constants.cameraPosition = XMFLOAT4(cameraPosition.x, cameraPosition.y, cameraPosition.z, 1.0f);
    constants.emitterAndDeltaTime = XMFLOAT4(
        emitterPosition.x, emitterPosition.y, emitterPosition.z, deltaTime);
    constants.timeAndSize = XMFLOAT4(totalTime, 0.075f, floorHeight, 0.18f);
    std::memcpy(m_constantsMapped[frameIndex], &constants, sizeof(constants));

    const UINT outputBuffer = 1u - m_currentBuffer;
    if (!m_firstUpdate)
    {
        const D3D12_RESOURCE_BARRIER toUav = Transition(
            m_particles[m_currentBuffer].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->ResourceBarrier(1, &toUav);
    }

    ID3D12DescriptorHeap* heaps[] = {shaderVisibleHeap};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_computeRootSignature.Get());
    cmd->SetPipelineState(m_computePso.Get());
    cmd->SetComputeRootConstantBufferView(0, m_constants[frameIndex]->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE gpu = shaderVisibleHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE input = gpu;
    input.ptr += static_cast<SIZE_T>(m_descriptorBase + m_currentBuffer) * m_descriptorIncrement;
    D3D12_GPU_DESCRIPTOR_HANDLE output = gpu;
    output.ptr += static_cast<SIZE_T>(m_descriptorBase + outputBuffer) * m_descriptorIncrement;
    cmd->SetComputeRootDescriptorTable(1, input);
    cmd->SetComputeRootDescriptorTable(2, output);
    cmd->Dispatch((kParticleCount + 63u) / 64u, 1, 1);

    D3D12_RESOURCE_BARRIER afterCompute[2] = {
        UavBarrier(m_particles[outputBuffer].Get()),
        Transition(m_particles[outputBuffer].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(_countof(afterCompute), afterCompute);
    m_currentBuffer = outputBuffer;
    m_firstUpdate = false;

    cmd->SetGraphicsRootSignature(m_renderRootSignature.Get());
    cmd->SetPipelineState(m_renderPso.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_constants[frameIndex]->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE particleSrv = gpu;
    particleSrv.ptr += static_cast<SIZE_T>(m_descriptorBase + 2 + m_currentBuffer) * m_descriptorIncrement;
    cmd->SetGraphicsRootDescriptorTable(1, particleSrv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->DrawInstanced(kParticleCount, 1, 0, 0);
}

void ParticleSystem::ReleaseUploadResources()
{
    // InitD3D calls this only after ExecuteCommandList has waited for the GPU.
    m_initialParticlesUpload.Reset();
    m_initialCountersUpload.Reset();
}
