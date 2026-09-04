// DirectX 12: окно, FPS-камера, сцена Sponza (OBJ/MTL).

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <wrl/client.h>

#include "ObjLoader.h"
#include "ParticleSystem.h"
#include "PostProcessSystem.h"
#include "RenderingSystem.h"
#include "ShadowSystem.h"
#include "TextureUtil.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kFrameCount = 2;
constexpr UINT kClientW = 1280;
constexpr UINT kClientH = 720;
constexpr UINT kSrvHeapCount = 512;
constexpr UINT kDeferredSrvBase = 400;
constexpr UINT kShadowSrvBase = 403;
constexpr UINT kParticleSrvBase = 404;
constexpr UINT kPostProcessSrvBase = 408;
constexpr UINT kCbAlign = 256;
constexpr UINT kCubeCount = 1000;

struct alignas(256) FrameCB
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProj;
    XMFLOAT4 TimeCamPos;
    XMFLOAT4 UvAnimAndPad;
    float _PadRest[24];
};

static_assert(sizeof(FrameCB) == 256);

struct alignas(256) MatCBGPU
{
    XMFLOAT4 Kd;
    XMFLOAT2 UvScale;
    XMFLOAT2 UvOffset;
    XMFLOAT3 Ks;
    float Ns;
    UINT UseUvAnim;
    UINT HasSpecularTex;
    UINT UseSwayAnim;
    float _PadMat[49];
};

static_assert(sizeof(MatCBGPU) == 256);

HWND g_hwnd = nullptr;
UINT g_width = kClientW;
UINT g_height = kClientH;
bool g_running = true;

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<IDXGIFactory6> g_factory;
ComPtr<IDXGISwapChain3> g_swapChain;

ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
UINT g_rtvDescriptorSize = 0;
ComPtr<ID3D12Resource> g_renderTargets[kFrameCount];

ComPtr<ID3D12DescriptorHeap> g_dsvHeap;
ComPtr<ID3D12Resource> g_depthStencil;

ComPtr<ID3D12CommandAllocator> g_cmdAlloc[kFrameCount];
ComPtr<ID3D12GraphicsCommandList> g_cmdList;

ComPtr<ID3D12Fence> g_fence;
UINT64 g_fenceValue = 0;
HANDLE g_fenceEvent = nullptr;
UINT64 g_frameFenceValues[kFrameCount]{};
bool g_swapSeenPresent[kFrameCount]{};

ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pipelineGeo;

RenderingSystem g_renderSys;
ShadowSystem g_shadowSys;
ParticleSystem g_particleSys;
PostProcessSystem g_postProcessSys;

ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvDescriptorSize = 0;

bool g_sceneReady = false;
Obj::LoadedMesh g_mesh{};
ComPtr<ID3D12Resource> g_meshVB;
ComPtr<ID3D12Resource> g_meshIB;
D3D12_VERTEX_BUFFER_VIEW g_meshVbv{};
D3D12_INDEX_BUFFER_VIEW g_meshIbv{};

std::vector<uint32_t> g_matSrvPairBase;
std::vector<ComPtr<ID3D12Resource>> g_gpuTextures;
ComPtr<ID3D12Resource> g_whiteTexture;
ComPtr<ID3D12Resource> g_matCBUpload;
UINT8* g_matCBMapped = nullptr;
UINT g_matCount = 0;

ComPtr<ID3D12Resource> g_frameCBUpload;
UINT8* g_frameCBMapped = nullptr;

ComPtr<ID3D12Resource> g_cubeVB;
ComPtr<ID3D12Resource> g_cubeIB;
D3D12_VERTEX_BUFFER_VIEW g_cubeVbv{};
D3D12_INDEX_BUFFER_VIEW g_cubeIbv{};
ComPtr<ID3D12Resource> g_cubeFrameCBUpload;
UINT8* g_cubeFrameCBMapped = nullptr;
ComPtr<ID3D12Resource> g_cubeMatCBUpload;
UINT8* g_cubeMatCBMapped = nullptr;

UINT g_frameIndex = 0;
float g_appTime = 0.0f;

XMFLOAT3 g_camPos{0.0f, 1.4f, 4.5f};
float g_camYaw = 0.0f;
float g_camPitch = -0.12f;
bool g_camPrevRmb = false;

XMFLOAT3 g_sceneCenter{0.f, 2.f, 0.f};
float g_sceneRadius = 25.f;
float g_particleFloorY = 0.f;

LARGE_INTEGER g_qpcFreq{};
LARGE_INTEGER g_qpcLast{};

void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        wchar_t buf[96];
        swprintf_s(buf, L"HRESULT 0x%08X", static_cast<unsigned>(hr));
        MessageBoxW(nullptr, buf, L"SecondSem CG", MB_OK | MB_ICONERROR);
        std::exit(static_cast<int>(hr));
    }
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        p.resize(slash + 1);
    return p;
}

std::wstring DeferredShaderPath()
{
    return ExeDirectory() + L"Deferred.hlsl";
}

std::wstring ParticleShaderPath()
{
    return ExeDirectory() + L"Particles.hlsl";
}

std::wstring PostProcessShaderPath()
{
    return ExeDirectory() + L"PostProcess.hlsl";
}

void CompileShader(const wchar_t* path, const char* entry, const char* target, ComPtr<ID3DBlob>& out)
{
    ComPtr<ID3DBlob> err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    const HRESULT hr = D3DCompileFromFile(
        path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &out, &err);
    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        ThrowIfFailed(hr);
    }
}

void WaitForGpu()
{
    const UINT64 v = ++g_fenceValue;
    ThrowIfFailed(g_queue->Signal(g_fence.Get(), v));
    if (g_fence->GetCompletedValue() < v)
    {
        ThrowIfFailed(g_fence->SetEventOnCompletion(v, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

void ExecuteCommandList()
{
    ThrowIfFailed(g_cmdList->Close());
    ID3D12CommandList* lists[] = {g_cmdList.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
}

D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

void CreateRtv()
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i])));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, h);
        h.ptr += static_cast<SIZE_T>(g_rtvDescriptorSize);
    }
}

void CreateDepth()
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC ds{};
    ds.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ds.Width = g_width;
    ds.Height = g_height;
    ds.DepthOrArraySize = 1;
    ds.MipLevels = 1;
    ds.Format = DXGI_FORMAT_D32_FLOAT;
    ds.SampleDesc.Count = 1;
    ds.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    ThrowIfFailed(g_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &ds, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
        IID_PPV_ARGS(&g_depthStencil)));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_device->CreateDepthStencilView(g_depthStencil.Get(), &dsv, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void ResizeSwapChain(UINT w, UINT h)
{
    if (!g_swapChain || w == 0 || h == 0)
        return;
    WaitForGpu();
    g_depthStencil.Reset();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        g_renderTargets[i].Reset();
        g_swapSeenPresent[i] = false;
        g_frameFenceValues[i] = g_fence->GetCompletedValue();
    }
    DXGI_SWAP_CHAIN_DESC d{};
    ThrowIfFailed(g_swapChain->GetDesc(&d));
    ThrowIfFailed(g_swapChain->ResizeBuffers(kFrameCount, w, h, d.BufferDesc.Format, d.Flags));
    g_width = w;
    g_height = h;
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    CreateRtv();
    CreateDepth();
    if (g_srvHeap && g_device)
    {
        g_renderSys.Resize(
            g_device.Get(), w, h, g_srvHeap.Get(), g_srvDescriptorSize);
        g_postProcessSys.Resize(g_device.Get(), w, h, g_srvHeap.Get());
    }
}

ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 size)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buf;
    ThrowIfFailed(g_device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)));
    void* mapped = nullptr;
    D3D12_RANGE rr{0, 0};
    ThrowIfFailed(buf->Map(0, &rr, &mapped));
    if (data != nullptr && size > 0)
        std::memcpy(mapped, data, static_cast<size_t>(size));
    else
        std::memset(mapped, 0, static_cast<size_t>(size));
    buf->Unmap(0, nullptr);
    return buf;
}

void CreateFrameCB()
{
    g_frameCBUpload = CreateUploadBuffer(nullptr, sizeof(FrameCB));
    D3D12_RANGE rr{0, 0};
    ThrowIfFailed(g_frameCBUpload->Map(0, &rr, reinterpret_cast<void**>(&g_frameCBMapped)));
}

void CreateSrvHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kSrvHeapCount;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void CreateGeometryPipeline()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr));
    ThrowIfFailed(g_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature)));

    const std::wstring sp = DeferredShaderPath();
    ComPtr<ID3DBlob> vs, ps;
    CompileShader(sp.c_str(), "GeometryVS", "vs_5_0", vs);
    CompileShader(sp.c_str(), "GeometryPS", "ps_5_0", ps);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    for (UINT rt = 0; rt < 3; ++rt)
        pso.BlendState.RenderTarget[rt].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 3;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {layout, _countof(layout)};
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipelineGeo)));
}

std::filesystem::path FindSponzaObj()
{
    const std::wstring rels[] = {
        L"Sponza\\sponza.obj",
        L"Sponza/sponza.obj",
        L"sponza.obj",
    };

    std::vector<std::filesystem::path> roots;
    auto tryAdd = [&roots](std::filesystem::path p) {
        if (p.empty())
            return;
        p = p.lexically_normal();
        for (const auto& existing : roots)
        {
            if (existing == p)
                return;
        }
        roots.push_back(std::move(p));
    };

    tryAdd(std::filesystem::path(ExeDirectory()));
    try
    {
        tryAdd(std::filesystem::current_path());
    }
    catch (...)
    {
    }

    // Расширяем базы родителями — только по индексу, без изменения вектора во время range-for.
    const size_t scanCount = roots.size();
    for (size_t i = 0; i < scanCount; ++i)
    {
        std::filesystem::path walk = roots[i];
        for (int depth = 0; depth < 4 && !walk.empty(); ++depth)
        {
            walk = walk.parent_path();
            tryAdd(walk);
        }
    }

    for (const auto& root : roots)
    {
        if (root.empty())
            continue;
        for (const auto& rel : rels)
        {
            const std::filesystem::path candidate = root / rel;
            if (std::filesystem::exists(candidate))
                return candidate;
        }
    }
    return {};
}

static bool MaterialPathSuggestUvAnim(const std::wstring& rel)
{
    std::wstring s = rel;
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));

    static const wchar_t* keys[] = {
        L"fabric", L"banner", L"water", L"flag", L"vines", L"leaf", L"drape", nullptr};
    for (int k = 0; keys[k] != nullptr; ++k)
    {
        if (s.find(keys[k]) != std::wstring::npos)
            return true;
    }
    return false;
}

static bool MaterialPathSuggestSway(const std::wstring& rel)
{
    std::wstring s = rel;
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
    return s.find(L"curtain") != std::wstring::npos;
}

XMMATRIX MeshWorldTransform()
{
    // This Sponza export is upside-down relative to the application's Y-up
    // world. RotationX(PI) converts (x,y,z) to (x,-y,-z). Its determinant is
    // positive, so triangle winding is preserved.
    return XMMatrixScaling(0.01f, 0.01f, 0.01f) * XMMatrixRotationX(XM_PI);
}

static bool IsFloorMaterial(UINT materialIndex)
{
    if (materialIndex >= g_mesh.materials.size())
        return false;
    std::string name = g_mesh.materials[materialIndex].name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name.find("floor") != std::string::npos;
}

void ComputeSceneMeasurements()
{
    if (g_mesh.vertices.empty())
        return;

    XMVECTOR bmin = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 0.f);
    XMVECTOR bmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.f);
    const XMMATRIX world = MeshWorldTransform();

    for (const Obj::MeshVertex& v : g_mesh.vertices)
    {
        const XMFLOAT3 p{v.px, v.py, v.pz};
        const XMVECTOR wp = XMVector3TransformCoord(XMLoadFloat3(&p), world);
        bmin = XMVectorMin(bmin, wp);
        bmax = XMVectorMax(bmax, wp);
    }

    const XMVECTOR center = XMVectorScale(XMVectorAdd(bmin, bmax), 0.5f);
    const XMVECTOR extent = XMVectorSubtract(bmax, bmin);
    XMStoreFloat3(&g_sceneCenter, center);
    g_sceneRadius = XMVectorGetX(XMVector3Length(extent)) * 0.5f + 2.f;

    float upperFloor = -FLT_MAX;
    for (const Obj::Submesh& submesh : g_mesh.submeshes)
    {
        if (!IsFloorMaterial(submesh.materialIndex))
            continue;
        const UINT end = submesh.indexStart + submesh.indexCount;
        for (UINT i = submesh.indexStart; i < end && i < g_mesh.indices.size(); ++i)
        {
            const UINT vertexIndex = g_mesh.indices[i];
            if (vertexIndex >= g_mesh.vertices.size())
                continue;
            const Obj::MeshVertex& vertex = g_mesh.vertices[vertexIndex];
            const XMVECTOR position = XMVector3TransformCoord(
                XMVectorSet(vertex.px, vertex.py, vertex.pz, 1.f), world);
            upperFloor = (std::max)(upperFloor, XMVectorGetY(position));
        }
    }
    // The known Sponza has two floor levels; the upper one is the maximum Y.
    // Fall back to the bottom of the scene only if the floor material is absent.
    g_particleFloorY = upperFloor != -FLT_MAX ? upperFloor : XMVectorGetY(bmin);
}

bool LoadScene()
{
    g_sceneReady = false;
    g_mesh = {};
    g_matSrvPairBase.clear();
    g_gpuTextures.clear();
    g_whiteTexture.Reset();
    g_meshVB.Reset();
    g_meshIB.Reset();
    g_matCBUpload.Reset();
    g_matCBMapped = nullptr;
    g_matCount = 0;

    const std::filesystem::path objPath = FindSponzaObj();
    if (objPath.empty())
    {
        MessageBoxW(
            g_hwnd,
            L"Не найден sponza.obj.\n\nСкопируйте папку Sponza (sponza.obj + MTL + текстуры) в каталог exe:\n"
            L"  <папка_с_exe>\\Sponza\\sponza.obj",
            L"Sponza",
            MB_OK | MB_ICONWARNING);
        return false;
    }

    std::wstring err;
    if (!Obj::LoadObj(objPath, g_mesh, err))
    {
        MessageBoxW(g_hwnd, err.c_str(), L"OBJ", MB_OK | MB_ICONERROR);
        return false;
    }

    if (g_mesh.vertices.empty() || g_mesh.indices.empty() || g_mesh.submeshes.empty())
    {
        MessageBoxW(g_hwnd, L"OBJ пустой или без граней.", L"OBJ", MB_OK | MB_ICONERROR);
        return false;
    }

    const UINT vbSize = static_cast<UINT>(g_mesh.vertices.size() * sizeof(Obj::MeshVertex));
    const UINT ibSize = static_cast<UINT>(g_mesh.indices.size() * sizeof(uint32_t));
    g_meshVB = CreateUploadBuffer(g_mesh.vertices.data(), vbSize);
    g_meshIB = CreateUploadBuffer(g_mesh.indices.data(), ibSize);

    g_meshVbv.BufferLocation = g_meshVB->GetGPUVirtualAddress();
    g_meshVbv.SizeInBytes = vbSize;
    g_meshVbv.StrideInBytes = sizeof(Obj::MeshVertex);
    g_meshIbv.BufferLocation = g_meshIB->GetGPUVirtualAddress();
    g_meshIbv.SizeInBytes = ibSize;
    g_meshIbv.Format = DXGI_FORMAT_R32_UINT;

    const std::filesystem::path mtlDir = objPath.parent_path();
    g_matSrvPairBase.assign(g_mesh.materials.size(), 0);
    std::vector<uint8_t> matHasSpecularTex(g_mesh.materials.size(), 0);

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), nullptr));

    std::vector<ComPtr<ID3D12Resource>> uploadKeep;
    uint32_t nextSlot = 0;

    ComPtr<ID3D12Resource> whiteTex;
    if (!Tex::CreateSolidTexture2D(
            g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), nextSlot, g_srvDescriptorSize, 0xFFFFFFFFu,
            whiteTex, uploadKeep))
    {
        MessageBoxW(g_hwnd, L"Не удалось создать текстуру по умолчанию.", L"Текстуры", MB_OK | MB_ICONERROR);
        return false;
    }
    g_whiteTexture = whiteTex;
    g_gpuTextures.push_back(whiteTex);
    Tex::WriteTexture2DSrv(
        g_device.Get(), whiteTex.Get(), g_srvHeap.Get(), nextSlot + 1, g_srvDescriptorSize);
    nextSlot += 2;

    std::unordered_map<std::wstring, ComPtr<ID3D12Resource>> texCache;

    auto bindTextureSlot = [&](UINT slot, const std::filesystem::path& texPath) -> bool {
        if (!std::filesystem::exists(texPath))
        {
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), slot, g_srvDescriptorSize);
            return false;
        }
        const std::wstring key = texPath.lexically_normal().wstring();
        const auto cached = texCache.find(key);
        if (cached != texCache.end())
        {
            Tex::WriteTexture2DSrv(
                g_device.Get(), cached->second.Get(), g_srvHeap.Get(), slot, g_srvDescriptorSize);
            return true;
        }

        ComPtr<ID3D12Resource> texRes;
        std::wstring terr;
        if (!Tex::CreateTexture2DFromFile(
                g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), slot, g_srvDescriptorSize, texPath,
                texRes, uploadKeep, terr))
        {
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), slot, g_srvDescriptorSize);
            return false;
        }

        g_gpuTextures.push_back(texRes);
        texCache[key] = texRes;
        return true;
    };

    for (size_t i = 0; i < g_mesh.materials.size(); ++i)
    {
        // Slots [kDeferredSrvBase, ...] are reserved for the G-buffer,
        // shadow map and particle UAV/SRV descriptors.
        if (nextSlot + 2u > kDeferredSrvBase)
        {
            MessageBoxW(
                g_hwnd, L"Слишком много текстур: достигнута зарезервированная область SRV.",
                L"Текстуры", MB_OK | MB_ICONWARNING);
            break;
        }

        const uint32_t pairBase = nextSlot;
        nextSlot += 2;
        g_matSrvPairBase[i] = pairBase;

        const Obj::Material& m = g_mesh.materials[i];

        if (m.diffuseMapRel.empty())
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), pairBase, g_srvDescriptorSize);
        else
            bindTextureSlot(pairBase, Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.diffuseMapRel));

        bool specLoaded = false;
        if (m.specularMapRel.empty())
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), pairBase + 1, g_srvDescriptorSize);
        else
            specLoaded =
                bindTextureSlot(pairBase + 1, Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.specularMapRel));

        matHasSpecularTex[i] = specLoaded ? 1 : 0;
    }

    ExecuteCommandList();
    uploadKeep.clear();

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), g_pipelineGeo.Get()));
    ThrowIfFailed(g_cmdList->Close());

    g_matCount = static_cast<UINT>(g_mesh.materials.size());
    if (g_matCount == 0)
        g_matCount = 1;

    const UINT64 matBufSize = static_cast<UINT64>(g_matCount) * kCbAlign;
    g_matCBUpload = CreateUploadBuffer(nullptr, matBufSize);
    D3D12_RANGE mr{0, 0};
    ThrowIfFailed(g_matCBUpload->Map(0, &mr, reinterpret_cast<void**>(&g_matCBMapped)));

    for (UINT i = 0; i < g_matCount; ++i)
    {
        MatCBGPU* slot = reinterpret_cast<MatCBGPU*>(g_matCBMapped + static_cast<size_t>(i) * kCbAlign);
        std::memset(slot, 0, sizeof(MatCBGPU));
        if (i < g_mesh.materials.size())
        {
            const Obj::Material& mm = g_mesh.materials[i];
            slot->Kd = XMFLOAT4(mm.Kd[0], mm.Kd[1], mm.Kd[2], 1.0f);
            slot->UvScale = XMFLOAT2(mm.uvScale[0], mm.uvScale[1]);
            slot->UvOffset = XMFLOAT2(mm.uvOffset[0], mm.uvOffset[1]);
            slot->Ks = XMFLOAT3(mm.Ks[0], mm.Ks[1], mm.Ks[2]);
            slot->Ns = mm.Ns;
            slot->UseUvAnim = MaterialPathSuggestUvAnim(mm.diffuseMapRel) ? 1u : 0u;
            slot->HasSpecularTex = matHasSpecularTex[i];
            slot->UseSwayAnim = MaterialPathSuggestSway(mm.diffuseMapRel) ? 1u : 0u;
        }
        else
        {
            slot->Kd = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
            slot->UvScale = XMFLOAT2(1.0f, 1.0f);
            slot->UvOffset = XMFLOAT2(0.0f, 0.0f);
            slot->Ks = XMFLOAT3(0.2f, 0.2f, 0.2f);
            slot->Ns = 32.0f;
        }
    }

    g_sceneReady = true;
    ComputeSceneMeasurements();
    return true;
}

void CreateCubes()
{
    std::vector<Obj::MeshVertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(kCubeCount * 24u);
    indices.reserve(kCubeCount * 36u);

    std::mt19937 random(0xC0BEEu);
    std::uniform_real_distribution<float> xDistribution(-8.0f, 8.0f);
    std::uniform_real_distribution<float> zDistribution(-5.0f, 5.0f);
    std::uniform_real_distribution<float> heightDistribution(0.0f, 8.0f);
    std::uniform_real_distribution<float> sizeDistribution(0.18f, 0.48f);
    std::uniform_real_distribution<float> colorDistribution(0.18f, 1.0f);

    const XMFLOAT3 normals[6] = {
        {1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
        {0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};
    const XMFLOAT3 tangents[6] = {
        {0.f, 0.f, -1.f}, {0.f, 0.f, 1.f}, {1.f, 0.f, 0.f},
        {1.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}};
    const XMFLOAT3 bitangents[6] = {
        {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f},
        {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}};
    const XMFLOAT2 uv[4] = {{0.f, 1.f}, {1.f, 1.f}, {1.f, 0.f}, {0.f, 0.f}};
    const float cornerSigns[4][2] = {{-1.f, -1.f}, {1.f, -1.f}, {1.f, 1.f}, {-1.f, 1.f}};

    g_cubeMatCBUpload = CreateUploadBuffer(nullptr, static_cast<UINT64>(kCubeCount) * kCbAlign);
    D3D12_RANGE noRead{0, 0};
    ThrowIfFailed(g_cubeMatCBUpload->Map(
        0, &noRead, reinterpret_cast<void**>(&g_cubeMatCBMapped)));

    for (UINT cube = 0; cube < kCubeCount; ++cube)
    {
        const float side = sizeDistribution(random);
        const float half = side * 0.5f;
        const XMFLOAT3 center{
            xDistribution(random),
            g_particleFloorY + half + 0.01f + heightDistribution(random),
            zDistribution(random)};

        const uint32_t cubeVertexStart = static_cast<uint32_t>(vertices.size());
        for (UINT face = 0; face < 6; ++face)
        {
            const XMFLOAT3& n = normals[face];
            const XMFLOAT3& t = tangents[face];
            const XMFLOAT3& b = bitangents[face];
            for (UINT corner = 0; corner < 4; ++corner)
            {
                const float ts = cornerSigns[corner][0];
                const float bs = cornerSigns[corner][1];
                Obj::MeshVertex vertex{};
                vertex.px = center.x + (n.x + t.x * ts + b.x * bs) * half;
                vertex.py = center.y + (n.y + t.y * ts + b.y * bs) * half;
                vertex.pz = center.z + (n.z + t.z * ts + b.z * bs) * half;
                vertex.nx = n.x;
                vertex.ny = n.y;
                vertex.nz = n.z;
                vertex.u = uv[corner].x;
                vertex.v = uv[corner].y;
                vertices.push_back(vertex);
            }
            const uint32_t base = cubeVertexStart + face * 4u;
            indices.insert(indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
        }

        auto* material = reinterpret_cast<MatCBGPU*>(
            g_cubeMatCBMapped + static_cast<size_t>(cube) * kCbAlign);
        std::memset(material, 0, sizeof(MatCBGPU));
        material->Kd = XMFLOAT4(
            colorDistribution(random), colorDistribution(random), colorDistribution(random), 1.f);
        material->UvScale = XMFLOAT2(1.f, 1.f);
        material->Ks = XMFLOAT3(0.12f, 0.12f, 0.12f);
        material->Ns = 24.f;
    }

    const UINT vbSize = static_cast<UINT>(vertices.size() * sizeof(Obj::MeshVertex));
    const UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    g_cubeVB = CreateUploadBuffer(vertices.data(), vbSize);
    g_cubeIB = CreateUploadBuffer(indices.data(), ibSize);
    g_cubeVbv = {g_cubeVB->GetGPUVirtualAddress(), vbSize, sizeof(Obj::MeshVertex)};
    g_cubeIbv = {g_cubeIB->GetGPUVirtualAddress(), ibSize, DXGI_FORMAT_R32_UINT};

    g_cubeFrameCBUpload = CreateUploadBuffer(nullptr, sizeof(FrameCB));
    ThrowIfFailed(g_cubeFrameCBUpload->Map(
        0, &noRead, reinterpret_cast<void**>(&g_cubeFrameCBMapped)));
}

void WriteFrameCBTo(UINT8* destination, const XMMATRIX& world, const XMMATRIX& viewProj, float timeSec)
{
    FrameCB data{};
    XMStoreFloat4x4(&data.World, world);
    XMStoreFloat4x4(&data.ViewProj, viewProj);
    data.TimeCamPos =
        XMFLOAT4(timeSec, g_camPos.x, g_camPos.y, g_camPos.z);
    // xy = UV scroll; z = sway amplitude (local OBJ units); w = sway speed scale
    data.UvAnimAndPad = XMFLOAT4(0.035f, 0.022f, 5.5f, 1.0f);
    std::memcpy(destination, &data, sizeof(FrameCB));
}

void WriteFrameCB(const XMMATRIX& world, const XMMATRIX& viewProj, float timeSec)
{
    WriteFrameCBTo(g_frameCBMapped, world, viewProj, timeSec);
}

XMMATRIX CalcView()
{
    const XMVECTOR eye = XMLoadFloat3(&g_camPos);
    const XMVECTOR dir = XMVector3Normalize(XMVectorSet(
        sinf(g_camYaw) * cosf(g_camPitch), sinf(g_camPitch), cosf(g_camYaw) * cosf(g_camPitch), 0.0f));
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookToLH(eye, dir, up);
}

XMMATRIX CalcProj()
{
    const float aspect = static_cast<float>(g_width) / static_cast<float>((std::max)(1u, g_height));
    return XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, ShadowSystem::kCameraNear, ShadowSystem::kCameraFar);
}

XMMATRIX CalcViewProj()
{
    return CalcView() * CalcProj();
}

XMFLOAT3 ParticleEmitterPosition()
{
    // Vertical offset of the emitter relative to the detected upper floor.
    constexpr float spawnClearance = -1.00f;
    return XMFLOAT3(
        g_sceneCenter.x, g_particleFloorY + spawnClearance, g_sceneCenter.z);
}

void UpdateCamera(float dt)
{
    if (!g_hwnd || dt <= 0.0f)
        return;

    constexpr float moveSpeed = 4.0f;
    constexpr float lookSpeed = 0.0022f;

    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0)
    {
        RECT cr{};
        GetClientRect(g_hwnd, &cr);
        POINT center{(cr.right - cr.left) / 2, (cr.bottom - cr.top) / 2};
        ClientToScreen(g_hwnd, &center);
        POINT cur{};
        GetCursorPos(&cur);
        if (g_camPrevRmb)
        {
            g_camYaw += static_cast<float>(cur.x - center.x) * lookSpeed;
            g_camPitch -= static_cast<float>(cur.y - center.y) * lookSpeed;
            g_camPitch = std::clamp(g_camPitch, -XM_PIDIV2 + 0.02f, XM_PIDIV2 - 0.02f);
        }
        SetCursorPos(center.x, center.y);
        g_camPrevRmb = true;

        POINT ul{0, 0};
        ClientToScreen(g_hwnd, &ul);
        POINT br{cr.right, cr.bottom};
        ClientToScreen(g_hwnd, &br);
        const RECT clip{ul.x, ul.y, br.x, br.y};
        ClipCursor(&clip);
    }
    else
    {
        ClipCursor(nullptr);
        g_camPrevRmb = false;
    }

    float moveX = 0.0f;
    float moveZ = 0.0f;
    float moveY = 0.0f;
    if ((GetAsyncKeyState('W') & 0x8000) != 0)
        moveZ += 1.0f;
    if ((GetAsyncKeyState('S') & 0x8000) != 0)
        moveZ -= 1.0f;
    if ((GetAsyncKeyState('D') & 0x8000) != 0)
        moveX += 1.0f;
    if ((GetAsyncKeyState('A') & 0x8000) != 0)
        moveX -= 1.0f;
    if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
        moveY += 1.0f;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        moveY -= 1.0f;

    const XMVECTOR forward = XMVector3Normalize(XMVectorSet(
        sinf(g_camYaw) * cosf(g_camPitch), sinf(g_camPitch), cosf(g_camYaw) * cosf(g_camPitch), 0.0f));
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
    XMVECTOR delta =
        XMVectorScale(forward, moveZ) + XMVectorScale(right, moveX) + XMVectorScale(worldUp, moveY);
    if (XMVectorGetX(XMVector3LengthSq(delta)) > 1e-8f)
    {
        delta = XMVector3Normalize(delta);
        XMVECTOR pos = XMLoadFloat3(&g_camPos);
        pos = XMVectorAdd(pos, XMVectorScale(delta, moveSpeed * dt));
        XMStoreFloat3(&g_camPos, pos);
    }
}

void DrawScene(const XMMATRIX& viewProj)
{
    if (!g_sceneReady)
        return;

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_cmdList->SetDescriptorHeaps(1, heaps);
    g_cmdList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_cmdList->SetPipelineState(g_pipelineGeo.Get());

    const XMMATRIX world = MeshWorldTransform();
    WriteFrameCB(world, viewProj, g_appTime);
    g_cmdList->SetGraphicsRootConstantBufferView(0, g_frameCBUpload->GetGPUVirtualAddress());

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->IASetVertexBuffers(0, 1, &g_meshVbv);
    g_cmdList->IASetIndexBuffer(&g_meshIbv);

    const D3D12_GPU_DESCRIPTOR_HANDLE srvHeapStart = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    for (const Obj::Submesh& sm : g_mesh.submeshes)
    {
        if (sm.materialIndex >= g_matSrvPairBase.size())
            continue;

        g_cmdList->SetGraphicsRootConstantBufferView(
            1, g_matCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(sm.materialIndex) * kCbAlign);

        const UINT pairBase = g_matSrvPairBase[sm.materialIndex];
        D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapStart;
        table.ptr += static_cast<SIZE_T>(pairBase) * g_srvDescriptorSize;
        g_cmdList->SetGraphicsRootDescriptorTable(2, table);
        g_cmdList->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
    }
}

void DrawCubes(const XMMATRIX& viewProj)
{
    if (!g_cubeVB || !g_cubeIB || !g_cubeMatCBUpload)
        return;

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_cmdList->SetDescriptorHeaps(1, heaps);
    g_cmdList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_cmdList->SetPipelineState(g_pipelineGeo.Get());

    WriteFrameCBTo(g_cubeFrameCBMapped, XMMatrixIdentity(), viewProj, g_appTime);
    g_cmdList->SetGraphicsRootConstantBufferView(
        0, g_cubeFrameCBUpload->GetGPUVirtualAddress());
    g_cmdList->SetGraphicsRootDescriptorTable(
        2, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->IASetVertexBuffers(0, 1, &g_cubeVbv);
    g_cmdList->IASetIndexBuffer(&g_cubeIbv);

    for (UINT cube = 0; cube < kCubeCount; ++cube)
    {
        g_cmdList->SetGraphicsRootConstantBufferView(
            1, g_cubeMatCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(cube) * kCbAlign);
        g_cmdList->DrawIndexedInstanced(36, 1, cube * 36u, 0, 0);
    }
}

void DrawSceneDepth(const XMMATRIX& lightViewProj)
{
    if (!g_sceneReady)
        return;

    const XMMATRIX world = MeshWorldTransform();
    WriteFrameCB(world, lightViewProj, g_appTime);
    g_cmdList->SetGraphicsRootConstantBufferView(0, g_frameCBUpload->GetGPUVirtualAddress());

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->IASetVertexBuffers(0, 1, &g_meshVbv);
    g_cmdList->IASetIndexBuffer(&g_meshIbv);

    for (const Obj::Submesh& sm : g_mesh.submeshes)
        g_cmdList->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
}

void DrawFrame(float dt)
{
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    const UINT64 fenceToWait = g_frameFenceValues[g_frameIndex];
    if (g_fence->GetCompletedValue() < fenceToWait)
    {
        ThrowIfFailed(g_fence->SetEventOnCompletion(fenceToWait, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    UpdateCamera(dt);
    g_appTime += dt;

    const XMMATRIX view = CalcView();
    const XMMATRIX proj = CalcProj();
    const XMMATRIX viewProj = view * proj;

    g_shadowSys.UpdateCascades(
        view, proj, g_camPos, g_renderSys.SunDirection(), g_sceneCenter, g_sceneRadius);

    ThrowIfFailed(g_cmdAlloc[g_frameIndex]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[g_frameIndex].Get(), g_pipelineGeo.Get()));

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_cmdList->SetDescriptorHeaps(1, heaps);

    g_shadowSys.DrawShadowPass(g_cmdList.Get(), [](const XMMATRIX& lightViewProj) {
        DrawSceneDepth(lightViewProj);
    });
    g_shadowSys.TransitionToShaderResource(g_cmdList.Get());

    GBuffer& gb = g_renderSys.GBufferTargets();
    gb.TransitionToRenderTargets(g_cmdList.Get());

    const float gbClearRgb[] = {0.06f, 0.07f, 0.10f};
    gb.ClearAndSetAsRenderTarget(g_cmdList.Get(), gbClearRgb);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);

    DrawScene(viewProj);
    DrawCubes(viewProj);
    g_particleSys.UpdateAndDraw(
        g_cmdList.Get(), g_srvHeap.Get(), g_frameIndex, dt, g_appTime, view, viewProj,
        g_camPos, ParticleEmitterPosition(), g_particleFloorY);

    gb.TransitionToShaderResource(g_cmdList.Get());

    const D3D12_CPU_DESCRIPTOR_HANDLE sceneRtv = g_postProcessSys.BeginScene(g_cmdList.Get());
    g_renderSys.UploadFrameConstants(g_camPos, g_width, g_height);
    g_renderSys.DrawLightingPass(
        g_cmdList.Get(), g_srvHeap.Get(), g_shadowSys, sceneRtv, g_width, g_height);

    ComPtr<ID3D12Resource> backBuffer = g_renderTargets[g_frameIndex];
    const D3D12_RESOURCE_STATES rtBefore =
        g_swapSeenPresent[g_frameIndex] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER toRt = MakeTransition(backBuffer.Get(), rtBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    g_postProcessSys.Apply(g_cmdList.Get(), g_srvHeap.Get(), rtv, g_width, g_height);

    D3D12_RESOURCE_BARRIER toPresent =
        MakeTransition(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_cmdList->ResourceBarrier(1, &toPresent);
    g_swapSeenPresent[g_frameIndex] = true;

    ThrowIfFailed(g_cmdList->Close());
    ID3D12CommandList* lists[] = {g_cmdList.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    ThrowIfFailed(g_swapChain->Present(1, 0));

    const UINT64 signalValue = ++g_fenceValue;
    ThrowIfFailed(g_queue->Signal(g_fence.Get(), signalValue));
    g_frameFenceValues[g_frameIndex] = signalValue;
}

void InitD3D(HWND hwnd)
{
    UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&g_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; g_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
            break;
        adapter.Reset();
    }
    if (!g_device)
        ThrowIfFailed(E_FAIL);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_queue)));

    DXGI_SWAP_CHAIN_DESC1 swapDesc{};
    swapDesc.Width = g_width;
    swapDesc.Height = g_height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = kFrameCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(g_factory->CreateSwapChainForHwnd(
        g_queue.Get(), hwnd, &swapDesc, nullptr, nullptr, &swapChain1));
    ThrowIfFailed(swapChain1.As(&g_swapChain));
    ThrowIfFailed(g_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)));
    CreateRtv();

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_dsvHeap)));
    CreateDepth();

    for (UINT i = 0; i < kFrameCount; ++i)
        ThrowIfFailed(g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmdAlloc[i])));
    ThrowIfFailed(g_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&g_cmdList)));
    ThrowIfFailed(g_cmdList->Close());

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent)
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

    CreateFrameCB();
    CreateSrvHeap();
    CreateGeometryPipeline();
    g_renderSys.Init(
        g_device.Get(),
        g_width,
        g_height,
        g_srvHeap.Get(),
        kDeferredSrvBase,
        kShadowSrvBase,
        g_srvDescriptorSize,
        DeferredShaderPath().c_str());
    g_shadowSys.Init(
        g_device.Get(),
        g_srvHeap.Get(),
        kShadowSrvBase,
        g_srvDescriptorSize,
        DeferredShaderPath().c_str());
    g_postProcessSys.Init(
        g_device.Get(), g_width, g_height, g_srvHeap.Get(), kPostProcessSrvBase,
        g_srvDescriptorSize, PostProcessShaderPath().c_str());
    LoadScene();
    CreateCubes();

    // Particle buffers live in the default heap, so their initial contents and
    // Append/Consume counters are uploaded once through the command list.
    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), nullptr));
    g_particleSys.Init(
        g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), kParticleSrvBase,
        g_srvDescriptorSize, ParticleShaderPath().c_str(), ParticleEmitterPosition());
    ExecuteCommandList();
    g_particleSys.ReleaseUploadResources();
}

void ShutdownD3D()
{
    WaitForGpu();
    if (g_fenceEvent)
    {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            g_running = false;
        return 0;
    case WM_SIZE:
        if (g_swapChain && wp != SIZE_MINIMIZED)
            ResizeSwapChain(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return static_cast<int>(coHr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SecondSemCG_Sponza";
    RegisterClassExW(&wc);

    RECT windowRect{0, 0, static_cast<LONG>(kClientW), static_cast<LONG>(kClientH)};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"SecondSem CG — Sponza: текстуры, MTL, тайлинг, UV-анимация", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd)
        return static_cast<int>(HRESULT_FROM_WIN32(GetLastError()));

    InitD3D(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (g_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running)
            break;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (g_qpcFreq.QuadPart == 0)
            QueryPerformanceFrequency(&g_qpcFreq);
        float dt = static_cast<float>(now.QuadPart - g_qpcLast.QuadPart) /
                   static_cast<float>(g_qpcFreq.QuadPart);
        g_qpcLast = now;
        if (dt > 0.1f)
            dt = 0.1f;
        DrawFrame(dt);
    }

    ShutdownD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(coHr))
        CoUninitialize();
    return static_cast<int>(msg.wParam);
}
