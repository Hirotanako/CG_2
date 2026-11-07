#pragma once

#include <cstdint>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

class TemporalAA
{
public:
    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* srvHeap,
        UINT currentSrvSlot,
        UINT historySrvSlot0,
        UINT historySrvSlot1,
        UINT srvIncrement,
        const wchar_t* shaderPath);

    void Resize(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE BeginCurrent(ID3D12GraphicsCommandList* cmd);
    void Resolve(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* srvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT motionSrvSlot,
        const DirectX::XMFLOAT2& currentJitterUv,
        const DirectX::XMFLOAT2& previousJitterUv);

    void Invalidate() { m_historyValid = false; }
    void Toggle() { m_enabled = !m_enabled; Invalidate(); }
    void ToggleMotionDebug() { m_debugMotion = !m_debugMotion; Invalidate(); }
    bool Enabled() const { return m_enabled; }
    bool MotionDebugEnabled() const { return m_debugMotion; }

private:
    void CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath);
    void CreateTargets(ID3D12Device* device, UINT width, UINT height, ID3D12DescriptorHeap* srvHeap);
    void WriteSrv(ID3D12Device* device, ID3D12Resource* resource, ID3D12DescriptorHeap* heap, UINT slot);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_current;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_history[2];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_currentSrvSlot = 0;
    UINT m_historySrvSlot[2]{};
    UINT m_srvIncrement = 0;
    UINT m_rtvIncrement = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_historyRead = 0;
    bool m_historyValid = false;
    bool m_enabled = true;
    bool m_debugMotion = false;
};
