#include "App.h"

#include <d3dcompiler.h>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
    const XMVECTOR kEyePosition = XMVectorSet(0.0f, 2.0f, -5.0f, 0.0f);
    const float kSkyboxScale = 50.0f;
    const XMVECTOR kLightDirection = []
    {
        return XMVector3Normalize(XMVectorSet(0.5f, -1.0f, 0.3f, 0.0f));
    }();

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
    InitConstantBuffer();
    InitTextures();
    InitSkybox();
}

App::~App()
{
    WaitForPreviousFrame();
    if (m_constantBuffer) m_constantBuffer->Unmap(0, nullptr);
    if (m_planeConstantBuffer) m_planeConstantBuffer->Unmap(0, nullptr);
    if (m_skyboxConstantBuffer) m_skyboxConstantBuffer->Unmap(0, nullptr);
    if (m_shadowCubeConstantBuffer) m_shadowCubeConstantBuffer->Unmap(0, nullptr);
    if (m_shadowPlaneConstantBuffer) m_shadowPlaneConstantBuffer->Unmap(0, nullptr);
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

    m_shadowCubeConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(ShadowConstantBuffer), reinterpret_cast<void**>(&m_mappedShadowCubeConstantBuffer));
    m_shadowPlaneConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(ShadowConstantBuffer), reinterpret_cast<void**>(&m_mappedShadowPlaneConstantBuffer));
}

void App::InitRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3; // t0 = diffuse, t1 = normal map, t2 = shadow map
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].Descriptor.ShaderRegister = 0;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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

    if (FAILED(D3DCompileFromFile(
        L"Shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &error)))
    {
        throw std::runtime_error(error ? static_cast<const char*>(error->GetBufferPointer()) : "VSMain compile failed");
    }
    if (FAILED(D3DCompileFromFile(
        L"Shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &error)))
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

void App::InitConstantBuffer()
{
    m_constantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(SceneConstantBuffer), reinterpret_cast<void**>(&m_mappedConstantBuffer));
    m_planeConstantBuffer = CreateConstantBuffer(
        m_device.Get(), sizeof(SceneConstantBuffer), reinterpret_cast<void**>(&m_mappedPlaneConstantBuffer));
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
    std::vector<UINT32> diffusePixels(static_cast<size_t>(TextureSize) * TextureSize);
    const UINT cellSize = TextureSize / 8;
    for (UINT y = 0; y < TextureSize; ++y)
    {
        for (UINT x = 0; x < TextureSize; ++x)
        {
            const bool isWhite = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            diffusePixels[static_cast<size_t>(y) * TextureSize + x] = isWhite ? 0xFFFFFFFFu : 0xFF3050A0u; // 0xAABBGGRR
        }
    }

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
    ComPtr<ID3D12Resource> normalUploadBuffer;
    m_diffuseTexture = UploadTexture2D(
        m_device.Get(), m_commandList.Get(), TextureSize, TextureSize, diffusePixels, diffuseUploadBuffer);
    m_normalMapTexture = UploadTexture2D(
        m_device.Get(), m_commandList.Get(), TextureSize, TextureSize, normalPixels, normalUploadBuffer);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForPreviousFrame();

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 3;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    const UINT srvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateShaderResourceView(m_diffuseTexture.Get(), &srvDesc, srvHandle);
    srvHandle.ptr += srvDescriptorSize;
    m_device->CreateShaderResourceView(m_normalMapTexture.Get(), &srvDesc, srvHandle);
    srvHandle.ptr += srvDescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
    shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadowSrvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_shadowMap.Get(), &shadowSrvDesc, srvHandle);
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

    const XMMATRIX cubeWorld = XMMatrixRotationRollPitchYaw(elapsedSeconds * 0.7f, elapsedSeconds, 0.0f);
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

    auto fillObjectConstants = [&](SceneConstantBuffer* cb, const XMMATRIX& world)
    {
        XMStoreFloat4x4(&cb->mvp, XMMatrixTranspose(world * view * projection));
        XMStoreFloat4x4(&cb->world, XMMatrixTranspose(world));
        XMStoreFloat4x4(&cb->lightMVP, XMMatrixTranspose(world * lightViewProjection));
        XMStoreFloat4(&cb->lightDirection, kLightDirection);
        // Boosted well past step 1-11's {1,1,1,1}/{1,1,1,8}: on a UNORM
        // target these would have clipped to plain white, but the HDR
        // target here keeps the true values so BrightPass.hlsl has
        // genuinely over-bright pixels to extract into the bloom halo. A
        // tighter, more intense specular term (higher shininess exponent,
        // higher RGB scale) gives a small hot highlight rather than
        // blowing out the whole lit face.
        cb->lightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        cb->ambientColor = { 0.15f, 0.15f, 0.15f, 1.0f };
        XMStoreFloat4(&cb->eyePosition, kEyePosition);
        cb->specularColor = { 10.0f, 10.0f, 8.0f, 32.0f }; // .a = shininess
    };
    fillObjectConstants(m_mappedConstantBuffer, cubeWorld);
    fillObjectConstants(m_mappedPlaneConstantBuffer, planeWorld);

    XMStoreFloat4x4(&m_mappedShadowCubeConstantBuffer->mvp, XMMatrixTranspose(cubeWorld * lightViewProjection));
    XMStoreFloat4x4(&m_mappedShadowPlaneConstantBuffer->mvp, XMMatrixTranspose(planeWorld * lightViewProjection));

    const XMMATRIX skyboxWorld = XMMatrixScaling(kSkyboxScale, kSkyboxScale, kSkyboxScale);
    XMStoreFloat4x4(&m_mappedSkyboxConstantBuffer->mvp, XMMatrixTranspose(skyboxWorld * view * projection));
    m_mappedSkyboxConstantBuffer->skyColor = { 0.35f, 0.55f, 0.9f, 1.0f };
    m_mappedSkyboxConstantBuffer->horizonColor = { 0.85f, 0.9f, 0.95f, 1.0f };
}

void App::Render()
{
    Update();

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

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

    m_commandList->SetGraphicsRootConstantBufferView(0, m_shadowCubeConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);
    m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

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

    // Pass 2: the normal color pass - skybox, then cube and plane lit with
    // the shadow map sampled in. This renders into the off-screen
    // multisampled color/depth target (m_msaaColorTarget/m_depthBuffer)
    // rather than the back buffer directly - see InitMsaaRenderTarget for
    // why a flip-model swapchain can't be the multisample target itself.
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
    m_commandList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);
    m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    m_commandList->SetGraphicsRootConstantBufferView(0, m_planeConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_planeVertexBufferView);
    m_commandList->IASetIndexBuffer(&m_planeIndexBufferView);
    m_commandList->DrawIndexedInstanced(m_planeIndexCount, 1, 0, 0, 0);

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
    m_commandList->ResourceBarrier(preResolveBarrierCount, preResolveBarriers);

    m_commandList->ResolveSubresource(m_hdrResolvedTarget.Get(), 0, m_msaaColorTarget.Get(), 0, HdrColorFormat);

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
    m_commandList->ResourceBarrier(_countof(postResolveBarriers), postResolveBarriers);

    // Pass 4: BrightPass - keep only the over-bright pixels (the boosted
    // specular highlight from Update()), writing them into m_bloomTargetA.
    ID3D12DescriptorHeap* computeHeaps[] = { m_computeHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(computeHeaps), computeHeaps);
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
    m_commandList->ResourceBarrier(1, &bloomAToUav);

    m_commandList->SetPipelineState(m_brightPassPipelineState.Get());
    m_commandList->SetComputeRootSignature(m_blurRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(0, tableAt(0)); // SRV hdrResolved, UAV bloomA
    m_commandList->Dispatch(groupCountX, groupCountY, 1);

    D3D12_RESOURCE_BARRIER bloomAToSrv = {};
    bloomAToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bloomAToSrv.Transition.pResource = m_bloomTargetA.Get();
    bloomAToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bloomAToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bloomAToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &bloomAToSrv);

    // Pass 5: blur the bright pixels into a soft halo, ping-ponging between
    // m_bloomTargetA/B so each iteration reads the previous one's result.
    // BloomBlurIterations must stay even: the Tonemap pass below always
    // reads its bloom input from m_bloomTargetA (see InitComputePostProcess's
    // descriptor [7]), which only holds the final blurred result if the
    // ping-pong loop runs an even number of times.
    ID3D12Resource* bloomTargets[2] = { m_bloomTargetA.Get(), m_bloomTargetB.Get() };
    const D3D12_GPU_DESCRIPTOR_HANDLE blurTables[2] = { tableAt(2), tableAt(4) }; // [A->B, B->A]
    m_commandList->SetPipelineState(m_blurPipelineState.Get());
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
        m_commandList->ResourceBarrier(1, &dstToUav);

        m_commandList->SetComputeRootDescriptorTable(0, blurTables[srcIndex]);
        m_commandList->Dispatch(groupCountX, groupCountY, 1);

        D3D12_RESOURCE_BARRIER dstToSrv = {};
        dstToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        dstToSrv.Transition.pResource = bloomTargets[dstIndex];
        dstToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        dstToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        dstToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &dstToSrv);

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
        m_commandList->ResourceBarrier(1, &finalToUav);
    }

    m_commandList->SetPipelineState(m_tonemapPipelineState.Get());
    m_commandList->SetComputeRootSignature(m_compositeRootSignature.Get());
    m_commandList->SetComputeRootDescriptorTable(0, tableAt(6)); // SRV scene, SRV bloom, UAV final
    m_commandList->Dispatch(groupCountX, groupCountY, 1);

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
    m_commandList->ResourceBarrier(_countof(preCopyBarriers), preCopyBarriers);

    m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), m_finalLdrTarget.Get());

    D3D12_RESOURCE_BARRIER postCopyBarrier = {};
    postCopyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    postCopyBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postCopyBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postCopyBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &postCopyBarrier);

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    ThrowIfFailed(m_swapChain->Present(1, 0));

    m_isFirstFrame = false;
    WaitForPreviousFrame();
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
