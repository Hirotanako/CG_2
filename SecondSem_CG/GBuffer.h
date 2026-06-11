#pragma once

// G-buffer (Geometry buffer) — набор вспомогательных рендер-таргетов для отложенного освещения:
// в геопроходе сюда пишутся альбедо, нормали и позиция в мире; в проходе света только читаются.

#include <d3d12.h>
#include <wrl/client.h>

class GBuffer
{
public:
    void Init(ID3D12Device* device, UINT width, UINT height);
    void Resize(ID3D12Device* device, UINT width, UINT height);

    void CreateShaderResourceViews(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvStartIndex,
        UINT srvDescriptorSize);

    void TransitionToRenderTargets(ID3D12GraphicsCommandList* cmd);
    void TransitionToShaderResource(ID3D12GraphicsCommandList* cmd);

    void ClearAndSetAsRenderTarget(
        ID3D12GraphicsCommandList* cmd,
        const float clearRgb[4]);

    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(size_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle() const;

    ID3D12Resource* Depth() const { return m_depth.Get(); }

private:
    void DestroySizeDependent();
    void CreateTargets(ID3D12Device* device, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D12Device> m_dev;
    UINT m_w = 0;
    UINT m_h = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_albedo;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_position;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvInc = 0;
    UINT m_dsvInc = 0;

    enum : int
    {
        kRtvCount = 3
    };
};
