#include "GBuffer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cwchar>

using Microsoft::WRL::ComPtr;

static void GFThrow(HRESULT hr)
{
    if (FAILED(hr))
    {
        wchar_t b[72];
        swprintf_s(b, L"GBuffer HRESULT 0x%08X", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, b, L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
}

void GBuffer::DestroySizeDependent()
{
    m_albedo.Reset();
    m_normal.Reset();
    m_position.Reset();
    m_depth.Reset();
}

void GBuffer::CreateTargets(ID3D12Device* device, UINT width, UINT height)
{
    DestroySizeDependent();
    m_w = width;
    m_h = height;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto makeColor = [&](DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width;
        rd.Height = height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = fmt;
        cv.Color[0] = cv.Color[1] = cv.Color[2] = 0.0f;
        cv.Color[3] = 1.0f;
        GFThrow(device->CreateCommittedResource(
            &hp,
            D3D12_HEAP_FLAG_NONE,
            &rd,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &cv,
            IID_PPV_ARGS(&out)));
    };

    makeColor(DXGI_FORMAT_R8G8B8A8_UNORM, m_albedo);
    makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, m_normal);
    makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, m_position);

    D3D12_RESOURCE_DESC ds{};
    ds.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ds.Width = width;
    ds.Height = height;
    ds.DepthOrArraySize = 1;
    ds.MipLevels = 1;
    ds.Format = DXGI_FORMAT_D32_FLOAT;
    ds.SampleDesc.Count = 1;
    ds.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = DXGI_FORMAT_D32_FLOAT;
    dcv.DepthStencil.Depth = 1.0f;
    GFThrow(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&m_depth)));

    D3D12_CPU_DESCRIPTOR_HANDLE rBase = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RENDER_TARGET_VIEW_DESC ra{};
    ra.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ra.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_albedo.Get(), &ra, rBase);
    rBase.ptr += static_cast<SIZE_T>(m_rtvInc);

    D3D12_RENDER_TARGET_VIEW_DESC rn{};
    rn.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rn.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_normal.Get(), &rn, rBase);
    rBase.ptr += static_cast<SIZE_T>(m_rtvInc);

    D3D12_RENDER_TARGET_VIEW_DESC rp{};
    rp.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rp.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    device->CreateRenderTargetView(m_position.Get(), &rp, rBase);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depth.Get(), &dsv, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void GBuffer::Init(ID3D12Device* device, UINT width, UINT height)
{
    m_dev = device;

    m_rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rhd.NumDescriptors = kRtvCount;
    GFThrow(device->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&m_rtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC dhd{};
    dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dhd.NumDescriptors = 1;
    GFThrow(device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&m_dsvHeap)));

    CreateTargets(device, width, height);
}

void GBuffer::Resize(ID3D12Device* device, UINT width, UINT height)
{
    if (width == m_w && height == m_h)
        return;
    CreateTargets(device, width, height);
}

static D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* r, D3D12_RESOURCE_STATES b, D3D12_RESOURCE_STATES a)
{
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = r;
    bar.Transition.StateBefore = b;
    bar.Transition.StateAfter = a;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return bar;
}

void GBuffer::TransitionToRenderTargets(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[3] = {
        Transition(m_albedo.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(m_normal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(m_position.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, bars);
}

void GBuffer::TransitionToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[3] = {
        Transition(m_albedo.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_normal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_position.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(3, bars);
}

void GBuffer::ClearAndSetAsRenderTarget(ID3D12GraphicsCommandList* cmd, const float clearRgb[4])
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kRtvCount];
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < kRtvCount; ++i)
    {
        rtvs[i] = h;
        h.ptr += static_cast<SIZE_T>(m_rtvInc);
        const float cc[4] = {clearRgb[0], clearRgb[1], clearRgb[2], 1.0f};
        cmd->ClearRenderTargetView(rtvs[i], cc, 0, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(kRtvCount, rtvs, FALSE, &dsv);
}

void GBuffer::CreateShaderResourceViews(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvStartIndex,
    UINT srvDescriptorSize)
{
    D3D12_CPU_DESCRIPTOR_HANDLE dh = srvHeap->GetCPUDescriptorHandleForHeapStart();
    dh.ptr += static_cast<SIZE_T>(srvStartIndex) * srvDescriptorSize;

    auto makeSrv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s{};
        s.Format = fmt;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        s.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex, &s, dh);
        dh.ptr += static_cast<SIZE_T>(srvDescriptorSize);
    };

    makeSrv(m_albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    makeSrv(m_normal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    makeSrv(m_position.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::RtvCpuHandle(size_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * m_rtvInc;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::DsvCpuHandle() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}
