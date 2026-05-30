#pragma once

// G-buffer в раскладке Killzone 2 (слайд):
// RT0 — накопление света RGB + intensity A
// RT1 — normal X (RG FP16) + normal Y (BA FP16)
// RT2 — motion XY + spec power + spec intensity
// RT3 — diffuse albedo RGB + sun occlusion A
// DS  — depth (+ stencil при clear)

#include <d3d12.h>
#include <wrl/client.h>

class GBuffer
{
public:
    enum Target : int
    {
        kLightAccum = 0,
        kNormal = 1,
        kMotionSpec = 2,
        kAlbedoOcc = 3,
        kTargetCount = 4
    };

    void Init(ID3D12Device* device, UINT width, UINT height);
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void CreateShaderResourceViews(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvStartIndex,
        UINT srvDescriptorIncrement);

    void TransitionGeometryToRenderTargets(ID3D12GraphicsCommandList* cmd);
    void TransitionGeometryToShaderResource(ID3D12GraphicsCommandList* cmd);

    void TransitionLightAccumToRenderTarget(ID3D12GraphicsCommandList* cmd);
    void TransitionLightAccumToShaderResource(ID3D12GraphicsCommandList* cmd);

    void ClearAndSetGeometryRenderTargets(
        ID3D12GraphicsCommandList* cmd,
        const float clearRgb[3]);

    void ClearLightAccum(ID3D12GraphicsCommandList* cmd);

    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(Target index) const;

    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle() const;

    ID3D12Resource* Depth() const { return m_depth.Get(); }

    static constexpr UINT kSrvCount = 5; // depth + RT0..RT3

private:
    void DestroySizeDependent();
    void CreateTargets(ID3D12Device* device, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D12Device> m_dev;
    UINT m_w = 0;
    UINT m_h = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightAccum;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motionSpec;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_albedoOcc;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvInc = 0;
    UINT m_dsvInc = 0;
};
