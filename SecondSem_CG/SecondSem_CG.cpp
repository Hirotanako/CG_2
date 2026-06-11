// DirectX 12: Sponza + 10000 кубов, deferred, frustum/octree culling.

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
#include "RenderingSystem.h"
#include "SceneCulling.h"
#include "TextureUtil.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
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
constexpr UINT kCbAlign = 256;

constexpr UINT kSceneCubeCount = 10000;
constexpr UINT kOctreeCubeCount = 500;
constexpr UINT kSponzaFrameSlot = 0;
constexpr UINT kCubeFrameBase = 1;
constexpr UINT kFrameCbSlotCount = kCubeFrameBase + kSceneCubeCount;

struct alignas(256) FrameCB
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProj;
    XMFLOAT4X4 PrevViewProj;
    XMFLOAT4 TimeCamPos;
    XMFLOAT4 UvAnimAndPad;
    XMFLOAT4 TessParams;
    XMFLOAT4 DebugView;
};

static_assert(sizeof(FrameCB) == 256);

struct alignas(256) MatCBGPU
{
    XMFLOAT4 Kd;
    XMFLOAT2 UvScale;
    XMFLOAT2 UvOffset;
    XMFLOAT4 MatFlags;
    XMFLOAT4 MatFlags2;
    float Ns;
    float SpecIntensity;
    float _PadMat[2];
    float _PadMatRest[44];
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
ComPtr<ID3D12PipelineState> g_pipelineGeoWire;
bool g_debugFrameView = false;
bool g_frustumCulling = true;
bool g_octreeCulling = true;
bool g_distanceCulling = true;
float g_maxDrawDistance = 42.0f;
bool g_octreeViz = false;

ComPtr<ID3D12RootSignature> g_octreeVizRootSig;
ComPtr<ID3D12PipelineState> g_octreeVizPso;
ComPtr<ID3D12Resource> g_octreeVizNodeUpload;
ComPtr<ID3D12Resource> g_octreeVizCbUpload;
UINT8* g_octreeVizCbMapped = nullptr;
UINT8* g_octreeVizNodeMapped = nullptr;
std::vector<Scene::Aabb> g_octreeVisitedNodes;
UINT g_octreeVizNodeCount = 0;
UINT g_octreeVizSrvSlot = 0;
constexpr UINT kOctreeVizMaxNodes = 16384;

struct alignas(256) OctreeVizCB
{
    XMFLOAT4X4 ViewProj;
    XMFLOAT4 LineColor;
    UINT NodeCount;
    UINT _Pad[3];
};

static_assert(sizeof(OctreeVizCB) == 256);

RenderingSystem g_renderSys;

ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvDescriptorSize = 0;

struct GpuMesh
{
    ComPtr<ID3D12Resource> vb;
    ComPtr<ID3D12Resource> ib;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    UINT indexCount = 0;
};

bool g_sceneReady = false;
bool g_hasSponza = false;
UINT g_sponzaMatCount = 0;
UINT g_cubeMatBase = 0;
UINT g_cubeSrvBase = 0;
Obj::LoadedMesh g_mesh{};
ComPtr<ID3D12Resource> g_meshVB;
ComPtr<ID3D12Resource> g_meshIB;
D3D12_VERTEX_BUFFER_VIEW g_meshVbv{};
D3D12_INDEX_BUFFER_VIEW g_meshIbv{};
std::vector<uint32_t> g_matSrvPairBase;
std::vector<ComPtr<ID3D12Resource>> g_gpuTextures;
bool g_sceneIsCliffRock = false;

GpuMesh g_gpuCube{};
std::vector<Scene::SceneObject> g_sceneObjects;
Scene::Octree g_octree;
Scene::Aabb g_sceneBounds{};
std::vector<uint32_t> g_visibleIndices;
UINT g_lastVisibleCount = 0;

ComPtr<ID3D12Resource> g_whiteTexture;
ComPtr<ID3D12Resource> g_grayDispTexture;
float g_tessFarDist = 55.0f;
ComPtr<ID3D12Resource> g_matCBUpload;
UINT8* g_matCBMapped = nullptr;
UINT g_matCount = 0;

ComPtr<ID3D12Resource> g_frameCBUpload;
UINT8* g_frameCBMapped = nullptr;
XMFLOAT4X4 g_prevViewProj{};

UINT g_frameIndex = 0;
float g_appTime = 0.0f;
uint32_t g_animFrameIndex = 0;

XMFLOAT3 g_camPos{0.0f, 1.4f, 4.5f};
float g_camYaw = 0.0f;
float g_camPitch = -0.12f;
bool g_camPrevRmb = false;

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

std::wstring FindShaderPath(const wchar_t* fileName)
{
    const std::wstring inExeDir = ExeDirectory() + fileName;
    if (std::filesystem::exists(inExeDir))
        return inExeDir;

    const std::wstring inProjectDir = ExeDirectory() + L"..\\..\\" + fileName;
    if (std::filesystem::exists(inProjectDir))
        return std::filesystem::weakly_canonical(inProjectDir).wstring();

    return inExeDir;
}

std::wstring OctreeVizShaderPath()
{
    return FindShaderPath(L"OctreeViz.hlsl");
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
        {
            const char* msg = static_cast<const char*>(err->GetBufferPointer());
            OutputDebugStringA(msg);
            MessageBoxA(nullptr, msg, "Shader compile error", MB_OK | MB_ICONERROR);
        }
        else
        {
            wchar_t buf[128];
            swprintf_s(buf, L"Shader compile failed: %s\nHRESULT 0x%08X", path, static_cast<unsigned>(hr));
            MessageBoxW(nullptr, buf, L"SecondSem CG", MB_OK | MB_ICONERROR);
        }
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
    const UINT64 frameBufSize = static_cast<UINT64>(kFrameCbSlotCount) * kCbAlign;
    g_frameCBUpload = CreateUploadBuffer(nullptr, frameBufSize);
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
    range.NumDescriptors = 3;
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
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    ComPtr<ID3DBlob> vs, hs, ds, ps;
    CompileShader(sp.c_str(), "GeometryVS", "vs_5_0", vs);
    CompileShader(sp.c_str(), "HSMain", "hs_5_0", hs);
    CompileShader(sp.c_str(), "DSMain", "ds_5_0", ds);
    CompileShader(sp.c_str(), "GeometryPS", "ps_5_0", ps);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.HS = {hs->GetBufferPointer(), hs->GetBufferSize()};
    pso.DS = {ds->GetBufferPointer(), ds->GetBufferSize()};
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
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = 3;
    pso.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pso.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {layout, _countof(layout)};

    auto createGeoPso = [&](D3D12_FILL_MODE fill, ComPtr<ID3D12PipelineState>& out) {
        pso.RasterizerState.FillMode = fill;
        ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&out)));
    };
    createGeoPso(D3D12_FILL_MODE_SOLID, g_pipelineGeo);
    createGeoPso(D3D12_FILL_MODE_WIREFRAME, g_pipelineGeoWire);
}

void BindOctreeVizSrv(UINT slot)
{
    g_octreeVizSrvSlot = slot;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = kOctreeVizMaxNodes;
    srvDesc.Buffer.StructureByteStride = sizeof(Scene::OctreeNodeGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += static_cast<SIZE_T>(g_octreeVizSrvSlot) * g_srvDescriptorSize;
    g_device->CreateShaderResourceView(g_octreeVizNodeUpload.Get(), &srvDesc, srvCpu);
}

void CreateOctreeVizPipeline()
{
    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, rsErr;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &rsErr));
    ThrowIfFailed(g_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_octreeVizRootSig)));

    const std::wstring sp = OctreeVizShaderPath();
    ComPtr<ID3DBlob> vs, ps;
    CompileShader(sp.c_str(), "OctreeWireVS", "vs_5_0", vs);
    CompileShader(sp.c_str(), "OctreeWirePS", "ps_5_0", ps);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_octreeVizRootSig.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = FALSE;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.AntialiasedLineEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {nullptr, 0};

    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_octreeVizPso)));

    g_octreeVizCbUpload = CreateUploadBuffer(nullptr, sizeof(OctreeVizCB));
    D3D12_RANGE rr{0, 0};
    ThrowIfFailed(g_octreeVizCbUpload->Map(0, &rr, reinterpret_cast<void**>(&g_octreeVizCbMapped)));

    const UINT64 nodeBufSize =
        static_cast<UINT64>(sizeof(Scene::OctreeNodeGpu)) * static_cast<UINT64>(kOctreeVizMaxNodes);
    g_octreeVizNodeUpload = CreateUploadBuffer(nullptr, nodeBufSize);
    D3D12_RANGE nr{0, 0};
    ThrowIfFailed(g_octreeVizNodeUpload->Map(0, &nr, reinterpret_cast<void**>(&g_octreeVizNodeMapped)));
}

static void CollectSearchRoots(std::vector<std::filesystem::path>& roots)
{
    auto tryAdd = [&](std::filesystem::path p) {
        if (p.empty())
            return;
        try
        {
            p = std::filesystem::weakly_canonical(p);
        }
        catch (...)
        {
            return;
        }
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
}

static bool PathLooksLikeCliffRock(const std::filesystem::path& objPath)
{
    std::wstring s = objPath.filename().wstring();
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
    return s.find(L"cliff") != std::wstring::npos;
}

static std::filesystem::path FindSponzaObj()
{
    const std::wstring sponzaRels[] = {
        L"Sponza\\sponza.obj",
        L"Sponza/sponza.obj",
        L"sponza.obj",
    };
    const std::wstring cliffRels[] = {
        L"Cliffrock_0006_120kHigh.obj",
        L"cliffrock/Cliffrock_0006_120kHigh.obj",
    };

    std::vector<std::filesystem::path> roots;
    CollectSearchRoots(roots);

    auto tryRels = [&](const std::wstring* rels, size_t count) -> std::filesystem::path {
        for (const auto& root : roots)
        {
            if (root.empty())
                continue;
            for (size_t i = 0; i < count; ++i)
            {
                const std::filesystem::path candidate = root / rels[i];
                if (std::filesystem::exists(candidate))
                    return candidate;
            }
        }
        return {};
    };

    std::filesystem::path p = tryRels(sponzaRels, _countof(sponzaRels));
    if (!p.empty())
        return p;
    return tryRels(cliffRels, _countof(cliffRels));
}

static std::filesystem::path ResolveNormalPath(
    const std::filesystem::path& mtlDir, const Obj::Material& m)
{
    if (!m.normalMapRel.empty())
    {
        const auto p = Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.normalMapRel);
        if (!p.empty())
            return p;
    }
    if (!m.displacementMapRel.empty())
        return Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.displacementMapRel);
    return {};
}

static std::filesystem::path ResolveDisplacementPath(
    const std::filesystem::path& mtlDir, const Obj::Material& m, bool& outUseRoughnessAsHeight)
{
    outUseRoughnessAsHeight = false;
    if (!m.displacementMapRel.empty())
    {
        const auto p = Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.displacementMapRel);
        if (!p.empty())
            return p;
    }
    if (!m.roughnessMapRel.empty())
    {
        const auto p = Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.roughnessMapRel);
        if (!p.empty())
        {
            outUseRoughnessAsHeight = true;
            return p;
        }
    }
    const auto fromNormal = ResolveNormalPath(mtlDir, m);
    if (!fromNormal.empty())
        return fromNormal;
    return {};
}

XMMATRIX SponzaWorldTransform()
{
    return g_sceneIsCliffRock
        ? XMMatrixIdentity()
        : XMMatrixScaling(0.01f, 0.01f, 0.01f) * XMMatrixRotationX(XM_PI);
}

bool ComputeSponzaWorldBounds(Scene::Aabb& outBounds)
{
    if (!g_hasSponza || g_mesh.vertices.empty())
        return false;

    const XMMATRIX worldXform = SponzaWorldTransform();
    XMFLOAT3 wmin{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 wmax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const Obj::MeshVertex& v : g_mesh.vertices)
    {
        const XMVECTOR wp = XMVector3TransformCoord(XMVectorSet(v.px, v.py, v.pz, 1.0f), worldXform);
        XMFLOAT3 p{};
        XMStoreFloat3(&p, wp);
        wmin.x = (std::min)(wmin.x, p.x);
        wmin.y = (std::min)(wmin.y, p.y);
        wmin.z = (std::min)(wmin.z, p.z);
        wmax.x = (std::max)(wmax.x, p.x);
        wmax.y = (std::max)(wmax.y, p.y);
        wmax.z = (std::max)(wmax.z, p.z);
    }

    outBounds.min = wmin;
    outBounds.max = wmax;
    return true;
}

void FitCameraToMesh()
{
    if (g_mesh.vertices.empty())
        return;

    Scene::Aabb sponzaBounds{};
    if (!ComputeSponzaWorldBounds(sponzaBounds))
        return;

    const XMVECTOR mn = XMLoadFloat3(&sponzaBounds.min);
    const XMVECTOR mx = XMLoadFloat3(&sponzaBounds.max);
    const XMVECTOR center = XMVectorScale(XMVectorAdd(mn, mx), 0.5f);
    const XMVECTOR ext = XMVectorSubtract(mx, mn);
    float radius = XMVectorGetX(XMVector3Length(ext)) * 0.5f;
    radius = (std::max)(radius, 0.05f);

    constexpr float kEyeHeight = 1.65f;
    const XMVECTOR eye = XMVectorSet(
        XMVectorGetX(center),
        XMVectorGetY(mn) + kEyeHeight,
        XMVectorGetZ(center),
        0.0f);
    XMStoreFloat3(&g_camPos, eye);

    g_camYaw = 0.0f;
    g_camPitch = -0.05f;

    g_tessFarDist = (std::max)(radius * 2.5f, 4.0f);
}

static bool MaterialPathSuggestUvAnim(const std::wstring& rel)
{
    std::wstring s = rel;
    for (wchar_t& c : s)
        c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));

    static const wchar_t* keys[] = {
        L"fabric", L"curtain", L"banner", L"water", L"flag", L"vines", L"leaf", L"drape", nullptr};
    for (int k = 0; keys[k] != nullptr; ++k)
    {
        if (s.find(keys[k]) != std::wstring::npos)
            return true;
    }
    return false;
}

GpuMesh UploadMesh(const Scene::ProceduralMesh& mesh)
{
    GpuMesh gpu{};
    const UINT vbSize = static_cast<UINT>(mesh.vertices.size() * sizeof(Obj::MeshVertex));
    const UINT ibSize = static_cast<UINT>(mesh.indices.size() * sizeof(uint32_t));
    gpu.vb = CreateUploadBuffer(mesh.vertices.data(), vbSize);
    gpu.ib = CreateUploadBuffer(mesh.indices.data(), ibSize);
    gpu.vbv.BufferLocation = gpu.vb->GetGPUVirtualAddress();
    gpu.vbv.SizeInBytes = vbSize;
    gpu.vbv.StrideInBytes = sizeof(Obj::MeshVertex);
    gpu.ibv.BufferLocation = gpu.ib->GetGPUVirtualAddress();
    gpu.ibv.SizeInBytes = ibSize;
    gpu.ibv.Format = DXGI_FORMAT_R32_UINT;
    gpu.indexCount = static_cast<UINT>(mesh.indices.size());
    return gpu;
}

void BuildCubeField()
{
    g_sceneObjects.clear();
    g_visibleIndices.clear();
    g_gpuCube = {};

    Scene::ProceduralMesh cubeMesh{};
    Scene::BuildUnitCube(cubeMesh);
    g_gpuCube = UploadMesh(cubeMesh);

    Scene::Aabb spawnRegion{};
    if (Scene::Aabb sponzaBounds{}; ComputeSponzaWorldBounds(sponzaBounds))
    {
        const float spanY = sponzaBounds.max.y - sponzaBounds.min.y;
        const float centerX = (sponzaBounds.min.x + sponzaBounds.max.x) * 0.5f;
        const float centerZ = (sponzaBounds.min.z + sponzaBounds.max.z) * 0.5f;
        constexpr float kHorizExtent = 55.0f;
        const float floorY = sponzaBounds.min.y + 0.45f;
        const float interiorTopY = sponzaBounds.min.y + spanY * 0.72f;

        spawnRegion.min = XMFLOAT3{
            centerX - kHorizExtent,
            floorY,
            centerZ - kHorizExtent};
        spawnRegion.max = XMFLOAT3{
            centerX + kHorizExtent,
            (std::max)(interiorTopY, 26.0f),
            centerZ + kHorizExtent};
    }
    else
    {
        spawnRegion.min = XMFLOAT3{-55.0f, 3.0f, -55.0f};
        spawnRegion.max = XMFLOAT3{55.0f, 28.0f, 55.0f};
    }
    Scene::ScatterCubesAndSpheres(
        g_sceneObjects, kSceneCubeCount, 0, spawnRegion, 20260323u);

    g_sceneBounds = spawnRegion;
    for (const Scene::SceneObject& obj : g_sceneObjects)
        g_sceneBounds.Merge(obj.worldBounds);
    g_sceneBounds.min.x -= 2.0f;
    g_sceneBounds.min.y -= 2.0f;
    g_sceneBounds.min.z -= 2.0f;
    g_sceneBounds.max.x += 2.0f;
    g_sceneBounds.max.y += 2.0f;
    g_sceneBounds.max.z += 2.0f;

    const UINT octreeCount = (std::min)(kOctreeCubeCount, static_cast<UINT>(g_sceneObjects.size()));
    g_octree.Build(g_sceneObjects, g_sceneBounds, octreeCount);
}

bool LoadScene()
{
    g_sceneReady = false;
    g_hasSponza = false;
    g_sponzaMatCount = 0;
    g_cubeMatBase = 0;
    g_cubeSrvBase = 0;
    g_sceneIsCliffRock = false;
    g_mesh = {};
    g_matSrvPairBase.clear();
    g_gpuTextures.clear();
    g_whiteTexture.Reset();
    g_grayDispTexture.Reset();
    g_meshVB.Reset();
    g_meshIB.Reset();
    g_matCBUpload.Reset();
    g_matCBMapped = nullptr;
    g_matCount = 0;
    g_sceneObjects.clear();
    g_visibleIndices.clear();
    g_gpuCube = {};

    std::vector<uint8_t> matHasNormalTex;
    std::vector<uint8_t> matHasDispTex;
    std::vector<uint8_t> matDispInvert;
    uint32_t nextSlot = 0;

    const std::filesystem::path objPath = FindSponzaObj();
    if (objPath.empty())
    {
        MessageBoxW(
            g_hwnd,
            L"sponza.obj не найден — будет только поле из кубов.\n\n"
            L"Для Sponza положите sponza.obj и textures\\ рядом с exe.",
            L"Сцена",
            MB_OK | MB_ICONWARNING);
    }
    else
    {
        g_sceneIsCliffRock = PathLooksLikeCliffRock(objPath);
        std::wstring err;
        if (Obj::LoadObj(objPath, g_mesh, err) && !g_mesh.vertices.empty() && !g_mesh.indices.empty() &&
            !g_mesh.submeshes.empty())
        {
            g_hasSponza = true;

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

            g_sponzaMatCount = static_cast<UINT>(g_mesh.materials.size());
            g_matSrvPairBase.assign(g_mesh.materials.size(), 0);
            matHasNormalTex.assign(g_mesh.materials.size(), 0);
            matHasDispTex.assign(g_mesh.materials.size(), 0);
            matDispInvert.assign(g_mesh.materials.size(), 0);

            ThrowIfFailed(g_cmdAlloc[0]->Reset());
            ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), nullptr));

            std::vector<ComPtr<ID3D12Resource>> uploadKeep;
            const std::filesystem::path mtlDir = objPath.parent_path();

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

            ComPtr<ID3D12Resource> grayDispTex;
            if (!Tex::CreateSolidTexture2D(
                    g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), nextSlot + 2, g_srvDescriptorSize, 0x808080FFu,
                    grayDispTex, uploadKeep))
            {
                MessageBoxW(g_hwnd, L"Не удалось создать displacement по умолчанию.", L"Текстуры", MB_OK | MB_ICONERROR);
                return false;
            }
            g_grayDispTexture = grayDispTex;
            g_gpuTextures.push_back(grayDispTex);
            nextSlot += 3;

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
                if (nextSlot + 3u > kSrvHeapCount)
                {
                    MessageBoxW(g_hwnd, L"Переполнение кучи SRV.", L"Текстуры", MB_OK | MB_ICONWARNING);
                    break;
                }
                const uint32_t pairBase = nextSlot;
                nextSlot += 3;
                g_matSrvPairBase[i] = pairBase;
                const Obj::Material& m = g_mesh.materials[i];
                if (m.diffuseMapRel.empty())
                    Tex::WriteTexture2DSrv(
                        g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), pairBase, g_srvDescriptorSize);
                else
                    bindTextureSlot(pairBase, Tex::ResolveTexturePathInTexturesFolder(mtlDir, m.diffuseMapRel));
                const std::filesystem::path normalPath = ResolveNormalPath(mtlDir, m);
                if (normalPath.empty())
                    Tex::WriteTexture2DSrv(
                        g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), pairBase + 1, g_srvDescriptorSize);
                else
                    matHasNormalTex[i] = bindTextureSlot(pairBase + 1, normalPath) ? 1 : 0;
                bool dispRough = false;
                const std::filesystem::path dispPath = ResolveDisplacementPath(mtlDir, m, dispRough);
                if (dispPath.empty())
                    Tex::WriteTexture2DSrv(
                        g_device.Get(), g_grayDispTexture.Get(), g_srvHeap.Get(), pairBase + 2, g_srvDescriptorSize);
                else
                {
                    matHasDispTex[i] = bindTextureSlot(pairBase + 2, dispPath) ? 1 : 0;
                    matDispInvert[i] = dispRough ? 1 : 0;
                }
            }
            ExecuteCommandList();
        }
        else if (!err.empty())
        {
            MessageBoxW(g_hwnd, err.c_str(), L"OBJ Sponza", MB_OK | MB_ICONWARNING);
        }
    }

    BuildCubeField();

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), nullptr));
    {
        std::vector<ComPtr<ID3D12Resource>> cubeUploadKeep;
        if (!g_hasSponza)
        {
            g_cubeSrvBase = nextSlot;
            ComPtr<ID3D12Resource> whiteTex;
            if (!Tex::CreateSolidTexture2D(
                    g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), g_cubeSrvBase, g_srvDescriptorSize, 0xFFFFFFFFu,
                    whiteTex, cubeUploadKeep))
            {
                MessageBoxW(g_hwnd, L"Не удалось создать текстуру кубов.", L"Текстуры", MB_OK | MB_ICONERROR);
                return false;
            }
            g_whiteTexture = whiteTex;
            Tex::WriteTexture2DSrv(
                g_device.Get(), whiteTex.Get(), g_srvHeap.Get(), g_cubeSrvBase + 1, g_srvDescriptorSize);
            ComPtr<ID3D12Resource> grayDispTex;
            if (!Tex::CreateSolidTexture2D(
                    g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), g_cubeSrvBase + 2, g_srvDescriptorSize,
                    0x808080FFu, grayDispTex, cubeUploadKeep))
            {
                MessageBoxW(g_hwnd, L"Не удалось создать displacement кубов.", L"Текстуры", MB_OK | MB_ICONERROR);
                return false;
            }
            g_grayDispTexture = grayDispTex;
        }
        else
        {
            g_cubeSrvBase = nextSlot;
            if (g_cubeSrvBase + 3u > kSrvHeapCount)
            {
                MessageBoxW(g_hwnd, L"Недостаточно SRV для кубов.", L"Текстуры", MB_OK | MB_ICONWARNING);
                return false;
            }
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), g_cubeSrvBase, g_srvDescriptorSize);
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_whiteTexture.Get(), g_srvHeap.Get(), g_cubeSrvBase + 1, g_srvDescriptorSize);
            Tex::WriteTexture2DSrv(
                g_device.Get(), g_grayDispTexture.Get(), g_srvHeap.Get(), g_cubeSrvBase + 2, g_srvDescriptorSize);
        }
        ExecuteCommandList();
    }

    if (nextSlot >= kSrvHeapCount)
    {
        MessageBoxW(g_hwnd, L"Недостаточно SRV для octree viz.", L"Текстуры", MB_OK | MB_ICONWARNING);
        return false;
    }
    BindOctreeVizSrv(nextSlot);
    ++nextSlot;

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), g_pipelineGeo.Get()));
    ThrowIfFailed(g_cmdList->Close());

    g_cubeMatBase = g_hasSponza ? g_sponzaMatCount : 0;
    g_matCount = g_cubeMatBase + kSceneCubeCount;
    const UINT64 matBufSize = static_cast<UINT64>(g_matCount) * kCbAlign;
    g_matCBUpload = CreateUploadBuffer(nullptr, matBufSize);
    D3D12_RANGE mr{0, 0};
    ThrowIfFailed(g_matCBUpload->Map(0, &mr, reinterpret_cast<void**>(&g_matCBMapped)));

    for (UINT i = 0; i < g_matCount; ++i)
    {
        MatCBGPU* slot = reinterpret_cast<MatCBGPU*>(g_matCBMapped + static_cast<size_t>(i) * kCbAlign);
        std::memset(slot, 0, sizeof(MatCBGPU));
        if (g_hasSponza && i < g_sponzaMatCount)
        {
            const Obj::Material& mm = g_mesh.materials[i];
            slot->Kd = XMFLOAT4(mm.Kd[0], mm.Kd[1], mm.Kd[2], 1.0f);
            slot->UvScale = XMFLOAT2(mm.uvScale[0], mm.uvScale[1]);
            slot->UvOffset = XMFLOAT2(mm.uvOffset[0], mm.uvOffset[1]);
            const float dispScale = g_sceneIsCliffRock ? 0.04f : 0.02f;
            slot->MatFlags = XMFLOAT4(
                dispScale,
                1.0f,
                MaterialPathSuggestUvAnim(mm.diffuseMapRel) ? 1.0f : 0.0f,
                matHasNormalTex[i] ? 1.0f : 0.0f);
            slot->MatFlags2 = XMFLOAT4(
                matHasDispTex[i] ? 1.0f : 0.0f,
                matDispInvert[i] ? 1.0f : 0.0f,
                0,
                0);
            slot->Ns = mm.Ns;
            slot->SpecIntensity = 0.35f;
        }
        else if (i >= g_cubeMatBase)
        {
            const Scene::SceneObject& obj = g_sceneObjects[i - g_cubeMatBase];
            slot->Kd = obj.color;
            slot->UvScale = XMFLOAT2(1.0f, 1.0f);
            slot->UvOffset = XMFLOAT2(0.0f, 0.0f);
            slot->MatFlags = XMFLOAT4(0.0f, 1.0f, 0.0f, 0.0f);
            slot->MatFlags2 = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
            slot->Ns = 48.0f;
            slot->SpecIntensity = 0.45f;
        }
    }

    if (g_hasSponza)
        FitCameraToMesh();
    else
    {
        const XMVECTOR mn = XMLoadFloat3(&g_sceneBounds.min);
        const XMVECTOR mx = XMLoadFloat3(&g_sceneBounds.max);
        const XMVECTOR ext = XMVectorSubtract(mx, mn);
        const XMVECTOR mid = XMVectorAdd(mn, XMVectorScale(ext, 0.5f));
        float radius = XMVectorGetX(XMVector3Length(ext)) * 0.5f;
        radius = (std::max)(radius, 5.0f);
        const XMVECTOR eye = XMVectorAdd(mid, XMVectorSet(0.0f, radius * 0.35f, radius * 1.35f, 0.0f));
        XMStoreFloat3(&g_camPos, eye);
        const XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(mid, eye));
        g_camPitch = asinf(XMVectorGetY(dir));
        g_camYaw = atan2f(XMVectorGetX(dir), XMVectorGetZ(dir));
        g_camPitch = std::clamp(g_camPitch, -XM_PIDIV2 + 0.02f, XM_PIDIV2 - 0.02f);
        g_tessFarDist = (std::max)(radius * 1.5f, 20.0f);
    }

    g_prevViewProj = XMFLOAT4X4{};
    g_sceneReady = true;
    return true;
}

void WriteFrameCB(const XMMATRIX& world, const XMMATRIX& viewProj, float timeSec, uint32_t slotIndex, bool forSponza)
{
    FrameCB data{};
    XMStoreFloat4x4(&data.World, world);
    XMStoreFloat4x4(&data.ViewProj, viewProj);
    data.PrevViewProj = g_prevViewProj;
    data.TimeCamPos = XMFLOAT4(timeSec, g_camPos.x, g_camPos.y, g_camPos.z);
    if (forSponza)
    {
        data.UvAnimAndPad = XMFLOAT4(0.035f, 0.022f, 0.0f, 0.0f);
        const float tessMax = g_sceneIsCliffRock ? 3.0f : 4.0f;
        const float tessNear = g_sceneIsCliffRock ? 0.35f : 0.5f;
        data.TessParams = XMFLOAT4(tessMax, tessNear, g_tessFarDist, 0.0f);
    }
    else
    {
        data.UvAnimAndPad = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        data.TessParams = XMFLOAT4(1.0f, 1.0f, g_tessFarDist, 0.0f);
    }
    data.DebugView = XMFLOAT4(g_debugFrameView ? 1.0f : 0.0f, 0, 0, 0);
    FrameCB* dst = reinterpret_cast<FrameCB*>(g_frameCBMapped + static_cast<size_t>(slotIndex) * kCbAlign);
    std::memcpy(dst, &data, sizeof(FrameCB));
}

XMVECTOR CameraForwardVector()
{
    return XMVector3Normalize(XMVectorSet(
        sinf(g_camYaw) * cosf(g_camPitch), sinf(g_camPitch), cosf(g_camYaw) * cosf(g_camPitch), 0.0f));
}

XMMATRIX CalcView()
{
    const XMVECTOR eye = XMLoadFloat3(&g_camPos);
    const XMVECTOR dir = CameraForwardVector();
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    return XMMatrixLookToLH(eye, dir, up);
}

XMMATRIX CalcProjection()
{
    const float aspect = static_cast<float>(g_width) / static_cast<float>((std::max)(1u, g_height));
    return XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 500.0f);
}

XMMATRIX CalcViewProj()
{
    return CalcView() * CalcProjection();
}

void UpdateCamera(float dt)
{
    if (!g_hwnd || dt <= 0.0f)
        return;

    constexpr float moveSpeed = 14.0f;
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

    const XMVECTOR forward = CameraForwardVector();
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

void UpdateWindowTitle()
{
    if (!g_hwnd)
        return;

    const wchar_t* cullMode = L"выкл";
    if (g_frustumCulling || g_distanceCulling)
    {
        if (g_frustumCulling && g_octreeCulling)
            cullMode = L"frustum+octree";
        else if (g_frustumCulling)
            cullMode = L"frustum";
        else
            cullMode = L"dist";
    }

    wchar_t title[360];
    swprintf_s(
        title,
        L"SecondSem CG — Sponza%s | %u кубов (octree %u) | видно %u | octree viz %u | cull: %s | F6 %s",
        g_hasSponza ? L"" : L" (нет)",
        kSceneCubeCount,
        kOctreeCubeCount,
        g_lastVisibleCount,
        g_octreeVizNodeCount,
        cullMode,
        g_octreeViz ? L"ON" : L"OFF");
    SetWindowTextW(g_hwnd, title);
}

void DrawScene(const XMMATRIX& view, const XMMATRIX& proj)
{
    if (!g_sceneReady)
        return;

    const XMMATRIX viewProj = view * proj;

    Scene::Frustum frustum{};
    frustum.FromViewAndProjection(view, proj);
    Scene::CollectVisibleObjects(
        g_sceneObjects,
        frustum,
        g_frustumCulling,
        g_octreeCulling,
        g_octree,
        g_camPos,
        g_maxDrawDistance,
        g_distanceCulling,
        g_visibleIndices,
        g_octreeViz ? &g_octreeVisitedNodes : nullptr);
    g_lastVisibleCount = static_cast<UINT>(g_visibleIndices.size());
    if (g_octreeViz)
    {
        g_octreeVizNodeCount = static_cast<UINT>(
            (std::min)(g_octreeVisitedNodes.size(), static_cast<size_t>(kOctreeVizMaxNodes)));
    }
    else
    {
        g_octreeVisitedNodes.clear();
        g_octreeVizNodeCount = 0;
    }

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_cmdList->SetDescriptorHeaps(1, heaps);
    g_cmdList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_cmdList->SetPipelineState(
        (g_debugFrameView ? g_pipelineGeoWire : g_pipelineGeo).Get());
    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);

    const D3D12_GPU_DESCRIPTOR_HANDLE srvHeapStart = g_srvHeap->GetGPUDescriptorHandleForHeapStart();

    if (g_hasSponza)
    {
        const XMMATRIX sponzaWorld = g_sceneIsCliffRock
            ? XMMatrixIdentity()
            : XMMatrixScaling(0.01f, 0.01f, 0.01f) * XMMatrixRotationX(XM_PI);
        WriteFrameCB(sponzaWorld, viewProj, g_appTime, kSponzaFrameSlot, true);
        g_cmdList->SetGraphicsRootConstantBufferView(
            0, g_frameCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(kSponzaFrameSlot) * kCbAlign);
        g_cmdList->IASetVertexBuffers(0, 1, &g_meshVbv);
        g_cmdList->IASetIndexBuffer(&g_meshIbv);

        for (const Obj::Submesh& sm : g_mesh.submeshes)
        {
            if (sm.materialIndex >= g_matSrvPairBase.size())
                continue;
            g_cmdList->SetGraphicsRootConstantBufferView(
                1, g_matCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(sm.materialIndex) * kCbAlign);
            D3D12_GPU_DESCRIPTOR_HANDLE table = srvHeapStart;
            table.ptr += static_cast<SIZE_T>(g_matSrvPairBase[sm.materialIndex]) * g_srvDescriptorSize;
            g_cmdList->SetGraphicsRootDescriptorTable(2, table);
            g_cmdList->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE cubeTable = srvHeapStart;
    cubeTable.ptr += static_cast<SIZE_T>(g_cubeSrvBase) * g_srvDescriptorSize;

    for (uint32_t objIndex : g_visibleIndices)
    {
        const Scene::SceneObject& obj = g_sceneObjects[objIndex];
        const XMMATRIX world = XMMatrixScaling(obj.uniformScale, obj.uniformScale, obj.uniformScale) *
            XMMatrixTranslation(obj.position.x, obj.position.y, obj.position.z);
        const UINT frameSlot = kCubeFrameBase + objIndex;
        const UINT matSlot = g_cubeMatBase + objIndex;

        WriteFrameCB(world, viewProj, g_appTime, frameSlot, false);
        g_cmdList->SetGraphicsRootConstantBufferView(
            0, g_frameCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(frameSlot) * kCbAlign);
        g_cmdList->SetGraphicsRootConstantBufferView(
            1, g_matCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(matSlot) * kCbAlign);
        g_cmdList->SetGraphicsRootDescriptorTable(2, cubeTable);
        g_cmdList->IASetVertexBuffers(0, 1, &g_gpuCube.vbv);
        g_cmdList->IASetIndexBuffer(&g_gpuCube.ibv);
        g_cmdList->DrawIndexedInstanced(g_gpuCube.indexCount, 1, 0, 0, 0);
    }
}

void DrawOctreeVizOverlay(const XMMATRIX& viewProj, D3D12_CPU_DESCRIPTOR_HANDLE backbufferRtv)
{
    if (!g_octreeViz || g_octreeVizNodeCount == 0 || !g_octreeVizPso)
        return;

    auto* nodesGpu = reinterpret_cast<Scene::OctreeNodeGpu*>(g_octreeVizNodeMapped);
    for (UINT i = 0; i < g_octreeVizNodeCount; ++i)
    {
        const Scene::Aabb& b = g_octreeVisitedNodes[i];
        nodesGpu[i].minW = XMFLOAT4(b.min.x, b.min.y, b.min.z, 0.0f);
        nodesGpu[i].maxW = XMFLOAT4(b.max.x, b.max.y, b.max.z, 0.0f);
    }

    OctreeVizCB cb{};
    XMStoreFloat4x4(&cb.ViewProj, viewProj);
    cb.LineColor = XMFLOAT4(0.1f, 1.0f, 0.2f, 1.0f);
    cb.NodeCount = g_octreeVizNodeCount;
    std::memcpy(g_octreeVizCbMapped, &cb, sizeof(OctreeVizCB));

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_cmdList->SetDescriptorHeaps(1, heaps);
    g_cmdList->SetGraphicsRootSignature(g_octreeVizRootSig.Get());
    g_cmdList->SetPipelineState(g_octreeVizPso.Get());
    g_cmdList->OMSetRenderTargets(1, &backbufferRtv, FALSE, nullptr);
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);
    g_cmdList->SetGraphicsRootConstantBufferView(0, g_octreeVizCbUpload->GetGPUVirtualAddress());

    D3D12_GPU_DESCRIPTOR_HANDLE srvTable = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    srvTable.ptr += static_cast<SIZE_T>(g_octreeVizSrvSlot) * g_srvDescriptorSize;
    g_cmdList->SetGraphicsRootDescriptorTable(1, srvTable);

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    g_cmdList->DrawInstanced(24, g_octreeVizNodeCount, 0, 0);
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
    Scene::UpdateCubeSinMotion(g_sceneObjects, g_camPos, g_appTime, g_animFrameIndex++);
    const XMMATRIX view = CalcView();
    const XMMATRIX proj = CalcProjection();
    const XMMATRIX viewProj = view * proj;

    ThrowIfFailed(g_cmdAlloc[g_frameIndex]->Reset());
    ThrowIfFailed(g_cmdList->Reset(
        g_cmdAlloc[g_frameIndex].Get(),
        (g_debugFrameView ? g_pipelineGeoWire : g_pipelineGeo).Get()));

    GBuffer& gb = g_renderSys.GBufferTargets();
    gb.TransitionGeometryToRenderTargets(g_cmdList.Get());

    static const float kGbClearNormal[] = {0.06f, 0.07f, 0.10f};
    static const float kGbClearDebug[] = {0.02f, 0.02f, 0.03f};
    const float* const gbClearRgb = g_debugFrameView ? kGbClearDebug : kGbClearNormal;
    gb.ClearAndSetGeometryRenderTargets(g_cmdList.Get(), gbClearRgb);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);

    DrawScene(view, proj);

    gb.TransitionGeometryToShaderResource(g_cmdList.Get());

    ComPtr<ID3D12Resource> backBuffer = g_renderTargets[g_frameIndex];
    const D3D12_RESOURCE_STATES rtBefore =
        g_swapSeenPresent[g_frameIndex] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER toRt = MakeTransition(backBuffer.Get(), rtBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    XMFLOAT3 camForward{};
    XMStoreFloat3(&camForward, CameraForwardVector());
    XMVECTOR det{};
    const XMMATRIX invViewProj = XMMatrixInverse(&det, viewProj);
    XMFLOAT4X4 invViewProjStore{};
    XMStoreFloat4x4(&invViewProjStore, invViewProj);
    g_renderSys.UploadFrameConstants(g_camPos, camForward, invViewProjStore, g_width, g_height);
    g_renderSys.DrawLightingPass(g_cmdList.Get(), g_srvHeap.Get(), rtv, g_width, g_height);

    DrawOctreeVizOverlay(viewProj, rtv);

    XMFLOAT4X4 currentViewProj{};
    XMStoreFloat4x4(&currentViewProj, viewProj);
    g_prevViewProj = currentViewProj;
    UpdateWindowTitle();

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
    CreateOctreeVizPipeline();
    g_renderSys.Init(
        g_device.Get(),
        g_width,
        g_height,
        g_srvHeap.Get(),
        kDeferredSrvBase,
        g_srvDescriptorSize,
        DeferredShaderPath().c_str());
    LoadScene();
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
        else if (wp == VK_F2)
            g_debugFrameView = !g_debugFrameView;
        else if (wp == VK_F3)
            g_frustumCulling = !g_frustumCulling;
        else if (wp == VK_F4)
            g_octreeCulling = !g_octreeCulling;
        else if (wp == VK_F5)
            g_distanceCulling = !g_distanceCulling;
        else if (wp == VK_F6)
            g_octreeViz = !g_octreeViz;
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
    wc.lpszClassName = L"SecondSemCG_Instancing";
    RegisterClassExW(&wc);

    RECT windowRect{0, 0, static_cast<LONG>(kClientW), static_cast<LONG>(kClientH)};
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"SecondSem CG — Sponza + 10000 кубов", WS_OVERLAPPEDWINDOW,
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
