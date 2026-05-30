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

void GBuffer::DestroySizeDependent()
{
    m_lightAccum.Reset();
    m_normal.Reset();
    m_motionSpec.Reset();
    m_albedoOcc.Reset();
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

    makeColor(DXGI_FORMAT_R8G8B8A8_UNORM, m_lightAccum);
    makeColor(DXGI_FORMAT_R16G16B16A16_FLOAT, m_normal);
    makeColor(DXGI_FORMAT_R8G8B8A8_UNORM, m_motionSpec);
    makeColor(DXGI_FORMAT_R8G8B8A8_UNORM, m_albedoOcc);

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
    dcv.DepthStencil.Stencil = 0;
    GFThrow(device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&m_depth)));

    D3D12_CPU_DESCRIPTOR_HANDLE rBase = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    auto makeRtv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE h) {
        D3D12_RENDER_TARGET_VIEW_DESC d{};
        d.Format = fmt;
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(tex, &d, h);
    };

    makeRtv(m_lightAccum.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rBase);
    rBase.ptr += static_cast<SIZE_T>(m_rtvInc);
    makeRtv(m_normal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, rBase);
    rBase.ptr += static_cast<SIZE_T>(m_rtvInc);
    makeRtv(m_motionSpec.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rBase);
    rBase.ptr += static_cast<SIZE_T>(m_rtvInc);
    makeRtv(m_albedoOcc.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rBase);

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
    rhd.NumDescriptors = kTargetCount;
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

void GBuffer::TransitionGeometryToRenderTargets(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[3] = {
        Transition(m_normal.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(m_motionSpec.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
        Transition(m_albedoOcc.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    cmd->ResourceBarrier(3, bars);
    D3D12_RESOURCE_BARRIER db =
        Transition(m_depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmd->ResourceBarrier(1, &db);
}

void GBuffer::TransitionGeometryToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER bars[4] = {
        Transition(m_normal.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_motionSpec.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_albedoOcc.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
    };
    cmd->ResourceBarrier(4, bars);
}

void GBuffer::TransitionLightAccumToRenderTarget(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER b = Transition(
        m_lightAccum.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(1, &b);
}

void GBuffer::TransitionLightAccumToShaderResource(ID3D12GraphicsCommandList* cmd)
{
    D3D12_RESOURCE_BARRIER b = Transition(
        m_lightAccum.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &b);
}

void GBuffer::ClearAndSetGeometryRenderTargets(ID3D12GraphicsCommandList* cmd, const float clearRgb[3])
{
    const float clear0[4] = {0, 0, 0, 1};
    const float clearAlbedo[4] = {clearRgb[0], clearRgb[1], clearRgb[2], 1.0f};

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3];
    rtvs[0] = RtvCpuHandle(kNormal);
    rtvs[1] = RtvCpuHandle(kMotionSpec);
    rtvs[2] = RtvCpuHandle(kAlbedoOcc);

    cmd->ClearRenderTargetView(rtvs[0], clear0, 0, nullptr);
    cmd->ClearRenderTargetView(rtvs[1], clear0, 0, nullptr);
    cmd->ClearRenderTargetView(rtvs[2], clearAlbedo, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = DsvCpuHandle();
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(3, rtvs, FALSE, &dsv);
}

void GBuffer::ClearLightAccum(ID3D12GraphicsCommandList* cmd)
{
    const float z[4] = {0, 0, 0, 0};
    cmd->ClearRenderTargetView(RtvCpuHandle(kLightAccum), z, 0, nullptr);
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

    makeSrv(m_depth.Get(), DXGI_FORMAT_R32_FLOAT);
    makeSrv(m_lightAccum.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    makeSrv(m_normal.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    makeSrv(m_motionSpec.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    makeSrv(m_albedoOcc.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::RtvCpuHandle(Target index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(index) * m_rtvInc;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::DsvCpuHandle() const
{
    return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
}
