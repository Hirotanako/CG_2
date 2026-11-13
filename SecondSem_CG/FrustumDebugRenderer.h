#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

// Draws the controlled camera frustum over the observer-camera image.
// The renderer is intentionally independent from the deferred pass: the lines
// are added to the final back buffer after lighting/TAA and stay easy to see.
class FrustumDebugRenderer
{
public:
    bool Init(ID3D12Device* device, const wchar_t* shaderPath, std::wstring& error);
    void Draw(
        ID3D12GraphicsCommandList* commandList,
        uint32_t frameIndex,
        const DirectX::XMMATRIX& observerViewProjection,
        const DirectX::XMMATRIX& sourceViewProjection);

private:
    static constexpr uint32_t kFrameCount = 2;
    static constexpr uint32_t kVertexCount = 24;

    struct Vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 color;
    };

    struct alignas(256) FrameConstants
    {
        DirectX::XMFLOAT4X4 viewProjection{};
        float padding[48]{};
    };

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantUpload;
    uint8_t* m_verticesMapped = nullptr;
    uint8_t* m_constantsMapped = nullptr;
};
