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
    const float kSkyboxScale = 50.0f; // must be well outside the far clip's near geometry

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
}

App::App(HWND hwnd, UINT width, UINT height)
    : m_hwnd(hwnd)
    , m_width(width)
    , m_height(height)
{
    m_viewport = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    m_scissorRect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };

    QueryPerformanceFrequency(&m_perfFrequency);
    QueryPerformanceCounter(&m_startTime);

    InitDevice();
    InitCommandQueue();
    InitSwapChain();
    InitRenderTargets();
    InitDepthBuffer();
    InitCommandList();
    InitFence();
    InitRootSignature();
    InitPipelineState();
    InitCubeGeometry();
    InitConstantBuffer();
    InitTextures();
    InitSkybox();
}

App::~App()
{
    WaitForPreviousFrame();
    if (m_constantBuffer)
    {
        m_constantBuffer->Unmap(0, nullptr);
    }
    if (m_skyboxConstantBuffer)
    {
        m_skyboxConstantBuffer->Unmap(0, nullptr);
    }
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

void App::InitDepthBuffer()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClearValue = {};
    depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    depthClearValue.DepthStencil.Depth = 1.0f;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClearValue, IID_PPV_ARGS(&m_depthBuffer)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
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

void App::InitRootSignature()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 2; // t0 = diffuse, t1 = normal map
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
}

void App::InitCubeGeometry()
{
    // 24 vertices (4 per face): flat per-face normal, a tangent pointing
    // along each face's +U (UV) direction, and a 0-1 UV range. Wound
    // clockwise as seen from outside for back-face culling.
    const Vertex vertices[] =
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
    const UINT vertexBufferSize = sizeof(vertices);

    std::vector<UINT16> indices;
    indices.reserve(36);
    for (UINT16 face = 0; face < 6; ++face)
    {
        const UINT16 base = face * 4;
        const UINT16 faceIndices[] = { base, static_cast<UINT16>(base + 1), static_cast<UINT16>(base + 2),
                                        base, static_cast<UINT16>(base + 2), static_cast<UINT16>(base + 3) };
        indices.insert(indices.end(), std::begin(faceIndices), std::end(faceIndices));
    }
    const UINT indexBufferSize = static_cast<UINT>(indices.size() * sizeof(UINT16));
    m_indexCount = static_cast<UINT>(indices.size());

    m_vertexBuffer = CreateUploadBuffer(m_device.Get(), vertices, vertexBufferSize);
    m_vertexBufferView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vertexBufferView.StrideInBytes = sizeof(Vertex);
    m_vertexBufferView.SizeInBytes = vertexBufferSize;

    m_indexBuffer = CreateUploadBuffer(m_device.Get(), indices.data(), indexBufferSize);
    m_indexBufferView.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_indexBufferView.Format = DXGI_FORMAT_R16_UINT;
    m_indexBufferView.SizeInBytes = indexBufferSize;
}

void App::InitConstantBuffer()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = (sizeof(SceneConstantBuffer) + 255) & ~255; // CBV size must be 256-byte aligned
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constantBuffer)));

    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedConstantBuffer)));
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
    // Diffuse: the same procedurally generated 8x8 checkerboard used since
    // step 4 - no image file / WIC dependency needed.
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

    // Normal map: a procedural height field of rounded bumps, one per 8x8
    // cell, converted to tangent-space normals via a central difference.
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
    srvHeapDesc.NumDescriptors = 2;
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
}

void App::InitSkybox()
{
    // Root signature: just one CBV (b0), nothing else - the sky color comes
    // from plain math in the pixel shader, no textures involved.
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
    // No culling - the camera always sits inside this cube, so only the
    // faces winding "the wrong way" (from an outside perspective) are ever
    // in view. Disabling culling sidesteps having to think about that.
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // No depth test/write: the skybox is drawn first and should never hide
    // (or be affected by) real geometry drawn afterwards.
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

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

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = (sizeof(SkyboxConstantBuffer) + 255) & ~255;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_skyboxConstantBuffer)));

    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(m_skyboxConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedSkyboxConstantBuffer)));
}

void App::Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float elapsedSeconds =
        static_cast<float>(now.QuadPart - m_startTime.QuadPart) / static_cast<float>(m_perfFrequency.QuadPart);

    const XMMATRIX world = XMMatrixRotationRollPitchYaw(elapsedSeconds * 0.7f, elapsedSeconds, 0.0f);
    const XMMATRIX view = XMMatrixLookAtLH(kEyePosition, XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, static_cast<float>(m_width) / static_cast<float>(m_height), 0.1f, 100.0f);

    XMStoreFloat4x4(&m_mappedConstantBuffer->mvp, XMMatrixTranspose(world * view * projection));
    XMStoreFloat4x4(&m_mappedConstantBuffer->world, XMMatrixTranspose(world));

    // Fixed world-space light - it doesn't rotate with the cube, so the lit
    // side visibly changes as the cube spins.
    const XMVECTOR lightDir = XMVector3Normalize(XMVectorSet(0.5f, -1.0f, 0.3f, 0.0f));
    XMStoreFloat4(&m_mappedConstantBuffer->lightDirection, lightDir);
    m_mappedConstantBuffer->lightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_mappedConstantBuffer->ambientColor = { 0.15f, 0.15f, 0.15f, 1.0f };

    XMStoreFloat4(&m_mappedConstantBuffer->eyePosition, kEyePosition);
    // Low shininess (see step 6) - the highlight needs to stay broad enough
    // to be visible on flat-shaded geometry regardless of rotation angle.
    m_mappedConstantBuffer->specularColor = { 1.0f, 1.0f, 1.0f, 8.0f }; // .a = shininess

    // Skybox: static, uniformly scaled cube centered on the origin. Since
    // the camera never moves in this tutorial, it always ends up inside it.
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

    m_commandList->RSSetViewports(1, &m_viewport);
    m_commandList->RSSetScissorRects(1, &m_scissorRect);

    D3D12_RESOURCE_BARRIER toRenderTarget = {};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Pass 1: skybox, filling every pixel behind the real geometry.
    m_commandList->SetPipelineState(m_skyboxPipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_skyboxRootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_skyboxConstantBuffer->GetGPUVirtualAddress());
    m_commandList->IASetVertexBuffers(0, 1, &m_skyboxVertexBufferView);
    m_commandList->IASetIndexBuffer(&m_skyboxIndexBufferView);
    m_commandList->DrawIndexedInstanced(m_skyboxIndexCount, 1, 0, 0, 0);

    // Pass 2: the normal-mapped lit cube from step 7, drawn on top.
    m_commandList->SetPipelineState(m_pipelineState.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    m_commandList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    m_commandList->IASetIndexBuffer(&m_indexBufferView);
    m_commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(m_commandList->Close());

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    ThrowIfFailed(m_swapChain->Present(1, 0));

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
