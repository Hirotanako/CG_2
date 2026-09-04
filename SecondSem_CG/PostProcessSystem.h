#pragma once

#include <d3d12.h>
#include <wrl/client.h>

class PostProcessSystem
{
public:
    static constexpr UINT kDescriptorCount = 2;

    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT srvBase,
        UINT srvDescriptorIncrement,
        const wchar_t* shaderPath);

    void Resize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap);

    D3D12_CPU_DESCRIPTOR_HANDLE BeginScene(ID3D12GraphicsCommandList* cmd);

    void Apply(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT width,
        UINT height);

private:
    void CreatePipeline(ID3D12Device* device, const wchar_t* shaderPath);
    void CreateTargets(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_grayscalePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurPso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_grayscaleColor;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    UINT m_srvBase = 0;
    UINT m_srvIncrement = 0;
    UINT m_rtvIncrement = 0;
};
