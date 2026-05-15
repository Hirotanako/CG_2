#include "TextureUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace Tex
{
namespace
{

void CreateSrv(
    ID3D12Device* device,
    ID3D12Resource* tex,
    ID3D12DescriptorHeap* heap,
    UINT heapIndex,
    UINT descriptorIncrement)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(heapIndex) * descriptorIncrement;
    device->CreateShaderResourceView(tex, &srv, h);
}

void AppendTransition(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &b);
}

bool CopyBufferToTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* dstTex,
    const D3D12_RESOURCE_DESC& desc,
    const void* srcPixels,
    UINT srcRowPitch,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep)
{
    UINT64 uploadSize = 0;
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &numRows, &rowSizeBytes, &uploadSize);

    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = uploadSize;
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    HRESULT hr = device->CreateCommittedResource(
        &uhp, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr))
        return false;

    BYTE* mapped = nullptr;
    hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr))
        return false;

    const BYTE* src = static_cast<const BYTE*>(srcPixels);
    for (UINT row = 0; row < numRows; ++row)
    {
        BYTE* dstRow = mapped + layout.Offset + row * layout.Footprint.RowPitch;
        const BYTE* srcRow = src + row * srcRowPitch;
        memcpy(dstRow, srcRow, static_cast<size_t>(rowSizeBytes));
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = dstTex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    uploadKeep.push_back(std::move(upload));
    return true;
}

} // namespace

bool CreateSolidTexture2D(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    uint32_t rgba8Unorm,
    ComPtr<ID3D12Resource>& outTexture,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep)
{
    outTexture.Reset();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 1;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;

    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture));
    if (FAILED(hr))
        return false;

    if (!CopyBufferToTexture(device, cmdList, outTexture.Get(), rd, &rgba8Unorm, 4, uploadKeep))
        return false;

    AppendTransition(cmdList, outTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateSrv(device, outTexture.Get(), srvHeap, heapIndex, descriptorIncrement);
    return true;
}

bool CreateTexture2DFromFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT heapIndex,
    UINT descriptorIncrement,
    const std::filesystem::path& filePath,
    ComPtr<ID3D12Resource>& outTexture,
    std::vector<ComPtr<ID3D12Resource>>& uploadKeep,
    std::wstring& error)
{
    outTexture.Reset();
    error.clear();

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        error = L"WIC factory";
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        filePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
    {
        error = L"Файл текстуры: " + filePath.wstring();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        return false;

    ComPtr<IWICFormatConverter> conv;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr))
        return false;

    hr = conv->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr))
        return false;

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0)
    {
        error = L"Нулевой размер текстуры";
        return false;
    }

    const UINT rowPitch = w * 4;
    const UINT imageSize = rowPitch * h;
    std::vector<uint8_t> pixels(imageSize);
    hr = conv->CopyPixels(nullptr, rowPitch, imageSize, pixels.data());
    if (FAILED(hr))
        return false;

    D3D12_HEAP_PROPERTIES thp{};
    thp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;

    hr = device->CreateCommittedResource(
        &thp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTexture));
    if (FAILED(hr))
        return false;

    if (!CopyBufferToTexture(device, cmdList, outTexture.Get(), rd, pixels.data(), rowPitch, uploadKeep))
        return false;

    AppendTransition(cmdList, outTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CreateSrv(device, outTexture.Get(), srvHeap, heapIndex, descriptorIncrement);
    return true;
}

} // namespace Tex.
// база
