#pragma once

#include <cstdint>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// GPU particle system.  The two particle buffers exchange their roles every
// frame: the compute shader consumes one and appends the updated particles to
// the other one.
class ParticleSystem
{
public:
    static constexpr UINT kParticleCount = 2048;
    static constexpr UINT kDescriptorCount = 4;

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

    Microsoft::WRL::ComPtr<ID3D12Resource> m_particles[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_counters[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_initialParticlesUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_initialCountersUpload;
    static constexpr UINT kFrameCount = 2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants[kFrameCount];
    std::uint8_t* m_constantsMapped[kFrameCount]{};

    UINT m_descriptorBase = 0;
    UINT m_descriptorIncrement = 0;
    UINT m_currentBuffer = 0;
    bool m_firstUpdate = true;
};
