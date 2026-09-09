#include "App.h"

#include <d3dcompiler.h>
// DXC, the compiler that replaced FXC. Steps 1-15 only ever needed
// d3dcompiler.h; DXR forces the switch because it needs DXIL, which FXC
// cannot emit. Both compilers coexist here - only RayTracing.hlsl goes
// through this one.
#include <dxcapi.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    // Pulled back and up from step 14's (0, 2, -5) - that framing suited a
    // single cube at the origin, but it sits close enough to the CubeGridDim
    // x CubeGridDim grid's near edge (see InitSceneGeometry) that perspective
    // would make the nearest and farthest rows of cubes look wildly
    // different sizes. This distance keeps the whole grid's depth range
    // (CubeGridDim - 1) * kCubeGridSpacing small relative to the
    // camera-to-grid distance, so the size difference across the grid stays
    // subtle instead of extreme.
    const XMVECTOR kEyePosition = XMVectorSet(0.0f, 3.0f, -10.0f, 0.0f);
    const float kSkyboxScale = 50.0f;
    const XMVECTOR kLightDirection = []
    {
        return XMVector3Normalize(XMVectorSet(0.5f, -1.0f, 0.3f, 0.0f));
    }();

    // Export names have to match the function names in RayTracing.hlsl
    // character for character. A typo doesn't fail the build - it fails
    // much later, as a null shader identifier when the shader table is
    // assembled, which is why InitRaytracingShaderTable checks for that.
    const wchar_t* kRayGenShaderName = L"RayGenShader";
    const wchar_t* kClosestHitShaderName = L"ClosestHitShader";
    const wchar_t* kMissShaderName = L"MissShader";
    const wchar_t* kShadowMissShaderName = L"ShadowMissShader";
    // Hit group names, unlike the four above, are invented on the D3D side
    // rather than declared in HLSL: a hit group is a grouping of up to
    // three shader stages, and these two deliberately share one
    // ClosestHitShader while differing only in their shader table record.
    const wchar_t* kCubeHitGroupName = L"CubeHitGroup";
    const wchar_t* kPlaneHitGroupName = L"PlaneHitGroup";

    // FXC (D3DCompileFromFile, used by every other shader here) stops at
    // shader model 5.1 and emits DXBC. DXR needs DXIL from shader model
    // 6.3 or newer, which only DXC produces - so this one shader takes a
    // completely separate path through a completely separate compiler.
    ComPtr<IDxcBlob> CompileRaytracingLibrary(const wchar_t* fileName)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
            FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        {
            throw std::runtime_error(
                "Failed to load dxcompiler.dll - it and dxil.dll must sit next to the executable. "
                "The project copies both out of the Windows SDK after every build.");
        }

        ComPtr<IDxcBlobEncoding> source;
        ThrowIfFailed(utils->LoadFile(fileName, nullptr, &source));

        DxcBuffer sourceBuffer = {};
        sourceBuffer.Ptr = source->GetBufferPointer();
        sourceBuffer.Size = source->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_ACP;

        // No -E entry point, unlike every FXC call in this file: a library
        // target exports every function carrying a [shader("...")]
        // attribute, which is how one file holds the raygen shader, the
        // closest-hit shader, and both miss shaders at once.
        std::vector<const wchar_t*> arguments = { L"-T", L"lib_6_3" };
#if defined(_DEBUG)
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Od");
#endif

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(
            &sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
            nullptr, IID_PPV_ARGS(&result)));

        HRESULT compileStatus = S_OK;
        ThrowIfFailed(result->GetStatus(&compileStatus));
        if (FAILED(compileStatus))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            throw std::runtime_error(
                (errors != nullptr && errors->GetStringLength() > 0)
                    ? std::string("RayTracing.hlsl failed to compile:\n") + errors->GetStringPointer()
                    : "RayTracing.hlsl failed to compile.");
        }

        ComPtr<IDxcBlob> shader;
        ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr));
        return shader;
    }

    ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, const void* data, UINT size)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)));

        if (data != nullptr)
        {
            void* mappedData = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            ThrowIfFailed(buffer->Map(0, &readRange, &mappedData));
            memcpy(mappedData, data, size);
            buffer->Unmap(0, nullptr);
        }

        return buffer;
    }

    // Acceleration structures and the scratch space they're built through
    // have requirements no buffer in steps 1-15 had: a DEFAULT heap (the
    // driver writes them from the GPU, so an upload heap is useless) and
    // ALLOW_UNORDERED_ACCESS (the build is a UAV write).
    //
    // initialState is RAYTRACING_ACCELERATION_STRUCTURE for the structures
    // themselves - a state they then never leave - and COMMON for scratch.
    // Scratch does need to be UNORDERED_ACCESS by the time a build reads
    // it, but D3D12 creates every buffer in COMMON regardless of what's
    // asked for and warns if told otherwise; common-state promotion then
    // moves it to UNORDERED_ACCESS on first use for free.
    ComPtr<ID3D12Resource> CreateUavBuffer(
        ID3D12Device* device, UINT64 size, D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer)));
        return buffer;
    }

    ComPtr<ID3D12Resource> CreateConstantBuffer(ID3D12Device* device, UINT structSize, void** mappedPtr)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = (structSize + 255) & ~255; // CBV size must be 256-byte aligned
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)));

        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(buffer->Map(0, &readRange, mappedPtr));
        return buffer;
    }

    // Like CreateConstantBuffer, but one upload buffer holding `count`
    // back-to-back 256-byte-aligned elements instead of a single struct -
    // used for the per-cube constant data in App::m_cubeConstantBuffers and
    // App::m_shadowCubeConstantBuffers, so the CubeCount cubes don't need
    // CubeCount separate committed resources. Element i's CPU pointer is
    // `*mappedPtr + i * elementStride`, its GPU address is
    // `buffer->GetGPUVirtualAddress() + i * elementStride`.
    ComPtr<ID3D12Resource> CreateConstantBufferArray(
        ID3D12Device* device, UINT elementStride, UINT count, void** mappedPtr)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = static_cast<UINT64>(elementStride) * count;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)));

        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(buffer->Map(0, &readRange, mappedPtr));
        return buffer;
    }

    constexpr UINT AlignTo256(UINT size)
    {
        return (size + 255) & ~255u;
    }
}

App::App(HWND hwnd, UINT width, UINT height)
    : m_hwnd(hwnd)
    , m_width(width)
    , m_height(height)
{
    m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_shadowViewport = { 0.0f, 0.0f, static_cast<float>(ShadowMapSize), static_cast<float>(ShadowMapSize), 0.0f, 1.0f };
    m_shadowScissorRect = { 0, 0, static_cast<LONG>(ShadowMapSize), static_cast<LONG>(ShadowMapSize) };

    QueryPerformanceFrequency(&m_perfFrequency);
    QueryPerformanceCounter(&m_startTime);

    InitDevice();
    InitRaytracingSupport();
    InitCommandQueue();
    InitSwapChain();
    InitRenderTargets();
    InitMsaaSupport();
    InitMsaaRenderTarget();
    InitDepthBuffer();
    InitComputePostProcess();
    InitCommandList();
    InitFence();
    InitShadowMap();
    InitRootSignature();
    InitPipelineState();
    InitSceneGeometry();
    InitRaytracingAccelerationStructures();
    InitConstantBuffer();
    InitTextures();
    InitRaytracingOutput();
    InitRaytracingRootSignatures();
    InitRaytracingPipeline();
    InitRaytracingShaderTable();
    InitSkybox();

    // The title bar doubles as the UI for the F key toggle, so it has to
    // start out agreeing with m_shadowMode's initial value.
    UpdateWindowTitle();
}

void App::UpdateWindowTitle()
{
    SetWindowTextW(m_hwnd, m_shadowMode == 0
        ? L"D3D12 Tutorial - 16. DXRRayTracing  |  Shadow Map  [F] to switch"
        : L"D3D12 Tutorial - 16. DXRRayTracing  |  Raytraced Shadows  [F] to switch");
}

void App::OnKeyDown(WPARAM key)
{
    if (key != 'F')
    {
        return;
    }

    m_shadowMode = (m_shadowMode == 0) ? 1u : 0u;
    UpdateWindowTitle();
}

App::~App()
{
    WaitForPreviousFrame();
    if (m_cubeConstantBuffers) m_cubeConstantBuffers->Unmap(0, nullptr);
    if (m_planeConstantBuffer) m_planeConstantBuffer->Unmap(0, nullptr);
    if (m_skyboxConstantBuffer) m_skyboxConstantBuffer->Unmap(0, nullptr);
    if (m_shadowCubeConstantBuffers) m_shadowCubeConstantBuffers->Unmap(0, nullptr);
    if (m_shadowPlaneConstantBuffer) m_shadowPlaneConstantBuffer->Unmap(0, nullptr);
    if (m_tlasInstanceDescs) m_tlasInstanceDescs->Unmap(0, nullptr);
    if (m_raytracingConstantBuffer) m_raytracingConstantBuffer->Unmap(0, nullptr);
    CloseHandle(m_fenceEvent);
}

void App::InitDevice()
{
    UINT factoryFlags = 0;

#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));
    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));
}

void App::InitRaytracingSupport()
{
    // ID3D12Device5 and ID3D12GraphicsCommandList4 both arrived in Windows
    // 10 1809 alongside DXR itself; an older runtime hands back the same
    // ID3D12Device as always and fails this cast.
    if (FAILED(m_device.As(&m_dxrDevice)))
    {
        throw std::runtime_error(
            "ID3D12Device5 is unavailable - DirectX Raytracing needs Windows 10 1809 or newer.");
    }

    // Having the interface is not the same as having the hardware. DXR is
    // an optional feature, and pre-RTX GPUs report TIER_NOT_SUPPORTED even
    // on a current OS - so ask before building anything that assumes it.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
        options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        throw std::runtime_error(
            "This step needs a GPU with DirectX Raytracing Tier 1.0 support "
            "(GeForce RTX 2000 series or newer, or an equivalent AMD/Intel part).");
    }
}

void App::InitCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
}

void App::InitSwapChain()
{
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(), m_hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1));
    ThrowIfFailed(swapChain1.As(&m_swapChain));

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void App::InitRenderTargets()
{
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i)
    {
        ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i])));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }
}

void App::InitMsaaSupport()
{
    // Ask the device how many quality levels it supports for 4x MSAA on the
    // main color target's format. Step 10/11 queried the back buffer's UNORM
    // format here; this step queries HdrColorFormat instead, since the main
    // pass no longer renders straight into a UNORM target - quality-level
    // support can differ between formats. Virtually all D3D12 hardware
    // supports 4x for a plain FLOAT16 color format, but if it somehow
    // doesn't, fall back to 1x (no multisampling) rather than fail to
    // create the resource below.
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels = {};
    qualityLevels.Format = HdrColorFormat;
    qualityLevels.SampleCount = 4;
    qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)))
        && qualityLevels.NumQualityLevels > 0)
    {
        m_msaaSampleCount = 4;
        m_msaaQualityLevel = qualityLevels.NumQualityLevels - 1;
    }
    else
    {
        // 4x MSAA unsupported on this device - render single-sample instead.
        m_msaaSampleCount = 1;
        m_msaaQualityLevel = 0;
    }
}

void App::InitMsaaRenderTarget()
{
    // A flip-model swapchain (DXGI_SWAP_EFFECT_FLIP_DISCARD) cannot itself
    // be a multisample render target, so the main color pass instead draws
    // into this off-screen multisampled texture; Render() resolves it down
    // into the actual back buffer afterwards.
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_msaaRtvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC colorDesc = {};
    colorDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    colorDesc.Width = m_width;
    colorDesc.Height = m_height;
    colorDesc.DepthOrArraySize = 1;
    colorDesc.MipLevels = 1;
    colorDesc.Format = HdrColorFormat;
    colorDesc.SampleDesc.Count = m_msaaSampleCount;
    colorDesc.SampleDesc.Quality = m_msaaQualityLevel;
    colorDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE colorClearValue = {};
    colorClearValue.Format = HdrColorFormat;
    colorClearValue.Color[0] = 0.0f;
    colorClearValue.Color[1] = 0.0f;
    colorClearValue.Color[2] = 0.0f;
    colorClearValue.Color[3] = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &colorDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClearValue, IID_PPV_ARGS(&m_msaaColorTarget)));

    m_device->CreateRenderTargetView(
        m_msaaColorTarget.Get(), nullptr, m_msaaRtvHeap->GetCPUDescriptorHandleForHeapStart());
}

void App::InitDepthBuffer()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // This depth buffer backs the same MSAA main pass as m_msaaColorTarget
    // above, so it needs the same sample count/quality - a depth buffer's
    // sample count must match the color target it is paired with.
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = m_msaaSampleCount;
    depthDesc.SampleDesc.Quality = m_msaaQualityLevel;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClearValue = {};
    depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthClearValue.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue, IID_PPV_ARGS(&m_depthBuffer)));

    // A null desc lets the runtime pick the correct view dimension
    // (TEXTURE2D vs TEXTURE2DMS) from the resource itself, since the format
    // here already matches the resource's format exactly.
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

namespace
{
    ComPtr<ID3D12Resource> CreatePostProcessTexture(
        ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    }
}

void App::InitComputePostProcess()
{
    // The HDR scene resolves into this single-sample texture - created
    // directly in RESOLVE_DEST since its very first use each run is being
    // the target of the MSAA resolve at the end of Pass 2.
    m_hdrResolvedTarget = CreatePostProcessTexture(
        m_device.Get(), m_width, m_height, HdrColorFormat,
        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_RESOLVE_DEST);

    // The bloom ping-pong pair: both need ALLOW_UNORDERED_ACCESS since each
    // is a Dispatch's output at least once per frame, and both are also
    // read as an SRV in between. Both start in NON_PIXEL_SHADER_RESOURCE,
    // which is also the state every frame leaves them in - see the
    // m_isFirstFrame comment in App.h for why that matters.
    m_bloomTargetA = CreatePostProcessTexture(
        m_device.Get(), m_width, m_height, HdrColorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_bloomTargetB = CreatePostProcessTexture(
        m_device.Get(), m_width, m_height, HdrColorFormat,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // The tonemapped LDR result that gets copied into the back buffer -
    // created directly in UNORDERED_ACCESS for the same first-frame reason
    // as m_hdrResolvedTarget above.
    m_finalLdrTarget = CreatePostProcessTexture(
        m_device.Get(), m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // One shader-visible heap holding every descriptor the post-process
    // chain needs, laid out as fixed contiguous table starts so Render()
    // can just pick which table-start handle to bind per dispatch:
    //   [0] SRV hdrResolved, [1] UAV bloomA               - BrightPass table
    //   [2] SRV bloomA,      [3] UAV bloomB               - Blur A->B table
    //   [4] SRV bloomB,      [5] UAV bloomA               - Blur B->A table
    //   [6] SRV hdrResolved, [7] SRV bloomA, [8] UAV final - Tonemap table
    const UINT kDescriptorCount = 9;
    D3D12_DESCRIPTOR_HEAP_DESC computeHeapDesc = {};
    computeHeapDesc.NumDescriptors = kDescriptorCount;
    computeHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    computeHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&computeHeapDesc, IID_PPV_ARGS(&m_computeHeap)));

    const UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_computeHeap->GetCPUDescriptorHandleForHeapStart();

    auto writeSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(resource, &srvDesc, handle);
        handle.ptr += descriptorSize;
    };
    auto writeUav = [&](ID3D12Resource* resource, DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        m_device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
        handle.ptr += descriptorSize;
    };

    writeSrv(m_hdrResolvedTarget.Get(), HdrColorFormat); // [0] BrightPass SRV
    writeUav(m_bloomTargetA.Get(), HdrColorFormat);      // [1] BrightPass UAV
    writeSrv(m_bloomTargetA.Get(), HdrColorFormat);      // [2] Blur A->B SRV
    writeUav(m_bloomTargetB.Get(), HdrColorFormat);      // [3] Blur A->B UAV
    writeSrv(m_bloomTargetB.Get(), HdrColorFormat);      // [4] Blur B->A SRV
    writeUav(m_bloomTargetA.Get(), HdrColorFormat);      // [5] Blur B->A UAV
    writeSrv(m_hdrResolvedTarget.Get(), HdrColorFormat); // [6] Tonemap SRV (scene)
    writeSrv(m_bloomTargetA.Get(), HdrColorFormat);      // [7] Tonemap SRV (bloom)
    writeUav(m_finalLdrTarget.Get(), DXGI_FORMAT_R8G8B8A8_UNORM); // [8] Tonemap UAV

    // BrightPass/Blur root signature: one descriptor table with an SRV (t0)
    // and UAV (u0) back to back. Compute shaders have no per-stage
    // visibility, so this must be D3D12_SHADER_VISIBILITY_ALL.
    D3D12_DESCRIPTOR_RANGE blurRanges[2] = {};
    blurRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    blurRanges[0].NumDescriptors = 1;
    blurRanges[0].BaseShaderRegister = 0;
    blurRanges[0].OffsetInDescriptorsFromTableStart = 0;
    blurRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    blurRanges[1].NumDescriptors = 1;
    blurRanges[1].BaseShaderRegister = 0;
    blurRanges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER blurRootParameter = {};
    blurRootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    blurRootParameter.DescriptorTable.NumDescriptorRanges = _countof(blurRanges);
    blurRootParameter.DescriptorTable.pDescriptorRanges = blurRanges;
    blurRootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC blurRootSignatureDesc = {};
    blurRootSignatureDesc.NumParameters = 1;
    blurRootSignatureDesc.pParameters = &blurRootParameter;
    // No ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT flag here - a compute root
    // signature has no input assembler stage to allow.

    ComPtr<ID3DBlob> blurSignature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &blurRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blurSignature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, blurSignature->GetBufferPointer(), blurSignature->GetBufferSize(), IID_PPV_ARGS(&m_blurRootSignature)));

    // Tonemap root signature: a table with two SRVs (t0 = scene, t1 = bloom)
    // followed by one UAV (u0 = final LDR result).
    D3D12_DESCRIPTOR_RANGE compositeRanges[2] = {};
    compositeRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    compositeRanges[0].NumDescriptors = 2;
    compositeRanges[0].BaseShaderRegister = 0;
    compositeRanges[0].OffsetInDescriptorsFromTableStart = 0;
    compositeRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    compositeRanges[1].NumDescriptors = 1;
    compositeRanges[1].BaseShaderRegister = 0;
    compositeRanges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_ROOT_PARAMETER compositeRootParameter = {};
    compositeRootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    compositeRootParameter.DescriptorTable.NumDescriptorRanges = _countof(compositeRanges);
    compositeRootParameter.DescriptorTable.pDescriptorRanges = compositeRanges;
    compositeRootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC compositeRootSignatureDesc = {};
    compositeRootSignatureDesc.NumParameters = 1;
    compositeRootSignatureDesc.pParameters = &compositeRootParameter;

    ComPtr<ID3DBlob> compositeSignature;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &compositeRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &compositeSignature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, compositeSignature->GetBufferPointer(), compositeSignature->GetBufferSize(),
        IID_PPV_ARGS(&m_compositeRootSignature)));

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileComputeShader = [&](const wchar_t* fileName, const char* entryPointForError) -> ComPtr<ID3DBlob>
    {
        ComPtr<ID3DBlob> shaderBlob;
        ComPtr<ID3DBlob> compileError;
        if (FAILED(D3DCompileFromFile(
            fileName, nullptr, nullptr, "CSMain", "cs_5_0", compileFlags, 0, &shaderBlob, &compileError)))
        {
            throw std::runtime_error(
                compileError ? static_cast<const char*>(compileError->GetBufferPointer()) : entryPointForError);
        }
        return shaderBlob;
    };

    ComPtr<ID3DBlob> brightPassShader = compileComputeShader(L"BrightPass.hlsl", "BrightPass CSMain compile failed");
    ComPtr<ID3DBlob> blurShader = compileComputeShader(L"BlurCompute.hlsl", "Blur CSMain compile failed");
    ComPtr<ID3DBlob> tonemapShader = compileComputeShader(L"Tonemap.hlsl", "Tonemap CSMain compile failed");

    D3D12_COMPUTE_PIPELINE_STATE_DESC brightPassPsoDesc = {};
    brightPassPsoDesc.pRootSignature = m_blurRootSignature.Get();
    brightPassPsoDesc.CS = { brightPassShader->GetBufferPointer(), brightPassShader->GetBufferSize() };
    ThrowIfFailed(m_device->CreateComputePipelineState(&brightPassPsoDesc, IID_PPV_ARGS(&m_brightPassPipelineState)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoDesc = {};
    blurPsoDesc.pRootSignature = m_blurRootSignature.Get();
    blurPsoDesc.CS = { blurShader->GetBufferPointer(), blurShader->GetBufferSize() };
    ThrowIfFailed(m_device->CreateComputePipelineState(&blurPsoDesc, IID_PPV_ARGS(&m_blurPipelineState)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC tonemapPsoDesc = {};
    tonemapPsoDesc.pRootSignature = m_compositeRootSignature.Get();
    tonemapPsoDesc.CS = { tonemapShader->GetBufferPointer(), tonemapShader->GetBufferSize() };
    ThrowIfFailed(m_device->CreateComputePipelineState(&tonemapPsoDesc, IID_PPV_ARGS(&m_tonemapPipelineState)));
}

void App::InitCommandList()
{
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));
    ThrowIfFailed(m_commandList->Close());

    // One allocator/list pair per worker thread (see App.h) - closed here
    // right after creation, same as m_commandList above, since Render()
    // always Resets before recording into any of them.
    for (UINT i = 0; i < WorkerThreadCount; ++i)
    {
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_workerCommandAllocators[i])));
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_workerCommandAllocators[i].Get(), nullptr,
            IID_PPV_ARGS(&m_workerCommandLists[i])));
        ThrowIfFailed(m_workerCommandLists[i]->Close());
    }

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_postCommandAllocator)));
    ThrowIfFailed(m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_postCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_postCommandList)));
    ThrowIfFailed(m_postCommandList->Close());
}

void App::InitFence()
{
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr)
    {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

void App::InitShadowMap()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_shadowDsvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC shadowDesc = {};
    shadowDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    shadowDesc.Width = ShadowMapSize;
    shadowDesc.Height = ShadowMapSize;
    shadowDesc.DepthOrArraySize = 1;
    shadowDesc.MipLevels = 1;
    // Typeless so the same resource can be viewed as a depth buffer (DSV)
    // and later sampled as a plain float texture (SRV) in the main pass.
    shadowDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    shadowDesc.SampleDesc.Count = 1;
    shadowDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &shadowDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(&m_shadowMap)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_shadowMap.Get(), &dsvDesc, m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Root signature: one CBV (b0) with the object's light-space MVP.
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_shadowRootSignature)));

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vertexShader;
    if (FAILED(D3DCompileFromFile(
        L"ShadowShaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Shadow VSMain compile failed");
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_shadowRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    // No pixel shader - this pass only ever writes depth.

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 0;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowPipelineState)));

    m_shadowCubeConstantBuffers = CreateConstantBufferArray(
        m_device.Get(), AlignTo256(sizeof(ShadowConstantBuffer)), CubeCount,
        reinterpret_cast<void**>(&m_mappedShadowCubeConstantBuffers));
    m_shadowPlaneConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(ShadowConstantBuffer), reinterpret_cast<void**>(&m_mappedShadowPlaneConstantBuffer));
}

void App::InitRootSignature()
{
    // The bindless SRV table: a fixed-size range (see BindlessHeapCapacity)
    // rather than the exact-3 table steps 4-13 used. Only 4 of these 16
    // slots are ever populated (InitTextures), and only ever at indices the
    // root constants below actually point to - a real bindless heap works
    // the same way, just at a much larger scale. Root signature version
    // 1.0 with a bounded (not -1/unbounded) range keeps this compatible
    // with Resource Binding Tier 2 hardware; a true unbounded array needs
    // Tier 3, which not all D3D12 GPUs support.
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = BindlessHeapCapacity;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[3] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // BindlessMaterialIndices (b1): which slots of the table below this
    // draw call should read through. Root constants live directly in the
    // root signature (no descriptor/memory indirection), so changing them
    // per draw is cheap - see Render(), which sets these once for the cube
    // and again for the plane instead of switching descriptor tables.
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[1].Constants.ShaderRegister = 1;
    rootParameters[1].Constants.Num32BitValues = sizeof(BindlessMaterialIndices) / sizeof(UINT32);
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[2].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
}

void App::InitPipelineState()
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> error;

    // vs_5_1/ps_5_1 instead of steps 1-13's vs_5_0/ps_5_0: dynamically
    // indexing a resource array (g_bindlessTextures[index] in Shaders.hlsl,
    // where index is a runtime value from a root constant) requires
    // shader model 5.1. This is still the same legacy FXC-based compiler
    // (D3DCompileFromFile) every earlier step used - full bindless (SM6.6
    // dynamic resources) would additionally require switching to DXC.
    if (FAILED(D3DCompileFromFile(
        L"Shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_1", compileFlags, 0, &vertexShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "VSMain compile failed");
    }
    if (FAILED(D3DCompileFromFile(
        L"Shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_1", compileFlags, 0, &pixelShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "PSMain compile failed");
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_rootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    // HDR now (see InitMsaaRenderTarget) - a UNORM target would silently
    // clamp the boosted specular highlight in Update() to 1.0, leaving
    // nothing for BrightPass.hlsl to find.
    psoDesc.RTVFormats[0] = HdrColorFormat;
    // Must match the multisampled color/depth target this PSO renders into
    // (see InitMsaaRenderTarget/InitDepthBuffer) - unlike the shadow PSO,
    // which stays single-sample because it never touches the screen.
    psoDesc.SampleDesc.Count = m_msaaSampleCount;
    psoDesc.SampleDesc.Quality = m_msaaQualityLevel;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
}

void App::InitSceneGeometry()
{
    // Cube: 24 vertices (4 per face), same layout used since step 7.
    const Vertex cubeVertices[] =
    {
        // Front (z = -1), normal (0,0,-1), tangent (1,0,0)
        { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        // Back (z = +1), normal (0,0,1), tangent (-1,0,0)
        { {  1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { -1.0f,  1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        // Left (x = -1), normal (-1,0,0), tangent (0,0,-1)
        { { -1.0f, -1.0f,  1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f,  1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
        { { -1.0f,  1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
        // Right (x = +1), normal (1,0,0), tangent (0,0,1)
        { {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        // Top (y = +1), normal (0,1,0), tangent (1,0,0)
        { { -1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        // Bottom (y = -1), normal (0,-1,0), tangent (1,0,0)
        { { -1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
    };
    const UINT cubeVertexBufferSize = sizeof(cubeVertices);

    std::vector<UINT16> cubeIndices;
    cubeIndices.reserve(36);
    for (UINT16 face = 0; face < 6; ++face)
    {
        const UINT16 base = face * 4;
        const UINT16 faceIndices[] = { base, static_cast<UINT16>(base + 1), static_cast<UINT16>(base + 2),
                                        base, static_cast<UINT16>(base + 2), static_cast<UINT16>(base + 3) };
        cubeIndices.insert(cubeIndices.end(), std::begin(faceIndices), std::end(faceIndices));
    }
    const UINT cubeIndexBufferSize = static_cast<UINT>(cubeIndices.size() * sizeof(UINT16));
    m_indexCount = static_cast<UINT>(cubeIndices.size());

    m_vertexBuffer = CreateUploadBuffer(m_device.Get(), cubeVertices, cubeVertexBufferSize);
    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = cubeVertexBufferSize;

    m_indexBuffer = CreateUploadBuffer(m_device.Get(), cubeIndices.data(), cubeIndexBufferSize);
    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    m_indexBufferView.SizeInBytes = cubeIndexBufferSize;

    // CubeGridDim x CubeGridDim grid centered on the origin, spaced 2.2
    // units apart - wide enough that the (roughly 0.9-unit, see Update's
    // per-cube scale) cubes never overlap, and the whole grid (up to
    // (CubeGridDim - 1) * 2.2 = 6.6 units wide/deep) stays within the
    // ground plane's -6..6 footprint above.
    constexpr float kCubeGridSpacing = 2.2f;
    for (UINT row = 0; row < CubeGridDim; ++row)
    {
        for (UINT col = 0; col < CubeGridDim; ++col)
        {
            const UINT cubeIndex = row * CubeGridDim + col;
            const float x = (static_cast<float>(col) - (CubeGridDim - 1) * 0.5f) * kCubeGridSpacing;
            const float z = (static_cast<float>(row) - (CubeGridDim - 1) * 0.5f) * kCubeGridSpacing;
            m_cubeGridPositions[cubeIndex] = { x, 0.0f, z };
        }
    }

    // Ground plane: one big quad at y = -2, tiling the checkerboard 4x4
    // across it via UVs that go 0..4 instead of 0..1.
    const Vertex planeVertices[] =
    {
        { { -6.0f, -2.0f, -6.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { -6.0f, -2.0f,  6.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 4.0f } },
        { {  6.0f, -2.0f,  6.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 4.0f, 4.0f } },
        { {  6.0f, -2.0f, -6.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 4.0f, 0.0f } },
    };
    const UINT planeVertexBufferSize = sizeof(planeVertices);
    const UINT16 planeIndices[] = { 0, 1, 2, 0, 2, 3 };
    const UINT planeIndexBufferSize = sizeof(planeIndices);
    m_planeIndexCount = _countof(planeIndices);

    m_planeVertexBuffer = CreateUploadBuffer(m_device.Get(), planeVertices, planeVertexBufferSize);
    m_planeVertexBufferView.BufferLocation = m_planeVertexBuffer->GetGPUVirtualAddress();
    m_planeVertexBufferView.StrideInBytes = sizeof(Vertex);
    m_planeVertexBufferView.SizeInBytes = planeVertexBufferSize;

    m_planeIndexBuffer = CreateUploadBuffer(m_device.Get(), planeIndices, planeIndexBufferSize);
    m_planeIndexBufferView.BufferLocation = m_planeIndexBuffer->GetGPUVirtualAddress();
    m_planeIndexBufferView.Format = DXGI_FORMAT_R16_UINT;
    m_planeIndexBufferView.SizeInBytes = planeIndexBufferSize;
}

void App::InitRaytracingAccelerationStructures()
{
    // A geometry desc points the builder straight at the vertex and index
    // buffers the rasterizer already uses - no separate copy of the mesh.
    // They live in upload heaps (see InitSceneGeometry), which is fine:
    // upload-heap resources are permanently in GENERIC_READ, and that
    // includes the NON_PIXEL_SHADER_RESOURCE state a build reads from.
    auto makeGeometryDesc = [](const D3D12_VERTEX_BUFFER_VIEW& vertexBufferView,
                               const D3D12_INDEX_BUFFER_VIEW& indexBufferView,
                               UINT indexCount)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Marking the geometry opaque lets traversal skip any-hit shaders
        // entirely. Both meshes here are solid, and it's also what lets a
        // shadow ray legitimately stop at the first thing it touches.
        desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        desc.Triangles.VertexBuffer.StartAddress = vertexBufferView.BufferLocation;
        desc.Triangles.VertexBuffer.StrideInBytes = vertexBufferView.StrideInBytes;
        desc.Triangles.VertexCount = vertexBufferView.SizeInBytes / vertexBufferView.StrideInBytes;
        // Only the position is read during traversal, so the builder needs
        // the format of that field alone - not of the whole Vertex struct,
        // whose normal/tangent/uv it steps over via StrideInBytes.
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.IndexBuffer = indexBufferView.BufferLocation;
        desc.Triangles.IndexCount = indexCount;
        desc.Triangles.IndexFormat = indexBufferView.Format;
        desc.Triangles.Transform3x4 = 0; // no per-geometry transform; instances carry it
        return desc;
    };

    // Building an acceleration structure is GPU work, so it needs a command
    // list - the same open/record/execute/wait shape InitTextures uses for
    // its texture uploads.
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    // Scratch buffers are only needed while the build runs, so both BLAS
    // builds can share one sized to the larger of the two.
    ComPtr<ID3D12Resource> blasScratch;
    UINT64 blasScratchSize = 0;

    auto buildBottomLevel = [&](const D3D12_RAYTRACING_GEOMETRY_DESC& geometryDesc,
                                ComPtr<ID3D12Resource>& outBlas,
                                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& outInputs,
                                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& outPrebuildInfo)
    {
        outInputs = {};
        outInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        // These meshes are built once and traced against forever, so trade
        // build time for traversal speed.
        outInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        outInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        outInputs.NumDescs = 1;
        outInputs.pGeometryDescs = &geometryDesc;

        // The driver, not the app, decides how much memory a structure
        // needs - the internal layout is entirely vendor-specific.
        outPrebuildInfo = {};
        m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&outInputs, &outPrebuildInfo);
        outBlas = CreateUavBuffer(
            m_device.Get(), outPrebuildInfo.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        blasScratchSize = (std::max)(blasScratchSize, outPrebuildInfo.ScratchDataSizeInBytes);
    };

    const D3D12_RAYTRACING_GEOMETRY_DESC cubeGeometry =
        makeGeometryDesc(m_vertexBufferView, m_indexBufferView, m_indexCount);
    const D3D12_RAYTRACING_GEOMETRY_DESC planeGeometry =
        makeGeometryDesc(m_planeVertexBufferView, m_planeIndexBufferView, m_planeIndexCount);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS cubeInputs = {};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO cubePrebuildInfo = {};
    buildBottomLevel(cubeGeometry, m_cubeBlas, cubeInputs, cubePrebuildInfo);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS planeInputs = {};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO planePrebuildInfo = {};
    buildBottomLevel(planeGeometry, m_planeBlas, planeInputs, planePrebuildInfo);

    blasScratch = CreateUavBuffer(m_device.Get(), blasScratchSize, D3D12_RESOURCE_STATE_COMMON);

    auto recordBuild = [&](const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
                           ID3D12Resource* destination)
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs = inputs;
        buildDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
        buildDesc.DestAccelerationStructureData = destination->GetGPUVirtualAddress();
        m_commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        // Both builds share one scratch buffer, so the second can't start
        // writing it until the first is done reading it. There's no state
        // transition to express that - acceleration structures never leave
        // RAYTRACING_ACCELERATION_STRUCTURE - so a UAV barrier is the only
        // tool for the job.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = destination;
        m_commandList->ResourceBarrier(1, &barrier);
    };

    recordBuild(cubeInputs, m_cubeBlas.Get());
    recordBuild(planeInputs, m_planeBlas.Get());

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForPreviousFrame();

    // The top level is rebuilt every frame rather than here, but its
    // storage and the instance array feeding it are allocated once.
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs = {};
    topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    topLevelInputs.NumDescs = RaytracingInstanceCount;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
    m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);
    m_topLevelAS = CreateUavBuffer(
        m_device.Get(), topLevelPrebuildInfo.ResultDataMaxSizeInBytes,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    m_tlasScratch = CreateUavBuffer(
        m_device.Get(), topLevelPrebuildInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_COMMON);

    // Not a constant buffer, but CreateConstantBuffer is the existing
    // helper for "upload-heap buffer that stays mapped", which is exactly
    // what the instance array needs.
    m_tlasInstanceDescs = CreateConstantBuffer(
        m_device.Get(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * RaytracingInstanceCount,
        reinterpret_cast<void**>(&m_mappedTlasInstanceDescs));

    // Everything about an instance except its transform is fixed for the
    // life of the app, so it's filled in once here; each frame only
    // rewrites the transforms (UpdateTopLevelAccelerationStructure).
    const D3D12_RAYTRACING_INSTANCE_DESC templateInstance = {};
    for (UINT instanceIndex = 0; instanceIndex < RaytracingInstanceCount; ++instanceIndex)
    {
        D3D12_RAYTRACING_INSTANCE_DESC& instance = m_mappedTlasInstanceDescs[instanceIndex];
        instance = templateInstance;
        instance.InstanceID = instanceIndex;
        // A ray's InstanceInclusionMask is AND-ed with this; 0xFF means
        // "every ray can see me". Masks are how a real renderer would, say,
        // keep a character out of its own reflection.
        instance.InstanceMask = 0xFF;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

        const bool isCube = instanceIndex < CubeCount;
        // The one shared cube BLAS for all CubeCount cube instances - the
        // point of having two levels at all.
        instance.AccelerationStructure = isCube
            ? m_cubeBlas->GetGPUVirtualAddress()
            : m_planeBlas->GetGPUVirtualAddress();
        // Which record of the hit group shader table this instance's hits
        // run. Record 0 carries the cube's vertex/index buffers, record 1
        // the plane's (see InitRaytracingShaderTable).
        instance.InstanceContributionToHitGroupIndex = isCube ? 0 : 1;

        // Identity to start with. The cubes get a real transform every
        // frame; the plane keeps this one, since its vertices are already
        // in world space and its world matrix is the identity.
        instance.Transform[0][0] = 1.0f;
        instance.Transform[1][1] = 1.0f;
        instance.Transform[2][2] = 1.0f;
    }
}

void App::UpdateTopLevelAccelerationStructure(ID3D12GraphicsCommandList4* commandList)
{
    // Only the transforms change frame to frame - every other field was
    // filled once in InitRaytracingAccelerationStructures.
    for (UINT cubeIndex = 0; cubeIndex < CubeCount; ++cubeIndex)
    {
        memcpy(m_mappedTlasInstanceDescs[cubeIndex].Transform,
               &m_cubeInstanceTransforms[cubeIndex],
               sizeof(m_mappedTlasInstanceDescs[cubeIndex].Transform));
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = RaytracingInstanceCount;
    inputs.InstanceDescs = m_tlasInstanceDescs->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_topLevelAS->GetGPUVirtualAddress();
    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // The DispatchRays right after this reads what the build just wrote,
    // and there's no state transition available to order the two, so this
    // UAV barrier is what keeps them from overlapping.
    D3D12_RESOURCE_BARRIER tlasBarrier = {};
    tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlasBarrier.UAV.pResource = m_topLevelAS.Get();
    commandList->ResourceBarrier(1, &tlasBarrier);
}

void App::InitConstantBuffer()
{
    m_cubeConstantBuffers = CreateConstantBufferArray(
        m_device.Get(), AlignTo256(sizeof(SceneConstantBuffer)), CubeCount,
        reinterpret_cast<void**>(&m_mappedCubeConstantBuffers));
    m_planeConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(SceneConstantBuffer), reinterpret_cast<void**>(&m_mappedPlaneConstantBuffer));
}

D3D12_GPU_VIRTUAL_ADDRESS App::CubeConstantBufferAddress(UINT cubeIndex) const
{
    return m_cubeConstantBuffers->GetGPUVirtualAddress()
        + static_cast<UINT64>(cubeIndex) * AlignTo256(sizeof(SceneConstantBuffer));
}

D3D12_GPU_VIRTUAL_ADDRESS App::ShadowCubeConstantBufferAddress(UINT cubeIndex) const
{
    return m_shadowCubeConstantBuffers->GetGPUVirtualAddress()
        + static_cast<UINT64>(cubeIndex) * AlignTo256(sizeof(ShadowConstantBuffer));
}

namespace
{
    // Builds a DEFAULT-heap texture from CPU pixels, recording the upload
    // copy into an already-open command list. The caller must keep
    // `uploadBufferKeepAlive` alive until the GPU has finished the copy.
    ComPtr<ID3D12Resource> UploadTexture2D(
        ID3D12Device* device, ID3D12GraphicsCommandList* commandList, UINT width, UINT height,
        const std::vector<UINT32>& pixels, ComPtr<ID3D12Resource>& uploadBufferKeepAlive)
    {
        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;

        ComPtr<ID3D12Resource> texture;
        ThrowIfFailed(device->CreateCommittedResource(
            &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadBufferSize = 0;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);

        uploadBufferKeepAlive = CreateUploadBuffer(device, nullptr, static_cast<UINT>(uploadBufferSize));

        UINT8* mappedData = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(uploadBufferKeepAlive->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
        const UINT8* srcData = reinterpret_cast<const UINT8*>(pixels.data());
        for (UINT row = 0; row < numRows; ++row)
        {
            memcpy(mappedData + footprint.Footprint.RowPitch * row,
                srcData + static_cast<size_t>(width) * 4 * row,
                static_cast<size_t>(rowSizeInBytes));
        }
        uploadBufferKeepAlive->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = texture.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = uploadBufferKeepAlive.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint = footprint;

        commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

        D3D12_RESOURCE_BARRIER toShaderResource = {};
        toShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShaderResource.Transition.pResource = texture.Get();
        toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toShaderResource);

        return texture;
    }
}

void App::InitTextures()
{
    const UINT cellSize = TextureSize / 8;
    // Same 8x8 checkerboard generator as steps 4-13, just parameterized on
    // color so the cube and plane can each get their own diffuse texture -
    // that's what actually makes BindlessMaterialIndices.diffuseTextureIndex
    // matter per draw call instead of always resolving to the same slot.
    auto generateCheckerboard = [&](UINT32 colorA, UINT32 colorB)
    {
        std::vector<UINT32> pixels(static_cast<size_t>(TextureSize) * TextureSize);
        for (UINT y = 0; y < TextureSize; ++y)
        {
            for (UINT x = 0; x < TextureSize; ++x)
            {
                const bool isA = ((x / cellSize) + (y / cellSize)) % 2 == 0;
                pixels[static_cast<size_t>(y) * TextureSize + x] = isA ? colorA : colorB; // 0xAABBGGRR
            }
        }
        return pixels;
    };
    std::vector<UINT32> diffusePixels = generateCheckerboard(0xFFFFFFFFu, 0xFF3050A0u); // cube: white / brown
    std::vector<UINT32> diffusePixels2 = generateCheckerboard(0xFFFFFFFFu, 0xFF8C8C1Eu); // plane: white / teal

    std::vector<UINT32> normalPixels(static_cast<size_t>(TextureSize) * TextureSize);
    auto heightAt = [&](int x, int y) -> float
    {
        const float u = (static_cast<float>((x + TextureSize) % TextureSize) / cellSize) * XM_PI;
        const float v = (static_cast<float>((y + TextureSize) % TextureSize) / cellSize) * XM_PI;
        return sinf(u) * sinf(v);
    };
    const float bumpStrength = 1.5f;
    for (UINT y = 0; y < TextureSize; ++y)
    {
        for (UINT x = 0; x < TextureSize; ++x)
        {
            const float dHeightDx = (heightAt(static_cast<int>(x) + 1, static_cast<int>(y))
                - heightAt(static_cast<int>(x) - 1, static_cast<int>(y))) * 0.5f;
            const float dHeightDy = (heightAt(static_cast<int>(x), static_cast<int>(y) + 1)
                - heightAt(static_cast<int>(x), static_cast<int>(y) - 1)) * 0.5f;

            XMVECTOR n = XMVector3Normalize(XMVectorSet(-dHeightDx * bumpStrength, -dHeightDy * bumpStrength, 1.0f, 0.0f));
            XMFLOAT3 nf;
            XMStoreFloat3(&nf, n);

            const UINT8 r = static_cast<UINT8>((nf.x * 0.5f + 0.5f) * 255.0f);
            const UINT8 g = static_cast<UINT8>((nf.y * 0.5f + 0.5f) * 255.0f);
            const UINT8 b = static_cast<UINT8>((nf.z * 0.5f + 0.5f) * 255.0f);
            normalPixels[static_cast<size_t>(y) * TextureSize + x] =
                (0xFFu << 24) | (b << 16) | (g << 8) | r; // 0xAABBGGRR
        }
    }

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    ComPtr<ID3D12Resource> diffuseUploadBuffer;
    ComPtr<ID3D12Resource> diffuseUploadBuffer2;
    ComPtr<ID3D12Resource> normalUploadBuffer;
    m_diffuseTexture = UploadTexture2D(
        m_device.Get(), m_commandList.Get(), TextureSize, TextureSize, diffusePixels, diffuseUploadBuffer);
    m_diffuseTexture2 = UploadTexture2D(
        m_device.Get(), m_commandList.Get(), TextureSize, TextureSize, diffusePixels2, diffuseUploadBuffer2);
    m_normalMapTexture = UploadTexture2D(
        m_device.Get(), m_commandList.Get(), TextureSize, TextureSize, normalPixels, normalUploadBuffer);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForPreviousFrame();

    // Sized to BindlessHeapCapacity (16), not to the 4 descriptors actually
    // written below - see the comment on that constant in App.h. The
    // unused slots are simply never created and never indexed.
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = BindlessHeapCapacity;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    const UINT srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    // Index 0: cube's diffuse texture.
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateShaderResourceView(m_diffuseTexture.Get(), &srvDesc, srvHandle);
    srvHandle.ptr += srvDescriptorSize;

    // Index 1: plane's diffuse texture - a different texture in the same
    // bindless table, selected purely by the index Render() passes per draw.
    m_device->CreateShaderResourceView(m_diffuseTexture2.Get(), &srvDesc, srvHandle);
    srvHandle.ptr += srvDescriptorSize;

    // Index 2: normal map (shared by both objects).
    m_device->CreateShaderResourceView(m_normalMapTexture.Get(), &srvDesc, srvHandle);
    srvHandle.ptr += srvDescriptorSize;

    // Index 3: shadow map (shared by both objects).
    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
    shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, srvHandle);
}

void App::InitRaytracingOutput()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC maskDesc = {};
    maskDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    maskDesc.Width = m_width;
    maskDesc.Height = m_height;
    maskDesc.DepthOrArraySize = 1;
    maskDesc.MipLevels = 1;
    // One channel is all a visibility mask needs. Note this is a plain
    // single-sample texture even though the color pass that reads it is
    // 4x MSAA - one ray per pixel, shared by all four subsamples.
    maskDesc.Format = DXGI_FORMAT_R8_UNORM;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &maskDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_shadowMask)));

    D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
    uavHeapDesc.NumDescriptors = 1;
    uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&m_raytracingUavHeap)));

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_shadowMask.Get(), nullptr, &uavDesc,
        m_raytracingUavHeap->GetCPUDescriptorHandleForHeapStart());

    // The read side goes into the step 14 bindless table, so the pixel
    // shader reaches the mask the same way it reaches any other texture -
    // by index, with no extra descriptor table to bind.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    const UINT srvDescriptorSize =
        m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(ShadowMaskBindlessIndex) * srvDescriptorSize;
    m_device->CreateShaderResourceView(m_shadowMask.Get(), &srvDesc, srvHandle);

    m_raytracingConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(RaytracingConstantBuffer),
        reinterpret_cast<void**>(&m_mappedRaytracingConstantBuffer));
}

void App::InitRaytracingRootSignatures()
{
    // Global root signature: bound once per command list with the ordinary
    // SetComputeRoot* calls, and visible to every shader in the state
    // object regardless of which record dispatched it.
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0; // u0 - the shadow mask

    D3D12_ROOT_PARAMETER globalParameters[3] = {};
    globalParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    globalParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    globalParameters[0].DescriptorTable.pDescriptorRanges = &uavRange;
    // The TLAS binds as a root SRV rather than through a heap: an
    // acceleration structure has no CPU descriptor to create in the first
    // place, only a GPU virtual address, so a root descriptor is the
    // natural fit.
    globalParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    globalParameters[1].Descriptor.ShaderRegister = 0; // t0
    globalParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    globalParameters[2].Descriptor.ShaderRegister = 0; // b0

    D3D12_ROOT_SIGNATURE_DESC globalDesc = {};
    globalDesc.NumParameters = _countof(globalParameters);
    globalDesc.pParameters = globalParameters;
    // None of the input-assembler flags the graphics root signatures use
    // mean anything here - there's no vertex stage in a raytracing pass.
    globalDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &globalDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&m_raytracingGlobalRootSignature)));

    // Local root signature: its arguments come from the shader table
    // record that dispatched the shader, not from any command list call.
    // That's what lets one ClosestHitShader read the cube's buffers when
    // it runs for a cube and the plane's when it runs for the plane.
    D3D12_ROOT_PARAMETER localParameters[2] = {};
    localParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    localParameters[0].Descriptor.ShaderRegister = 1; // t1 - vertex buffer
    localParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    localParameters[1].Descriptor.ShaderRegister = 2; // t2 - index buffer

    D3D12_ROOT_SIGNATURE_DESC localDesc = {};
    localDesc.NumParameters = _countof(localParameters);
    localDesc.pParameters = localParameters;
    // This single flag is the entire difference between a local and a
    // global root signature at the API level.
    localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3DBlob> localSignature;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &localDesc, D3D_ROOT_SIGNATURE_VERSION_1, &localSignature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, localSignature->GetBufferPointer(), localSignature->GetBufferSize(),
        IID_PPV_ARGS(&m_raytracingLocalRootSignature)));
}

void App::InitRaytracingPipeline()
{
    ComPtr<IDxcBlob> library = CompileRaytracingLibrary(L"RayTracing.hlsl");

    // A state object is described as a flat array of subobjects, each a
    // type tag plus a pointer to a desc. Most samples build this through
    // the CD3DX12_STATE_OBJECT_DESC helper in d3dx12.h; doing it by hand
    // keeps the dependency list empty and, more usefully, keeps every part
    // of the structure visible. All the descs below are locals, which is
    // fine because none of them outlive the CreateStateObject call.
    //
    // Eight subobjects: the library, two hit groups, the shader config,
    // the local root signature and its export association, the global
    // root signature, and the pipeline config.
    D3D12_STATE_SUBOBJECT subobjects[8] = {};
    UINT subobjectIndex = 0;

    // 1. The compiled library, and which of its exports this state object
    //    uses. Naming them rather than passing zero exports (which would
    //    take everything) keeps the state object honest about its contents.
    D3D12_EXPORT_DESC exportDescs[4] = {};
    exportDescs[0].Name = kRayGenShaderName;
    exportDescs[1].Name = kClosestHitShaderName;
    exportDescs[2].Name = kMissShaderName;
    exportDescs[3].Name = kShadowMissShaderName;

    D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
    libraryDesc.DXILLibrary.pShaderBytecode = library->GetBufferPointer();
    libraryDesc.DXILLibrary.BytecodeLength = library->GetBufferSize();
    libraryDesc.NumExports = _countof(exportDescs);
    libraryDesc.pExports = exportDescs;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[subobjectIndex].pDesc = &libraryDesc;
    ++subobjectIndex;

    // 2/3. Two hit groups over the *same* closest-hit shader. They exist
    //      as separate groups purely so the shader table can hand each one
    //      different local root arguments - the cube's vertex/index buffer
    //      addresses for one, the plane's for the other.
    D3D12_HIT_GROUP_DESC cubeHitGroup = {};
    cubeHitGroup.HitGroupExport = kCubeHitGroupName;
    cubeHitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    cubeHitGroup.ClosestHitShaderImport = kClosestHitShaderName;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[subobjectIndex].pDesc = &cubeHitGroup;
    ++subobjectIndex;

    D3D12_HIT_GROUP_DESC planeHitGroup = {};
    planeHitGroup.HitGroupExport = kPlaneHitGroupName;
    planeHitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    planeHitGroup.ClosestHitShaderImport = kClosestHitShaderName;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[subobjectIndex].pDesc = &planeHitGroup;
    ++subobjectIndex;

    // 4. Payload and attribute sizes. The driver reserves per-ray stack
    //    space from these numbers, so they have to cover the *largest*
    //    payload any shader here uses - RayPayload's float + float3, not
    //    the smaller ShadowPayload.
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = 4 * sizeof(float);
    shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float); // triangle barycentrics
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[subobjectIndex].pDesc = &shaderConfig;
    ++subobjectIndex;

    // 5/6. The local root signature, then an association naming which
    //      exports it applies to. Without the association the runtime has
    //      no way to tell the local signature belongs to the hit groups
    //      rather than to the raygen or miss shaders.
    ID3D12RootSignature* localRootSignature = m_raytracingLocalRootSignature.Get();
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    subobjects[subobjectIndex].pDesc = &localRootSignature;
    const D3D12_STATE_SUBOBJECT* localRootSignatureSubobject = &subobjects[subobjectIndex];
    ++subobjectIndex;

    const wchar_t* hitGroupExports[] = { kCubeHitGroupName, kPlaneHitGroupName };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
    association.pSubobjectToAssociate = localRootSignatureSubobject;
    association.NumExports = _countof(hitGroupExports);
    association.pExports = hitGroupExports;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subobjects[subobjectIndex].pDesc = &association;
    ++subobjectIndex;

    // 7. The global root signature. No association needed - with none, it
    //    applies to everything, which is exactly what "global" means.
    ID3D12RootSignature* globalRootSignature = m_raytracingGlobalRootSignature.Get();
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[subobjectIndex].pDesc = &globalRootSignature;
    ++subobjectIndex;

    // 8. Recursion depth. Both rays are fired from RayGenShader rather
    //    than the shadow ray being spawned inside ClosestHitShader, so a
    //    depth of 1 covers it. Every extra level costs stack the driver
    //    must reserve for every ray in flight, so the limit is worth
    //    keeping as tight as the shaders actually need.
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 1;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[subobjectIndex].pDesc = &pipelineConfig;
    ++subobjectIndex;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = subobjectIndex;
    stateObjectDesc.pSubobjects = subobjects;
    ThrowIfFailed(m_dxrDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_raytracingStateObject)));

    // The interface that turns export names into the shader identifiers
    // the shader table is built out of.
    ThrowIfFailed(m_raytracingStateObject.As(&m_raytracingStateObjectProperties));
}

void App::InitRaytracingShaderTable()
{
    // Two alignment rules apply here, and they are deliberately different
    // numbers:
    //   - every record's stride must be a multiple of 32
    //     (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT), and
    //   - every table's start address must be a multiple of 64
    //     (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT).
    // Giving each table its own committed resource satisfies the second
    // rule for free, since committed buffers already start well past a
    // 64-byte boundary.
    const UINT identifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    auto alignUp = [](UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    };

    auto shaderIdentifier = [&](const wchar_t* exportName)
    {
        void* identifier = m_raytracingStateObjectProperties->GetShaderIdentifier(exportName);
        // A misspelled export name doesn't fail anywhere earlier - it just
        // returns null here, and a table full of zeroes then produces a
        // device removal at DispatchRays time with no useful diagnostic.
        if (identifier == nullptr)
        {
            throw std::runtime_error("The raytracing state object has no export by that name.");
        }
        return identifier;
    };

    // RayGen: exactly one record, and no local root arguments, so the
    // record is nothing but the identifier.
    {
        const UINT tableSize = alignUp(identifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        std::vector<uint8_t> table(tableSize, 0);
        memcpy(table.data(), shaderIdentifier(kRayGenShaderName), identifierSize);
        m_rayGenShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), tableSize);
    }

    // Miss: two records, also identifier-only. Their order in this table
    // is precisely what TraceRay's MissShaderIndex argument selects -
    // index 0 for the camera ray, index 1 for the shadow ray.
    {
        m_missShaderTableStride = alignUp(identifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_missShaderTableSize = m_missShaderTableStride * 2;
        std::vector<uint8_t> table(m_missShaderTableSize, 0);
        memcpy(table.data(), shaderIdentifier(kMissShaderName), identifierSize);
        memcpy(table.data() + m_missShaderTableStride, shaderIdentifier(kShadowMissShaderName), identifierSize);
        m_missShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), m_missShaderTableSize);
    }

    // Hit groups: two records, each carrying two root SRV addresses as
    // local root arguments - 16 bytes on top of the 32-byte identifier.
    // This is what a stride is actually for. The records genuinely differ
    // in content, so the GPU can't just reuse one; it has to step by a
    // fixed size to reach the record the traversal picked.
    {
        const UINT localArgumentSize = 2 * sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        m_hitGroupShaderTableStride =
            alignUp(identifierSize + localArgumentSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_hitGroupShaderTableSize = m_hitGroupShaderTableStride * 2;
        std::vector<uint8_t> table(m_hitGroupShaderTableSize, 0);

        auto writeHitGroupRecord = [&](UINT recordIndex, const wchar_t* hitGroupName,
                                       ID3D12Resource* vertexBuffer, ID3D12Resource* indexBuffer)
        {
            uint8_t* record = table.data() + static_cast<size_t>(recordIndex) * m_hitGroupShaderTableStride;
            memcpy(record, shaderIdentifier(hitGroupName), identifierSize);
            // Root descriptors inside a shader record are raw GPU virtual
            // addresses laid out in the order the local root signature
            // declares them (t1 = vertices, t2 = indices) - there's no
            // descriptor heap involved at all.
            const D3D12_GPU_VIRTUAL_ADDRESS addresses[2] =
            {
                vertexBuffer->GetGPUVirtualAddress(),
                indexBuffer->GetGPUVirtualAddress(),
            };
            memcpy(record + identifierSize, addresses, sizeof(addresses));
        };

        // Record 0 is what TLAS instances with
        // InstanceContributionToHitGroupIndex == 0 select - the cubes.
        // Record 1 is the ground plane's.
        writeHitGroupRecord(0, kCubeHitGroupName, m_vertexBuffer.Get(), m_indexBuffer.Get());
        writeHitGroupRecord(1, kPlaneHitGroupName, m_planeVertexBuffer.Get(), m_planeIndexBuffer.Get());

        m_hitGroupShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), m_hitGroupShaderTableSize);
    }
}

void App::InitSkybox()
{
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    ThrowIfFailed(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(
        0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_skyboxRootSignature)));

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    if (FAILED(D3DCompileFromFile(
        L"SkyboxShaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Skybox VSMain compile failed");
    }
    if (FAILED(D3DCompileFromFile(
        L"SkyboxShaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "Skybox PSMain compile failed");
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
    psoDesc.pRootSignature = m_skyboxRootSignature.Get();
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = HdrColorFormat;
    // Same multisampled HDR target as the main PSO - see InitPipelineState.
    psoDesc.SampleDesc.Count = m_msaaSampleCount;
    psoDesc.SampleDesc.Quality = m_msaaQualityLevel;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_skyboxPipelineState)));

    const SkyboxVertex vertices[] =
    {
        { { -1.0f, -1.0f, -1.0f } },
        { { -1.0f,  1.0f, -1.0f } },
        { {  1.0f,  1.0f, -1.0f } },
        { {  1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } },
        { { -1.0f,  1.0f,  1.0f } },
        { {  1.0f,  1.0f,  1.0f } },
        { {  1.0f, -1.0f,  1.0f } },
    };
    const UINT vertexBufferSize = sizeof(vertices);

    const UINT16 indices[] =
    {
        0, 1, 2, 0, 2, 3, // front  (z = -1)
        4, 6, 5, 4, 7, 6, // back   (z = +1)
        4, 5, 1, 4, 1, 0, // left   (x = -1)
        3, 2, 6, 3, 6, 7, // right  (x = +1)
        1, 5, 6, 1, 6, 2, // top    (y = +1)
        4, 0, 3, 4, 3, 7, // bottom (y = -1)
    };
    const UINT indexBufferSize = sizeof(indices);
    m_skyboxIndexCount = _countof(indices);

    m_skyboxVertexBuffer = CreateUploadBuffer(m_device.Get(), vertices, vertexBufferSize);
    m_skyboxVertexBufferView.BufferLocation = m_skyboxVertexBuffer->GetGPUVirtualAddress();
    m_skyboxVertexBufferView.StrideInBytes = sizeof(SkyboxVertex);
    m_skyboxVertexBufferView.SizeInBytes = vertexBufferSize;

    m_skyboxIndexBuffer = CreateUploadBuffer(m_device.Get(), indices, indexBufferSize);
    m_skyboxIndexBufferView.BufferLocation = m_skyboxIndexBuffer->GetGPUVirtualAddress();
    m_skyboxIndexBufferView.Format = DXGI_FORMAT_R16_UINT;
    m_skyboxIndexBufferView.SizeInBytes = indexBufferSize;

    m_skyboxConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(SkyboxConstantBuffer), reinterpret_cast<void**>(&m_mappedSkyboxConstantBuffer));
}

void App::Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float elapsedSeconds =
        static_cast<float>(now.QuadPart - m_startTime.QuadPart) / static_cast<float>(m_perfFrequency.QuadPart);

    const XMMATRIX planeWorld = XMMatrixIdentity();
    const XMMATRIX view = XMMatrixLookAtLH(kEyePosition, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, static_cast<float>(m_width) / static_cast<float>(m_height), 0.1f, 100.0f);

    // Light-space view/projection: an orthographic camera sitting back
    // along the (reversed) light direction, looking at the scene's center.
    const XMVECTOR lightPosition = XMVectorScale(kLightDirection, -15.0f);
    const XMMATRIX lightView = XMMatrixLookAtLH(lightPosition, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX lightProjection = XMMatrixOrthographicLH(20.0f, 20.0f, 0.1f, 30.0f);
    const XMMATRIX lightViewProjection = lightView * lightProjection;

    auto fillObjectConstants = [&](SceneConstantBuffer* cb, const XMMATRIX& world, float metallic, float roughness)
    {
        XMStoreFloat4x4(&cb->mvp, XMMatrixTranspose(world * view * projection));
        XMStoreFloat4x4(&cb->world, XMMatrixTranspose(world));
        XMStoreFloat4x4(&cb->lightMVP, XMMatrixTranspose(world * lightViewProjection));
        XMStoreFloat4(&cb->lightDirection, kLightDirection);
        // The Cook-Torrance diffuse term (see Shaders.hlsl) divides by PI,
        // which steps 1-12's un-normalized Lambertian diffuse didn't - so
        // the light is boosted here to land the overall scene brightness
        // back in the same ballpark as those earlier steps.
        cb->lightColor = { 2.5f, 2.5f, 2.4f, 1.0f };
        // A fully metallic surface has zero diffuse (see kD in Shaders.hlsl),
        // so without any image-based lighting (deferred to a later step -
        // see the roadmap) its non-highlighted areas would have nothing but
        // this ambient term to go on. Bumped up from a dim 0.12 mainly to
        // keep the metal cube's checker texture legible outside its
        // specular hotspot, standing in for the environment reflections a
        // real metal would be showing.
        cb->ambientColor = { 0.22f, 0.22f, 0.22f, 1.0f };
        XMStoreFloat4(&cb->eyePosition, kEyePosition);
        cb->materialParams = { metallic, roughness, 0.0f, 0.0f };
    };
    // Metallic cube grid (shiny, fairly low roughness): almost no diffuse, a
    // bright albedo-tinted specular hotspot - easily bright enough to feed
    // BrightPass.hlsl's bloom extraction, the same way step 12's
    // artificially boosted specular color did. Each cube spins at its own
    // phase (offset by its grid index) purely so the grid reads as CubeCount
    // independent objects rather than one shape copy-pasted CubeCount times.
    constexpr UINT CubeCBStride = AlignTo256(sizeof(SceneConstantBuffer));
    constexpr UINT ShadowCubeCBStride = AlignTo256(sizeof(ShadowConstantBuffer));
    for (UINT cubeIndex = 0; cubeIndex < CubeCount; ++cubeIndex)
    {
        const float phase = static_cast<float>(cubeIndex) * 0.35f;
        const XMMATRIX cubeSpin =
            XMMatrixRotationRollPitchYaw(elapsedSeconds * 0.7f + phase, elapsedSeconds + phase * 0.5f, 0.0f);
        const XMVECTOR gridPosition = XMLoadFloat3(&m_cubeGridPositions[cubeIndex]);
        const XMMATRIX cubeWorld =
            XMMatrixScaling(0.45f, 0.45f, 0.45f) * cubeSpin * XMMatrixTranslationFromVector(gridPosition);

        // The very same matrix the raster constant buffer gets, in the 3x4
        // layout D3D12_RAYTRACING_INSTANCE_DESC wants. XMStoreFloat3x4
        // transposes as it stores, which is exactly the conversion from
        // DirectXMath's row-vector convention to DXR's column-vector one.
        // Feeding both paths from one matrix is what guarantees the
        // raytraced shadows line up with the rasterized cubes.
        XMStoreFloat3x4(&m_cubeInstanceTransforms[cubeIndex], cubeWorld);

        SceneConstantBuffer* cb = reinterpret_cast<SceneConstantBuffer*>(
            m_mappedCubeConstantBuffers + static_cast<size_t>(cubeIndex) * CubeCBStride);
        fillObjectConstants(cb, cubeWorld, 0.9f, 0.4f);

        ShadowConstantBuffer* shadowCb = reinterpret_cast<ShadowConstantBuffer*>(
            m_mappedShadowCubeConstantBuffers + static_cast<size_t>(cubeIndex) * ShadowCubeCBStride);
        XMStoreFloat4x4(&shadowCb->mvp, XMMatrixTranspose(cubeWorld * lightViewProjection));
    }
    // Non-metal ground plane (duller, higher roughness): mostly diffuse
    // with only a faint, wide dielectric highlight (F0 = 0.04 in the
    // shader) - the two materials side by side show what metallic/
    // roughness actually change about a surface's look.
    fillObjectConstants(m_mappedPlaneConstantBuffer, planeWorld, 0.0f, 0.7f);

    XMStoreFloat4x4(&m_mappedShadowPlaneConstantBuffer->mvp, XMMatrixTranspose(planeWorld * lightViewProjection));

    // RayGenShader unprojects NDC through this to build its camera rays,
    // so it has to invert exactly the view-projection the raster passes
    // use - anything else and the mask would sit slightly off from the
    // image it gets composited into.
    const XMMATRIX viewProjection = view * projection;
    XMStoreFloat4x4(&m_mappedRaytracingConstantBuffer->inverseViewProjection,
                    XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
    XMStoreFloat4(&m_mappedRaytracingConstantBuffer->cameraPosition, kEyePosition);
    XMStoreFloat4(&m_mappedRaytracingConstantBuffer->lightDirection, kLightDirection);

    const XMMATRIX skyboxWorld = XMMatrixScaling(kSkyboxScale, kSkyboxScale, kSkyboxScale);
    XMStoreFloat4x4(&m_mappedSkyboxConstantBuffer->mvp, XMMatrixTranspose(skyboxWorld * view * projection));
    m_mappedSkyboxConstantBuffer->skyColor = { 0.35f, 0.55f, 0.9f, 1.0f };
    m_mappedSkyboxConstantBuffer->horizonColor = { 0.85f, 0.9f, 0.95f, 1.0f };
}

void App::RenderRaytracedShadows(ID3D12GraphicsCommandList4* commandList)
{
    // Raytracing root arguments go through the *compute* setters, not the
    // graphics ones: DispatchRays lives on the compute side of the API
    // even though what it produces here feeds a graphics pass.
    commandList->SetComputeRootSignature(m_raytracingGlobalRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_raytracingUavHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootDescriptorTable(0, m_raytracingUavHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootShaderResourceView(1, m_topLevelAS->GetGPUVirtualAddress());
    commandList->SetComputeRootConstantBufferView(2, m_raytracingConstantBuffer->GetGPUVirtualAddress());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    // The raygen table holds exactly one record, so it needs no stride -
    // this is the only one of the three described by size alone.
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable->GetDesc().Width;
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missShaderTableSize;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderTableStride;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderTableSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderTableStride;
    // One ray per pixel of the final image - RayGenShader runs once per
    // entry in this grid, the same way a compute shader thread does.
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;

    // SetPipelineState1, not SetPipelineState - a state object isn't a PSO
    // and doesn't go through the same setter.
    commandList->SetPipelineState1(m_raytracingStateObject.Get());
    commandList->DispatchRays(&dispatchDesc);
}

void App::Render()
{
    Update();

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    // Pass 0: the raytraced shadow mask, which has to exist before the
    // color pass samples it. Building it from camera rays rather than from
    // the color pass's depth buffer is exactly what lets it run first -
    // reconstructing world positions from depth would mean the color pass
    // needed the mask and the mask needed the color pass, so a depth
    // pre-pass would have to be added to break the cycle.
    if (!m_isFirstFrame)
    {
        // The mask is created in UNORDERED_ACCESS, so frame 1 skips this;
        // every frame after that, the color pass left it as a pixel shader
        // resource and DispatchRays needs it writable again.
        D3D12_RESOURCE_BARRIER maskToWrite = {};
        maskToWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        maskToWrite.Transition.pResource = m_shadowMask.Get();
        maskToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        maskToWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        maskToWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &maskToWrite);
    }

    UpdateTopLevelAccelerationStructure(m_commandList.Get());
    RenderRaytracedShadows(m_commandList.Get());

    D3D12_RESOURCE_BARRIER maskToRead = {};
    maskToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    maskToRead.Transition.pResource = m_shadowMask.Get();
    maskToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    maskToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    maskToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &maskToRead);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Pass 1: render depth-only, from the light's point of view, into the
    // shadow map. The resource is created already in DEPTH_WRITE, so the
    // very first frame skips this transition; every frame after that needs
    // it since the previous frame left it in PIXEL_SHADER_RESOURCE.
    if (!m_isFirstFrame)
    {
        D3D12_RESOURCE_BARRIER shadowToWrite = {};
        shadowToWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        shadowToWrite.Transition.pResource = m_shadowMap.Get();
        shadowToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        shadowToWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        shadowToWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &shadowToWrite);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvHandle = m_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsvHandle);
    m_commandList->RSSetViewports(1, &m_shadowViewport);
    m_commandList->RSSetScissorRects(1, &m_shadowScissorRect);

    m_commandList->SetPipelineState(m_shadowPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_shadowRootSignature.Get());

    // Depth-only, so this stays a plain serial loop on the main thread even
    // though there are CubeCount of them - the color pass below is where
    // splitting the per-cube draws across worker threads actually matters
    // (it also runs the full PBR pixel shader, not just a depth write).
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);
    for (UINT cubeIndex = 0; cubeIndex < CubeCount; ++cubeIndex)
    {
        m_commandList->SetGraphicsRootConstantBufferView(0, ShadowCubeConstantBufferAddress(cubeIndex));
        m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }

    m_commandList->SetGraphicsRootConstantBufferView(0, m_shadowPlaneConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_planeVertexBufferView);
    m_commandList->IASetIndexBuffer(&m_planeIndexBufferView);
    m_commandList->DrawIndexedInstanced(m_planeIndexCount, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER shadowToRead = {};
    shadowToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    shadowToRead.Transition.pResource = m_shadowMap.Get();
    shadowToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    shadowToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    shadowToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &shadowToRead);

    // Pass 2: the normal color pass - skybox and plane here, then the
    // CubeCount-cube grid lit with the shadow map sampled in, split across
    // WorkerThreadCount worker command lists below (RecordWorkerCommandList).
    // This renders into the off-screen multisampled color/depth target
    // (m_msaaColorTarget/m_depthBuffer) rather than the back buffer
    // directly - see InitMsaaRenderTarget for why a flip-model swapchain
    // can't be the multisample target itself. Clearing happens only here,
    // before any worker list runs, since OMSetRenderTargets in a worker
    // list only binds the target - it never re-clears it.
    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE msaaRtvHandle = m_msaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &msaaRtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_commandList->ClearRenderTargetView(msaaRtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->SetPipelineState(m_skyboxPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_skyboxRootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_skyboxConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_skyboxVertexBufferView);
    m_commandList->IASetIndexBuffer(&m_skyboxIndexBufferView);
    m_commandList->DrawIndexedInstanced(m_skyboxIndexCount, 1, 0, 0, 0);

    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    // Bound once for the plane draw below and every worker's cube draws -
    // unlike steps 4-13, switching which texture a draw actually samples no
    // longer means switching descriptor tables, just the small root-constant
    // index set before each draw.
    m_commandList->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    // diffuse2@1, normalMap@2, shadowMap@3, then the current shadowing
    // technique and the raytraced mask's slot.
    const BindlessMaterialIndices planeIndices =
        { 1, 2, 3, m_shadowMode, ShadowMaskBindlessIndex, { 0, 0, 0 } };
    m_commandList->SetGraphicsRoot32BitConstants(1, sizeof(planeIndices) / sizeof(UINT32), &planeIndices, 0);
    m_commandList->SetGraphicsRootConstantBufferView(0, m_planeConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_planeVertexBufferView);
    m_commandList->IASetIndexBuffer(&m_planeIndexBufferView);
    m_commandList->DrawIndexedInstanced(m_planeIndexCount, 1, 0, 0, 0);

    ThrowIfFailed(m_commandList->Close());

    // The cube grid's draw calls are the one part of this frame with enough
    // independent, identically-shaped work to be worth splitting across
    // threads. Each worker gets its own already-created command list/
    // allocator (InitCommandList) and records CubesPerWorker cubes into it
    // completely independently - no locking needed, since each thread only
    // ever touches its own command list and only reads shared, unchanging
    // data (the vertex/index buffers, root signature, PSO, SRV heap).
    std::array<std::thread, WorkerThreadCount> workerThreads;
    for (UINT threadIndex = 0; threadIndex < WorkerThreadCount; ++threadIndex)
    {
        workerThreads[threadIndex] = std::thread(&App::RecordWorkerCommandList, this, threadIndex);
    }
    for (std::thread& workerThread : workerThreads)
    {
        workerThread.join();
    }

    // Everything from here on runs after the cube grid is fully recorded,
    // so it moves into its own command list (m_postCommandList) rather than
    // continuing in m_commandList, which is already closed above.
    ThrowIfFailed(m_postCommandAllocator->Reset());
    ThrowIfFailed(m_postCommandList->Reset(m_postCommandAllocator.Get(), nullptr));

    // Pass 3: resolve the multisampled HDR color target into an off-screen
    // single-sample HDR texture (a compute shader can only read a plain
    // Texture2D SRV, not a multisampled one) - the bloom/tonemap chain
    // below reads this instead of going straight to the back buffer as
    // step 10/11 did.
    D3D12_RESOURCE_BARRIER preResolveBarriers[2] = {};
    UINT preResolveBarrierCount = 0;
    preResolveBarriers[preResolveBarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preResolveBarriers[preResolveBarrierCount].Transition.pResource = m_msaaColorTarget.Get();
    preResolveBarriers[preResolveBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    preResolveBarriers[preResolveBarrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    preResolveBarriers[preResolveBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++preResolveBarrierCount;
    if (!m_isFirstFrame)
    {
        // m_hdrResolvedTarget is already RESOLVE_DEST on frame 1 (see
        // InitComputePostProcess) - every frame after that, the tonemap
        // pass at the end of the previous frame left it as an SRV.
        preResolveBarriers[preResolveBarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        preResolveBarriers[preResolveBarrierCount].Transition.pResource = m_hdrResolvedTarget.Get();
        preResolveBarriers[preResolveBarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        preResolveBarriers[preResolveBarrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
        preResolveBarriers[preResolveBarrierCount].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++preResolveBarrierCount;
    }
    m_postCommandList->ResourceBarrier(preResolveBarrierCount, preResolveBarriers);

    m_postCommandList->ResolveSubresource(m_hdrResolvedTarget.Get(), 0, m_msaaColorTarget.Get(), 0, HdrColorFormat);

    D3D12_RESOURCE_BARRIER postResolveBarriers[2] = {};
    postResolveBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postResolveBarriers[0].Transition.pResource = m_msaaColorTarget.Get();
    postResolveBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    postResolveBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postResolveBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postResolveBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postResolveBarriers[1].Transition.pResource = m_hdrResolvedTarget.Get();
    postResolveBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    postResolveBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    postResolveBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_postCommandList->ResourceBarrier(_countof(postResolveBarriers), postResolveBarriers);

    // Pass 4: BrightPass - keep only the over-bright pixels (the boosted
    // specular highlight from Update()), writing them into m_bloomTargetA.
    ID3D12DescriptorHeap* computeHeaps[] = { m_computeHeap.Get() };
    m_postCommandList->SetDescriptorHeaps(_countof(computeHeaps), computeHeaps);
    const UINT descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE computeHeapStart = m_computeHeap->GetGPUDescriptorHandleForHeapStart();
    auto tableAt = [&](UINT index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = computeHeapStart;
        handle.ptr += static_cast<UINT64>(index) * descriptorSize;
        return handle;
    };

    // One thread per pixel, in 8x8 groups - round the dispatch grid up so
    // groups fully cover the image even when width/height aren't multiples
    // of 8 (every compute shader here discards the extra off-edge threads
    // itself).
    const UINT groupCountX = (m_width + 7) / 8;
    const UINT groupCountY = (m_height + 7) / 8;

    D3D12_RESOURCE_BARRIER bloomAToUav = {};
    bloomAToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bloomAToUav.Transition.pResource = m_bloomTargetA.Get();
    bloomAToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bloomAToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bloomAToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_postCommandList->ResourceBarrier(1, &bloomAToUav);

    m_postCommandList->SetPipelineState(m_brightPassPipelineState.Get());
    m_postCommandList->SetComputeRootSignature(m_blurRootSignature.Get());
    m_postCommandList->SetComputeRootDescriptorTable(0, tableAt(0)); // SRV hdrResolved, UAV bloomA
    m_postCommandList->Dispatch(groupCountX, groupCountY, 1);

    D3D12_RESOURCE_BARRIER bloomAToSrv = {};
    bloomAToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bloomAToSrv.Transition.pResource = m_bloomTargetA.Get();
    bloomAToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bloomAToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bloomAToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_postCommandList->ResourceBarrier(1, &bloomAToSrv);

    // Pass 5: blur the bright pixels into a soft halo, ping-ponging between
    // m_bloomTargetA/B so each iteration reads the previous one's result.
    // BloomBlurIterations must stay even: the Tonemap pass below always
    // reads its bloom input from m_bloomTargetA (see InitComputePostProcess's
    // descriptor [7]), which only holds the final blurred result if the
    // ping-pong loop runs an even number of times.
    ID3D12Resource* bloomTargets[2] = { m_bloomTargetA.Get(), m_bloomTargetB.Get() };
    const D3D12_GPU_DESCRIPTOR_HANDLE blurTables[2] = { tableAt(2), tableAt(4) }; // [A->B, B->A]
    m_postCommandList->SetPipelineState(m_blurPipelineState.Get());
    UINT srcIndex = 0; // m_bloomTargetA already holds BrightPass's output
    for (UINT iteration = 0; iteration < BloomBlurIterations; ++iteration)
    {
        const UINT dstIndex = 1 - srcIndex;

        D3D12_RESOURCE_BARRIER dstToUav = {};
        dstToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        dstToUav.Transition.pResource = bloomTargets[dstIndex];
        dstToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        dstToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dstToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_postCommandList->ResourceBarrier(1, &dstToUav);

        m_postCommandList->SetComputeRootDescriptorTable(0, blurTables[srcIndex]);
        m_postCommandList->Dispatch(groupCountX, groupCountY, 1);

        D3D12_RESOURCE_BARRIER dstToSrv = {};
        dstToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        dstToSrv.Transition.pResource = bloomTargets[dstIndex];
        dstToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dstToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        dstToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_postCommandList->ResourceBarrier(1, &dstToSrv);

        srcIndex = dstIndex;
    }

    // Pass 6: Tonemap - add the scene (m_hdrResolvedTarget) and the final
    // bloom (m_bloomTargetA) together and Reinhard-tonemap the sum into
    // m_finalLdrTarget. Neither SRV input needs a barrier here: both are
    // already NON_PIXEL_SHADER_RESOURCE from the passes above.
    if (!m_isFirstFrame)
    {
        // m_finalLdrTarget is already UNORDERED_ACCESS on frame 1 (see
        // InitComputePostProcess) - every later frame left it as a copy
        // source after Pass 7 below.
        D3D12_RESOURCE_BARRIER finalToUav = {};
        finalToUav.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        finalToUav.Transition.pResource = m_finalLdrTarget.Get();
        finalToUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        finalToUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        finalToUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_postCommandList->ResourceBarrier(1, &finalToUav);
    }

    m_postCommandList->SetPipelineState(m_tonemapPipelineState.Get());
    m_postCommandList->SetComputeRootSignature(m_compositeRootSignature.Get());
    m_postCommandList->SetComputeRootDescriptorTable(0, tableAt(6)); // SRV scene, SRV bloom, UAV final
    m_postCommandList->Dispatch(groupCountX, groupCountY, 1);

    // Pass 7: copy the tonemapped result into the current back buffer - a
    // plain GPU-side copy, since the compute shader already wrote the
    // final pixels and no further blending or format conversion is needed.
    D3D12_RESOURCE_BARRIER preCopyBarriers[2] = {};
    preCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[0].Transition.pResource = m_finalLdrTarget.Get();
    preCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    preCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[1].Transition.pResource = m_renderTargets[m_frameIndex].Get();
    preCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    preCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    preCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_postCommandList->ResourceBarrier(_countof(preCopyBarriers), preCopyBarriers);

    m_postCommandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_finalLdrTarget.Get());

    D3D12_RESOURCE_BARRIER postCopyBarrier = {};
    postCopyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    postCopyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postCopyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postCopyBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_postCommandList->ResourceBarrier(1, &postCopyBarrier);

    ThrowIfFailed(m_postCommandList->Close());

    // Submit setup + every worker's cube draws + post-process, all in the
    // exact order the GPU must execute them in - a single ExecuteCommandLists
    // call runs its array in order, so this is what actually ties "recorded
    // in parallel" back together into one correct frame.
    std::array<ID3D12CommandList*, 2 + WorkerThreadCount> commandLists;
    commandLists[0] = m_commandList.Get();
    for (UINT threadIndex = 0; threadIndex < WorkerThreadCount; ++threadIndex)
    {
        commandLists[1 + threadIndex] = m_workerCommandLists[threadIndex].Get();
    }
    commandLists[1 + WorkerThreadCount] = m_postCommandList.Get();
    m_commandQueue->ExecuteCommandLists(static_cast<UINT>(commandLists.size()), commandLists.data());

    ThrowIfFailed(m_swapChain->Present(1, 0));

    m_isFirstFrame = false;
    WaitForPreviousFrame();
}

void App::RecordWorkerCommandList(UINT threadIndex)
{
    ID3D12GraphicsCommandList* commandList = m_workerCommandLists[threadIndex].Get();
    ThrowIfFailed(m_workerCommandAllocators[threadIndex]->Reset());
    ThrowIfFailed(commandList->Reset(m_workerCommandAllocators[threadIndex].Get(), nullptr));

    // Every one of these calls has to be repeated here even though
    // m_commandList already made the identical calls a moment ago - a
    // command list only ever knows about state recorded into itself, never
    // state from a different command list, even one submitted right before
    // it in the same ExecuteCommandLists call.
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->RSSetViewports(1, &m_viewport);
    commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_CPU_DESCRIPTOR_HANDLE msaaRtvHandle = m_msaaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    commandList->OMSetRenderTargets(1, &msaaRtvHandle, FALSE, &dsvHandle);

    commandList->SetPipelineState(m_pipelineState.Get());
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetGraphicsRootDescriptorTable(2, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    // Every cube uses the same material (diffuse@0, normalMap@2, shadowMap@3
    // - see BindlessMaterialIndices), so this is set once per worker rather
    // than once per cube. m_shadowMode is only ever written from the
    // message loop, which never overlaps Render(), so the workers can read
    // it without any synchronization.
    const BindlessMaterialIndices cubeIndices =
        { 0, 2, 3, m_shadowMode, ShadowMaskBindlessIndex, { 0, 0, 0 } };
    commandList->SetGraphicsRoot32BitConstants(1, sizeof(cubeIndices) / sizeof(UINT32), &cubeIndices, 0);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->IASetIndexBuffer(&m_indexBufferView);

    const UINT firstCube = threadIndex * CubesPerWorker;
    for (UINT cubeIndex = firstCube; cubeIndex < firstCube + CubesPerWorker; ++cubeIndex)
    {
        commandList->SetGraphicsRootConstantBufferView(0, CubeConstantBufferAddress(cubeIndex));
        commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
    }

    ThrowIfFailed(commandList->Close());
}

void App::WaitForPreviousFrame()
{
    const UINT64 fenceToWaitFor = m_fenceValue;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), fenceToWaitFor));
    ++m_fenceValue;

    if (m_fence->GetCompletedValue() < fenceToWaitFor)
    {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent));
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}
