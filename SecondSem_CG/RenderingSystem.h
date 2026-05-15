#pragma once

// RenderingSystem — высокоуровневая подсистема рендеринга для отложенного освещения:
// оркеструет G-buffer (геопроход) и полноэкранный проход света по нескольким источникам.

#include <cstdint>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "GBuffer.h"

class RenderingSystem
{
public:
    void Init(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT gbufferSrvStartIndex,
        UINT srvDescriptorIncrement,
        const wchar_t* deferredHlslPath);

    void Resize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        ID3D12DescriptorHeap* shaderVisibleSrvHeap,
        UINT srvDescriptorIncrement);

    void UploadFrameConstants(
        const DirectX::XMFLOAT3& cameraPos,
        UINT screenW,
        UINT screenH);

    void DrawLightingPass(
        ID3D12GraphicsCommandList* cmd,
        ID3D12DescriptorHeap* srvHeapShaderVisible,
        D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv,
        UINT screenW,
        UINT screenH);

    GBuffer& GBufferTargets() { return m_gbuffer; }

private:
    void CreateLightingPipeline(ID3D12Device* device, const wchar_t* hlslPath);
    void WriteDefaultLights();

    GBuffer m_gbuffer;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSigLight;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoLight;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_lightingCB;
    uint8_t* m_lightingCBMapped = nullptr;

    UINT m_gbufferSrvBase = 0;
    UINT m_srvDescriptorIncrement = 0;
};
