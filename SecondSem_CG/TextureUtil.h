#pragma once

#include <d3d12.h>
#include <filesystem>
#include <string>
#include <vector>

#include <wrl/client.h>

namespace Tex
{

// Decode an image through the same WIC/TGA path used by GPU textures, but keep
// the RGBA8 pixels on the CPU (height fields and vertex-baked terrain maps).
bool LoadImageRgba8(
    const std::filesystem::path& filePath,
    std::vector<uint8_t>& outPixels,
    UINT& outWidth,
    UINT& outHeight,
    std::wstring& error);

// Каталог текстур: <mtlDir>/textures/. В MTL может быть другое расширение — ищем тот же stem с .tga.
std::filesystem::path ResolveTexturePathInTexturesFolder(
    const std::filesystem::path& mtlDir,
    const std::wstring& mapRelFromMtl);

bool CreateSolidTexture2D(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    uint32_t rgba8Unorm,
    Microsoft::WRL::ComPtr<ID3D12Resource>& outTexture,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadKeep);

bool CreateTexture2DFromRgba8(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    UINT width,
    UINT height,
    const uint8_t* pixels,
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

// Запись SRV для уже созданной текстуры R8G8B8A_UNORM (повторное использование ресурса в другом слоте кучи).
void WriteTexture2DSrv(
    ID3D12Device* device,
    ID3D12Resource* texture,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement);

} // namespace Tex
