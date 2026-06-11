#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <deque>
#include <vector>

#include <wrl/client.h>

struct RainPointLightGpu
{
    DirectX::XMFLOAT4 posRange{};
    DirectX::XMFLOAT4 colorIntensity{};
};

class RainLightSystem
{
public:
    static constexpr uint32_t kMaxGpuLights = 512;
    static constexpr uint32_t kMinLandedBeforeFade = 200;

    void Init(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvSlot,
        UINT srvDescriptorSize);
    void ConfigureInterior(
        const DirectX::XMFLOAT3& spawnMinXZY,
        const DirectX::XMFLOAT3& spawnMaxXZY,
        float floorY,
        float ceilingY,
        bool fallUpward = false);
    void Update(float dt);
    void Upload();
    UINT GetGpuLightCount() const { return m_gpuLightCount; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(
        ID3D12DescriptorHeap* heap,
        UINT srvDescriptorSize) const;

private:
    struct RainDrop
    {
        DirectX::XMFLOAT3 position{};
        DirectX::XMFLOAT3 color{1, 1, 1};
        float fallSpeed = 8.0f;
        float intensity = 4.0f;
        float range = 3.5f;
        float fade = 1.0f;
        bool falling = true;
    };

    void SpawnDrop();
    void LandDrop(RainDrop drop);

    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightUpload;
    RainPointLightGpu* m_lightMapped = nullptr;
    UINT m_srvSlot = 0;
    UINT m_srvDescriptorSize = 0;

    std::vector<RainDrop> m_falling;
    std::deque<RainDrop> m_landed;
    std::vector<RainPointLightGpu> m_gpuScratch;

    DirectX::XMFLOAT3 m_spawnMin{};
    DirectX::XMFLOAT3 m_spawnMax{};
    float m_floorY = 0.0f;
    float m_ceilingY = 6.0f;
    float m_spawnY = 0.0f;
    float m_landY = 0.0f;
    int m_fallSign = -1;
    bool m_configured = false;
    float m_spawnTimer = 0.0f;
    UINT m_gpuLightCount = 0;
    uint32_t m_rngState = 0xC0FFEE01u;
};
