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
#include "TextureUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
constexpr UINT kCbAlign = 256;

struct alignas(256) FrameCB
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 ViewProj;
    float _Pad[32];
};

static_assert(sizeof(FrameCB) == 256);

struct MatCB
{
    XMFLOAT4 Kd;
};

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
ComPtr<ID3D12PipelineState> g_pipeline;

ComPtr<ID3D12DescriptorHeap> g_srvHeap;
UINT g_srvDescriptorSize = 0;

bool g_sceneReady = false;
Obj::LoadedMesh g_mesh{};
ComPtr<ID3D12Resource> g_meshVB;
ComPtr<ID3D12Resource> g_meshIB;
D3D12_VERTEX_BUFFER_VIEW g_meshVbv{};
D3D12_INDEX_BUFFER_VIEW g_meshIbv{};

std::vector<uint32_t> g_matToSrv;
std::vector<ComPtr<ID3D12Resource>> g_gpuTextures;
ComPtr<ID3D12Resource> g_matCBUpload;
UINT8* g_matCBMapped = nullptr;
UINT g_matCount = 0;

ComPtr<ID3D12Resource> g_frameCBUpload;
UINT8* g_frameCBMapped = nullptr;

UINT g_frameIndex = 0;

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

std::wstring ShaderPath()
{
    return ExeDirectory() + L"Shaders.hlsl";
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

void CreatePipeline()
{
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
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

    const std::wstring sp = ShaderPath();
    ComPtr<ID3DBlob> vs, ps;
    CompileShader(sp.c_str(), "VSMain", "vs_5_0", vs);
    CompileShader(sp.c_str(), "PSMain", "ps_5_0", ps);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_rootSignature.Get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.InputLayout = {layout, _countof(layout)};
    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_pipeline)));
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

bool LoadScene()
{
    g_sceneReady = false;
    g_mesh = {};
    g_matToSrv.clear();
    g_gpuTextures.clear();
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
    std::unordered_map<std::wstring, uint32_t> pathToSrv;
    g_matToSrv.assign(g_mesh.materials.size(), 0);

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), nullptr));

    std::vector<ComPtr<ID3D12Resource>> uploadKeep;
    uint32_t nextSrv = 0;

    ComPtr<ID3D12Resource> whiteTex;
    if (!Tex::CreateSolidTexture2D(
            g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), nextSrv, g_srvDescriptorSize, 0xFFFFFFFFu,
            whiteTex, uploadKeep))
    {
        MessageBoxW(g_hwnd, L"Не удалось создать текстуру по умолчанию.", L"Текстуры", MB_OK | MB_ICONERROR);
        return false;
    }
    g_gpuTextures.push_back(whiteTex);
    const uint32_t whiteSrv = nextSrv++;

    for (size_t i = 0; i < g_mesh.materials.size(); ++i)
    {
        const Obj::Material& m = g_mesh.materials[i];
        if (m.diffuseMapRel.empty())
        {
            g_matToSrv[i] = whiteSrv;
            continue;
        }

        const std::filesystem::path texPath = mtlDir / m.diffuseMapRel;
        if (!std::filesystem::exists(texPath))
        {
            g_matToSrv[i] = whiteSrv;
            continue;
        }

        const std::wstring key = texPath.lexically_normal().wstring();
        const auto found = pathToSrv.find(key);
        if (found != pathToSrv.end())
        {
            g_matToSrv[i] = found->second;
            continue;
        }

        if (nextSrv >= kSrvHeapCount)
        {
            g_matToSrv[i] = whiteSrv;
            continue;
        }

        ComPtr<ID3D12Resource> tex;
        std::wstring terr;
        if (!Tex::CreateTexture2DFromFile(
                g_device.Get(), g_cmdList.Get(), g_srvHeap.Get(), nextSrv, g_srvDescriptorSize, texPath, tex,
                uploadKeep, terr))
        {
            g_matToSrv[i] = whiteSrv;
            continue;
        }

        g_gpuTextures.push_back(tex);
        pathToSrv[key] = nextSrv;
        g_matToSrv[i] = nextSrv;
        ++nextSrv;
    }

    ExecuteCommandList();
    uploadKeep.clear();

    ThrowIfFailed(g_cmdAlloc[0]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[0].Get(), g_pipeline.Get()));
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
        MatCB* slot = reinterpret_cast<MatCB*>(g_matCBMapped + static_cast<size_t>(i) * kCbAlign);
        if (i < g_mesh.materials.size())
        {
            const float* kd = g_mesh.materials[i].Kd;
            slot->Kd = XMFLOAT4(kd[0], kd[1], kd[2], 1.0f);
        }
        else
            slot->Kd = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    g_sceneReady = true;
    return true;
}

void WriteFrameCB(const XMMATRIX& world, const XMMATRIX& viewProj)
{
    FrameCB data{};
    XMStoreFloat4x4(&data.World, world);
    XMStoreFloat4x4(&data.ViewProj, viewProj);
    std::memcpy(g_frameCBMapped, &data, sizeof(FrameCB));
}

XMMATRIX CalcViewProj()
{
    const XMVECTOR eye = XMLoadFloat3(&g_camPos);
    const XMVECTOR dir = XMVector3Normalize(XMVectorSet(
        sinf(g_camYaw) * cosf(g_camPitch), sinf(g_camPitch), cosf(g_camYaw) * cosf(g_camPitch), 0.0f));
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX view = XMMatrixLookToLH(eye, dir, up);
    const float aspect = static_cast<float>(g_width) / static_cast<float>((std::max)(1u, g_height));
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, aspect, 0.1f, 500.0f);
    return view * proj;
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
    g_cmdList->SetPipelineState(g_pipeline.Get());

    const XMMATRIX world = XMMatrixScaling(0.01f, 0.01f, 0.01f);
    WriteFrameCB(world, viewProj);
    g_cmdList->SetGraphicsRootConstantBufferView(0, g_frameCBUpload->GetGPUVirtualAddress());

    g_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_cmdList->IASetVertexBuffers(0, 1, &g_meshVbv);
    g_cmdList->IASetIndexBuffer(&g_meshIbv);

    const D3D12_GPU_DESCRIPTOR_HANDLE srvBase = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    for (const Obj::Submesh& sm : g_mesh.submeshes)
    {
        if (sm.materialIndex >= g_matToSrv.size())
            continue;

        const UINT srvIndex = g_matToSrv[sm.materialIndex];
        g_cmdList->SetGraphicsRootConstantBufferView(
            1, g_matCBUpload->GetGPUVirtualAddress() + static_cast<UINT64>(sm.materialIndex) * kCbAlign);

        D3D12_GPU_DESCRIPTOR_HANDLE srv = srvBase;
        srv.ptr += static_cast<SIZE_T>(srvIndex) * g_srvDescriptorSize;
        g_cmdList->SetGraphicsRootDescriptorTable(2, srv);
        g_cmdList->DrawIndexedInstanced(sm.indexCount, 1, sm.indexStart, 0, 0);
    }
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
    const XMMATRIX viewProj = CalcViewProj();

    ThrowIfFailed(g_cmdAlloc[g_frameIndex]->Reset());
    ThrowIfFailed(g_cmdList->Reset(g_cmdAlloc[g_frameIndex].Get(), g_pipeline.Get()));

    ComPtr<ID3D12Resource> backBuffer = g_renderTargets[g_frameIndex];
    const D3D12_RESOURCE_STATES rtBefore =
        g_swapSeenPresent[g_frameIndex] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_BARRIER toRt = MakeTransition(backBuffer.Get(), rtBefore, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_cmdList->ResourceBarrier(1, &toRt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();

    const float clearColor[] = {0.06f, 0.07f, 0.10f, 1.0f};
    g_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    g_cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g_cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(g_width);
    viewport.Height = static_cast<float>(g_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
    g_cmdList->RSSetViewports(1, &viewport);
    g_cmdList->RSSetScissorRects(1, &scissor);

    DrawScene(viewProj);

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
    CreatePipeline();
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
        0, wc.lpszClassName, L"SecondSem CG — Sponza (ПКМ, WASD, Space/Ctrl)", WS_OVERLAPPEDWINDOW,
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
