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
    DirectX::XMFLOAT4X4 lightMVP;
    DirectX::XMFLOAT4 lightDirection;
    DirectX::XMFLOAT4 lightColor;
    DirectX::XMFLOAT4 ambientColor;
    DirectX::XMFLOAT4 eyePosition;
    DirectX::XMFLOAT4 specularColor; // .a = shininess
};

struct SkyboxVertex
{
    DirectX::XMFLOAT3 position;
};

struct SkyboxConstantBuffer
{
    DirectX::XMFLOAT4X4 mvp;
    DirectX::XMFLOAT4 skyColor;
    DirectX::XMFLOAT4 horizonColor;
};

struct ShadowConstantBuffer
{
    DirectX::XMFLOAT4X4 mvp; // object world * light view * light projection
};

// Extends step 9 with multisample anti-aliasing (MSAA) on the final scene
// image: the skybox + cube + ground plane pass now renders into an
// off-screen multisampled color/depth target instead of the swapchain back
// buffer directly (a flip-model swapchain cannot itself be multisampled),
// and that target is resolved down into the back buffer right before
// Present. The shadow-map pass stays single-sample - it never touches the
// screen directly, so there is nothing to anti-alias there.
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
    static const UINT ShadowMapSize = 1024;

    void InitDevice();
    void InitCommandQueue();
    void InitSwapChain();
    void InitRenderTargets();
    void InitMsaaSupport();
    void InitMsaaRenderTarget();
    void InitDepthBuffer();
    void InitCommandList();
    void InitFence();
    void InitRootSignature();
    void InitPipelineState();
    void InitSceneGeometry();
    void InitConstantBuffer();
    void InitTextures();
    void InitSkybox();
    void InitShadowMap();
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

    // MSAA: the main pass draws into this off-screen multisampled color
    // target (and the multisampled m_depthBuffer above) instead of a
    // swapchain back buffer, then Render() resolves it down into whichever
    // back buffer is current.
    UINT m_msaaSampleCount = 4;
    UINT m_msaaQualityLevel = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_msaaRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_msaaColorTarget;

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;

    // Cube
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    UINT m_indexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    SceneConstantBuffer* m_mappedConstantBuffer = nullptr;

    // Ground plane
    Microsoft::WRL::ComPtr<ID3D12Resource> m_planeVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_planeVertexBufferView = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_planeIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_planeIndexBufferView = {};
    UINT m_planeIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_planeConstantBuffer;
    SceneConstantBuffer* m_mappedPlaneConstantBuffer = nullptr;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_diffuseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalMapTexture;

    // Skybox
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_skyboxRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_skyboxPipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_skyboxVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_skyboxVertexBufferView = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_skyboxIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_skyboxIndexBufferView = {};
    UINT m_skyboxIndexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_skyboxConstantBuffer;
    SkyboxConstantBuffer* m_mappedSkyboxConstantBuffer = nullptr;

    // Shadow pass
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_shadowRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_shadowPipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_shadowDsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowCubeConstantBuffer;
    ShadowConstantBuffer* m_mappedShadowCubeConstantBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowPlaneConstantBuffer;
    ShadowConstantBuffer* m_mappedShadowPlaneConstantBuffer = nullptr;
    D3D12_VIEWPORT m_shadowViewport = {};
    D3D12_RECT m_shadowScissorRect = {};
    bool m_isFirstFrame = true;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissorRect = {};

    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_startTime = {};
};
