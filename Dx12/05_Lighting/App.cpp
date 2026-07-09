#include "App.h"

#include <d3dcompiler.h>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
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
    InitTexture();
}

App::~App()
{
    WaitForPreviousFrame();
    if (m_constantBuffer)
    {
        m_constantBuffer->Unmap(0, nullptr);
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
    srvRange.NumDescriptors = 1;
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
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
    // 24 vertices (4 per face), each face carrying its own flat normal and
    // 0-1 UV range, wound clockwise as seen from outside for back-face culling.
    const Vertex vertices[] =
    {
        // Front (z = -1), normal (0,0,-1)
        { { -1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
        // Back (z = +1), normal (0,0,1)
        { {  1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { { -1.0f,  1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f,  1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        // Left (x = -1), normal (-1,0,0)
        { { -1.0f, -1.0f,  1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f,  1.0f }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { -1.0f,  1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        // Right (x = +1), normal (1,0,0)
        { {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        // Top (y = +1), normal (0,1,0)
        { { -1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
        // Bottom (y = -1), normal (0,-1,0)
        { { -1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f } },
        { {  1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
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

void App::InitTexture()
{
    // Procedurally generated 8x8 checkerboard - no image file / WIC
    // dependency needed for this step.
    std::vector<UINT32> pixels(static_cast<size_t>(TextureSize) * TextureSize);
    const UINT cellSize = TextureSize / 8;
    for (UINT y = 0; y < TextureSize; ++y)
    {
        for (UINT x = 0; x < TextureSize; ++x)
        {
            const bool isWhite = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            pixels[static_cast<size_t>(y) * TextureSize + x] = isWhite ? 0xFFFFFFFFu : 0xFF3050A0u; // 0xAABBGGRR
        }
    }

    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = TextureSize;
    textureDesc.Height = TextureSize;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;

    ThrowIfFailed(m_device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_texture)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 uploadBufferSize = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);

    ComPtr<ID3D12Resource> uploadBuffer = CreateUploadBuffer(m_device.Get(), nullptr, static_cast<UINT>(uploadBufferSize));

    UINT8* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    ThrowIfFailed(uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData)));
    const UINT8* srcData = reinterpret_cast<const UINT8*>(pixels.data());
    for (UINT row = 0; row < numRows; ++row)
    {
        memcpy(mappedData + footprint.Footprint.RowPitch * row,
            srcData + static_cast<size_t>(TextureSize) * 4 * row,
            static_cast<size_t>(rowSizeInBytes));
    }
    uploadBuffer->Unmap(0, nullptr);

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), nullptr));

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = m_texture.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = uploadBuffer.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    m_commandList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    D3D12_RESOURCE_BARRIER toShaderResource = {};
    toShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShaderResource.Transition.pResource = m_texture.Get();
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toShaderResource);

    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);
    WaitForPreviousFrame();

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_srvHeap)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_texture.Get(), &srvDesc, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
}

void App::Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const float elapsedSeconds =
        static_cast<float>(now.QuadPart - m_startTime.QuadPart) / static_cast<float>(m_perfFrequency.QuadPart);

    const XMMATRIX world = XMMatrixRotationRollPitchYaw(elapsedSeconds * 0.7f, elapsedSeconds, 0.0f);
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0.0f, 2.0f, -5.0f, 0.0f), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
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
}

void App::Render()
{
    Update();

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));

    m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    m_commandList->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());

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

    const float clearColor[] = { 0.392f, 0.584f, 0.929f, 1.0f }; // CornflowerBlue
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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
