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
    DirectX::XMFLOAT4 materialParams; // .x = metallic, .y = roughness
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

// Root constants (b1) for the main pass's bindless texture indices - which
// slots of the one big m_srvHeap array (see BindlessHeapCapacity) this
// draw call's pixel shader should read through. Only diffuseTextureIndex
// actually differs between the cube and the plane in this scene; the other
// two are here so a real multi-material scene could vary them too.
struct BindlessMaterialIndices
{
    UINT diffuseTextureIndex;
    UINT normalMapIndex;
    UINT shadowMapIndex;
    UINT padding;
};

// Extends step 13 by replacing the main pass's fixed 3-descriptor SRV table
// (steps 4-13: always diffuse@t0, normal@t1, shadow@t2) with a "bindless"
// pattern: one large descriptor table (see BindlessHeapCapacity) bound once,
// and a small per-draw root constant telling the shader which index in that
// table to read through (see BindlessMaterialIndices). A second diffuse
// texture is added purely to make that index matter - the cube and the
// ground plane now index two different textures out of the same bound
// table, instead of the CPU rebinding a different descriptor table per
// draw the way steps 4-13 implicitly did. Root signature version 1.0 with
// a fixed-size (not unbounded) range keeps this compatible with Resource
// Binding Tier 2 hardware - see InitTextures for why. HDR, bloom, tonemap,
// shadow map, skybox, and the PBR lighting model are unchanged from step 13.
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
    // Size of the bindless SRV table in InitTextures/InitRootSignature.
    // Only 4 slots are ever populated (2 diffuse textures + normal map +
    // shadow map), but a real bindless heap is deliberately sized well
    // beyond what's in use on day one, so it's declared generously here
    // too rather than as an exact-fit table of 4.
    static const UINT BindlessHeapCapacity = 16;
    // Must be even - the bloom ping-pong loop in Render() alternates
    // between m_bloomTargetA/B, and the composite pass always reads the
    // result back out of m_bloomTargetA (see InitComputePostProcess). A
    // single 3x3 pass barely spreads a highlight a few pixels; repeating
    // it widens the effective blur radius into a visible glow without
    // needing a wider (and more complex) blur kernel.
    static const UINT BloomBlurIterations = 16;

    void InitDevice();
    void InitCommandQueue();
    void InitSwapChain();
    void InitRenderTargets();
    void InitMsaaSupport();
    void InitMsaaRenderTarget();
    void InitDepthBuffer();
    void InitComputePostProcess();
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
    // swapchain back buffer, then Render() resolves it down before the
    // post-process chain below runs. Unlike steps 1-11, this target is HDR
    // (see HdrColorFormat) so a bright specular highlight can exceed 1.0
    // instead of clamping to white.
    static constexpr DXGI_FORMAT HdrColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    UINT m_msaaSampleCount = 4;
    UINT m_msaaQualityLevel = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_msaaRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_msaaColorTarget;

    // HDR bloom + tonemap post-process. Frame order (see Render()):
    //   1. m_msaaColorTarget resolves into m_hdrResolvedTarget (HDR, SRV).
    //   2. BrightPass.hlsl keeps only over-bright pixels, into
    //      m_bloomTargetA (UAV).
    //   3. BlurCompute.hlsl (same shader as step 11) runs
    //      BloomBlurIterations times, ping-ponging between
    //      m_bloomTargetA/B, spreading the bright pixels into a halo.
    //   4. Tonemap.hlsl adds m_hdrResolvedTarget + the final bloom
    //      (back in m_bloomTargetA - see BloomBlurIterations) and
    //      Reinhard-tonemaps the sum into m_finalLdrTarget (LDR, UAV).
    //   5. m_finalLdrTarget is copied into the current back buffer.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hdrResolvedTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bloomTargetA;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bloomTargetB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_finalLdrTarget;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_computeHeap;
    // Shared by BrightPass and Blur - both take one SRV(t0) + UAV(u0) table.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blurRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_brightPassPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blurPipelineState;
    // Tonemap takes two SRVs (scene, bloom) + one UAV (final), so it needs
    // its own root signature layout.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_compositeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_tonemapPipelineState;

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

    // The bindless SRV table: index 0 = cube's diffuse texture, index 1 =
    // plane's diffuse texture, index 2 = normal map, index 3 = shadow map.
    // Bound once per frame (see Render()); which slots a given draw call
    // actually reads come from that draw's BindlessMaterialIndices root
    // constants instead of from a different descriptor table per object.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_diffuseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_diffuseTexture2;
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
    // Guards the pre-pass transitions for every resource whose frame-1
    // state doesn't match its steady-state end-of-frame state (shadow map,
    // m_hdrResolvedTarget, m_finalLdrTarget). Each is created already in
    // the state its *first* use needs, so frame 1 must skip the
    // "transition back from last frame's end state" barrier that every
    // later frame performs. m_bloomTargetA/B don't need this: they're
    // created in the same state (NON_PIXEL_SHADER_RESOURCE) they always
    // end a frame in, so their transitions never special-case frame 1.
    bool m_isFirstFrame = true;

    D3D12_VIEWPORT m_viewport = {};
    D3D12_RECT m_scissorRect = {};

    LARGE_INTEGER m_perfFrequency = {};
    LARGE_INTEGER m_startTime = {};
};
