#pragma once

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <vector>

#include <wrl/client.h>

namespace Tex
{

bool CreateSolidTexture2D(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    uint32_t rgba8Unorm,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadKeep);

bool CreateTexture2DFromFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    const std::filesystem::path& filePath,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadKeep,
    std::wstring& error);

} // namespace Tex
