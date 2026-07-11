#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <stdexcept>

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw std::runtime_error("HRESULT failed");
    }
}

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT3 tangent;
    DirectX::XMFLOAT2 uv;
};

struct SceneConstantBuffer
{
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4 lightDirection;
    DirectX::XMFLOAT4 lightColor;
    DirectX::XMFLOAT4 ambientColor;
    DirectX::XMFLOAT4 eyePosition;
    DirectX::XMFLOAT4 specularColor; // .a = shininess
};

// Extends step 6 with normal mapping: a per-vertex tangent lets the pixel
// shader build a TBN basis and swap the flat per-face normal for one
// perturbed by a procedurally generated bump-map texture, so the same flat
// cube face now catches light unevenly instead of like one big flat mirror.
class App
{
public:
    App(HWND hwnd, UINT width, UINT height);
    ~App();

    void Update();
    void Render();

private:
    static const UINT FrameCount = 2;
    static const UINT TextureSize = 256;

    void InitDevice();
    void InitCommandQueue();
    void InitSwapChain();
    void InitRenderTargets();
    void InitDepthBuffer();
    void InitCommandList();
    void InitFence();
    void InitRootSignature();
    void InitPipelineState();
    void InitCubeGeometry();
    void InitConstantBuffer();
    void InitTextures();
    void WaitForPreviousFrame();

    HWND m_hwnd;
    UINT m_width;
    UINT m_height;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_renderTargets[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthBuffer;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    UINT m_indexCount = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    SceneConstantBuffer* m_mappedConstantBuffer = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_diffuseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalMapTexture;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissorRect = {};

    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_startTime = {};
};
