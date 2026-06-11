#include "RainLightSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{

float Rand01(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return static_cast<float>(state & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, UINT64 size)
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
        return {};
    return buf;
}

} // namespace

void RainLightSystem::Init(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvSlot,
    UINT srvDescriptorSize)
{
    m_srvSlot = srvSlot;
    m_srvDescriptorSize = srvDescriptorSize;

    const UINT64 bufSize =
        static_cast<UINT64>(sizeof(RainPointLightGpu)) * static_cast<UINT64>(kMaxGpuLights);
    m_lightUpload = CreateUploadBuffer(device, bufSize);
    if (!m_lightUpload)
        return;

    D3D12_RANGE rr{0, 0};
    m_lightUpload->Map(0, &rr, reinterpret_cast<void**>(&m_lightMapped));
    std::memset(m_lightMapped, 0, static_cast<size_t>(bufSize));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = kMaxGpuLights;
    srvDesc.Buffer.StructureByteStride = sizeof(RainPointLightGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(m_srvSlot) * srvDescriptorSize;
    device->CreateShaderResourceView(m_lightUpload.Get(), &srvDesc, cpu);
}

void RainLightSystem::ConfigureInterior(
    const XMFLOAT3& spawnMinXZY,
    const XMFLOAT3& spawnMaxXZY,
    float floorY,
    float ceilingY,
    bool fallUpward)
{
    m_spawnMin = spawnMinXZY;
    m_spawnMax = spawnMaxXZY;
    m_floorY = floorY;
    m_ceilingY = ceilingY;

    if (!fallUpward)
    {
        m_spawnY = ceilingY;
        m_landY = floorY + 0.08f;
        m_fallSign = -1;
    }
    else
    {
        m_spawnY = floorY + 0.05f;
        m_landY = ceilingY - 0.05f;
        m_fallSign = 1;
    }

    m_configured = true;
    m_falling.clear();
    m_landed.clear();
    m_spawnTimer = 0.0f;
}

void RainLightSystem::SpawnDrop()
{
    if (!m_configured)
        return;

    RainDrop drop{};
    drop.position.x = std::lerp(m_spawnMin.x, m_spawnMax.x, Rand01(m_rngState));
    drop.position.y = m_spawnY;
    drop.position.z = std::lerp(m_spawnMin.z, m_spawnMax.z, Rand01(m_rngState));
    drop.fallSpeed = std::lerp(7.0f, 14.0f, Rand01(m_rngState));
    drop.range = std::lerp(2.8f, 4.2f, Rand01(m_rngState));
    drop.intensity = std::lerp(3.5f, 7.0f, Rand01(m_rngState));

    const float tint = Rand01(m_rngState);
    if (tint < 0.33f)
        drop.color = XMFLOAT3{0.55f, 0.82f, 1.0f};
    else if (tint < 0.66f)
        drop.color = XMFLOAT3{1.0f, 0.95f, 0.65f};
    else
        drop.color = XMFLOAT3{0.75f, 0.55f, 1.0f};

    drop.falling = true;
    drop.fade = 1.0f;
    m_falling.push_back(drop);
}

void RainLightSystem::LandDrop(RainDrop drop)
{
    drop.falling = false;
    drop.position.y = m_landY;
    drop.fade = 1.0f;
    m_landed.push_back(drop);

    constexpr size_t kMaxLanded = 500;
    while (m_landed.size() > kMaxLanded && m_landed.size() > kMinLandedBeforeFade)
        m_landed.pop_front();
}

void RainLightSystem::Update(float dt)
{
    if (!m_configured || dt <= 0.0f)
        return;

    m_spawnTimer += dt;
    constexpr float kSpawnInterval = 0.045f;
    constexpr size_t kMaxFalling = 96;
    while (m_spawnTimer >= kSpawnInterval && m_falling.size() < kMaxFalling)
    {
        m_spawnTimer -= kSpawnInterval;
        SpawnDrop();
    }

    for (size_t i = 0; i < m_falling.size();)
    {
        RainDrop& drop = m_falling[i];
        drop.position.y += static_cast<float>(m_fallSign) * drop.fallSpeed * dt;
        const bool landed = (m_fallSign < 0) ? (drop.position.y <= m_landY) : (drop.position.y >= m_landY);
        if (landed)
        {
            LandDrop(drop);
            m_falling[i] = m_falling.back();
            m_falling.pop_back();
            continue;
        }
        ++i;
    }

    if (m_landed.size() > kMinLandedBeforeFade)
    {
        RainDrop& oldest = m_landed.front();
        oldest.fade -= dt * 0.12f;
        oldest.intensity = (std::max)(0.0f, oldest.intensity * (1.0f - dt * 0.04f));
        if (oldest.fade <= 0.0f || oldest.intensity < 0.25f)
            m_landed.pop_front();
    }
}

void RainLightSystem::Upload()
{
    if (!m_lightMapped)
    {
        m_gpuLightCount = 0;
        return;
    }

    m_gpuScratch.clear();
    m_gpuScratch.reserve(m_falling.size() + m_landed.size());

    auto packDrop = [&](const RainDrop& drop) {
        RainPointLightGpu gpu{};
        gpu.posRange = XMFLOAT4(drop.position.x, drop.position.y, drop.position.z, drop.range);
        gpu.colorIntensity = XMFLOAT4(
            drop.color.x,
            drop.color.y,
            drop.color.z,
            drop.intensity * drop.fade);
        m_gpuScratch.push_back(gpu);
    };

    for (const RainDrop& drop : m_falling)
        packDrop(drop);
    for (const RainDrop& drop : m_landed)
        packDrop(drop);

    m_gpuLightCount = static_cast<UINT>((std::min)(m_gpuScratch.size(), static_cast<size_t>(kMaxGpuLights)));
    if (m_gpuLightCount > 0)
        std::memcpy(m_lightMapped, m_gpuScratch.data(), sizeof(RainPointLightGpu) * m_gpuLightCount);
}

D3D12_GPU_DESCRIPTOR_HANDLE RainLightSystem::GetSrvGpuHandle(
    ID3D12DescriptorHeap* heap,
    UINT srvDescriptorSize) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<SIZE_T>(m_srvSlot) * srvDescriptorSize;
    return gpu;
}
