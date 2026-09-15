#pragma once

#include <cstdint>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// GPU particle system. The two particle buffers exchange their roles every
// frame: the compute shader consumes one and appends the updated particles to
// the other one.
class ParticleSystem
{
public:
    static constexpr UINT kDirectionCount = 4;
    static constexpr UINT kParticlesPerDirection = 512;
    static constexpr UINT kParticleCount = kDirectionCount * kParticlesPerDirection;
    // Every directional stream owns two UAVs for Consume/Append ping-pong and
    // two matching SRVs used to draw the current buffer.
    static constexpr UINT kDescriptorCount = kDirectionCount * 4;

    void Init(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* shaderVisibleHeap,
        UINT descriptorBase,
        UINT descriptorIncrement,
        const wchar_t* shaderPath,
        const DirectX::XMFLOAT3& emitterPosition);

    void UpdateAndDraw(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* shaderVisibleHeap,
        UINT frameIndex,
        float deltaTime,
        float totalTime,
        const DirectX::XMMATRIX& view,
        const DirectX::XMMATRIX& viewProj,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMFLOAT3& emitterPosition,
        float floorHeight);

    void ReleaseUploadResources();

private:
    void CreatePipelines(ID3D12Device* device, const wchar_t* shaderPath);
    void CreateBuffers(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmd,
        const DirectX::XMFLOAT3& emitterPosition);
    void CreateDescriptors(ID3D12Device* device, ID3D12DescriptorHeap* heap);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_computePso;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_renderRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_renderPso;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_particles[kDirectionCount][2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counters[kDirectionCount][2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_initialParticlesUpload[kDirectionCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_initialCountersUpload;
    static constexpr UINT kFrameCount = 2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants[kFrameCount][kDirectionCount];
    std::uint8_t* m_constantsMapped[kFrameCount][kDirectionCount]{};

    UINT m_descriptorBase = 0;
    UINT m_descriptorIncrement = 0;
    UINT m_currentBuffer[kDirectionCount]{};
    bool m_firstUpdate[kDirectionCount]{true, true, true, true};
};
