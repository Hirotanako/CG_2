#pragma once

// Каскадные карты теней (CSM): нелинейное распределение каскадов + depth pass.

#include <cstdint>
#include <functional>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

class ShadowSystem
{
public:
    static constexpr UINT kCascadeCount = 4;
    static constexpr UINT kMapSize = 2048;
    static constexpr float kSplitLambda = 0.72f;
    static constexpr float kCameraNear = 0.1f;
    static constexpr float kCameraFar = 500.0f;
    static constexpr float kShadowBias = 0.00006f;
    static constexpr float kNormalBias = 0.012f;
    static constexpr float kSlopeScale = 0.0018f;

    void Init(
        ID3D12Device* device,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT shadowSrvSlot,
        UINT srvDescriptorIncrement,
        const wchar_t* deferredHlslPath);

    void UpdateCascades(
        const DirectX::XMMATRIX& cameraView,
        const DirectX::XMMATRIX& cameraProj,
        const DirectX::XMFLOAT3& cameraPos,
        const DirectX::XMFLOAT3& lightDir,
        const DirectX::XMFLOAT3& sceneCenter,
        float sceneRadius);

    void DrawShadowPass(
        ID3D12GraphicsCommandList* cmd,
        const std::function<void(const DirectX::XMMATRIX& lightViewProj)>& drawScene);

    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);

    D3D12_GPU_DESCRIPTOR_HANDLE ShadowSrvGpu(ID3D12DescriptorHeap* srvHeap) const;
    D3D12_GPU_VIRTUAL_ADDRESS ShadowCBAddress() const;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature() const { return m_rootSig; }
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState() const { return m_psoShadow; }

private:
    void CreateResources(ID3D12Device* device, ID3D12DescriptorHeap* srvHeap);
    void CreatePipeline(ID3D12Device* device, const wchar_t* hlslPath);
    DirectX::XMMATRIX ComputeCascadeMatrix(
        const DirectX::XMMATRIX& cameraView,
        const DirectX::XMMATRIX& cameraProj,
        float splitNear,
        float splitFar,
        const DirectX::XMVECTOR& lightDir,
        const DirectX::XMVECTOR& sceneCenter) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowCB;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoShadow;

    uint8_t* m_shadowCBMapped = nullptr;
    UINT m_shadowSrvSlot = 0;
    UINT m_srvIncrement = 0;
    UINT m_dsvDescriptorSize = 0;
    bool m_shadowIsSrv = false;
};
