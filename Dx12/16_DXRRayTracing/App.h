#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
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

// Extends step 14 by replacing the single rotating cube with a
// CubeGridDim x CubeGridDim grid of cubes (CubeCount total) and splitting
// that grid's main-pass draw calls across WorkerThreadCount worker threads,
// each recording its own share of the cubes into its own
// ID3D12GraphicsCommandList. D3D12 command lists don't inherit pipeline
// state from each other even when submitted together, so every worker list
// re-sets its own render target/viewport/root signature/PSO/heap before
// drawing its cubes (see RecordWorkerCommandList) - the same state the
// single-threaded m_commandList used to set once. The main thread still
// records the shadow pass, skybox, and ground plane in m_commandList, and a
// separate m_postCommandList still handles the MSAA resolve + bloom +
// tonemap chain from step 12 unchanged; all of them, plus every worker
// list, are submitted together in one ExecuteCommandLists call so the GPU
// runs them in that exact order. Bindless textures (step 14), PBR
// materials (step 13), HDR bloom/tonemap (step 12), shadow map, skybox are
// otherwise unchanged.
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

    // The single step-14 cube is replaced by a CubeGridDim x CubeGridDim
    // grid (CubeCount total) so there's enough per-object work to actually
    // be worth splitting across threads - recording one or two draw calls
    // in parallel would be dominated by thread launch overhead.
    static const UINT CubeGridDim = 4;
    static const UINT CubeCount = CubeGridDim * CubeGridDim;
    // CubeCount must be an exact multiple of this - see RecordWorkerCommandList,
    // which gives every worker the same fixed-size contiguous slice of cubes.
    static const UINT WorkerThreadCount = 4;
    static const UINT CubesPerWorker = CubeCount / WorkerThreadCount;

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
    void RecordWorkerCommandList(UINT threadIndex);
    D3D12_GPU_VIRTUAL_ADDRESS CubeConstantBufferAddress(UINT cubeIndex) const;
    D3D12_GPU_VIRTUAL_ADDRESS ShadowCubeConstantBufferAddress(UINT cubeIndex) const;
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
    // One allocator/list pair per worker thread, created once in
    // InitCommandList and reused every frame (safe because Render() always
    // fully waits on the fence in WaitForPreviousFrame before returning -
    // there's never more than one frame's worth of GPU work in flight, the
    // same assumption the single m_commandAllocator above already relied
    // on in steps 1-14). Recorded in parallel by RecordWorkerCommandList,
    // one std::thread per entry, then joined before ExecuteCommandLists.
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, WorkerThreadCount> m_workerCommandAllocators;
    std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>, WorkerThreadCount> m_workerCommandLists;
    // Everything after the cube grid - MSAA resolve, bloom, tonemap, the
    // copy into the back buffer - moves here out of m_commandList so the
    // worker lists above can be submitted between "set up the color pass"
    // (m_commandList) and "resolve/post-process it" (this list) in a
    // single ExecuteCommandLists call.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_postCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_postCommandList;

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

    // Cube grid: one shared vertex/index buffer (every cube is the same
    // mesh), but each of the CubeCount cubes needs its own world matrix,
    // so their constant buffers live in one CubeCount-element upload
    // buffer instead of step 14's single m_constantBuffer. Each element is
    // 256-byte aligned (CBV alignment rule) - see CubeConstantBufferAddress.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    UINT m_indexCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeConstantBuffers;
    uint8_t* m_mappedCubeConstantBuffers = nullptr;
    // World-space grid position of each cube, computed once in
    // InitSceneGeometry; Update() reads this every frame to place and spin
    // each cube without recomputing the grid layout itself.
    std::array<DirectX::XMFLOAT3, CubeCount> m_cubeGridPositions;

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
    // One shadow-pass constant buffer per cube, same CubeCount-element
    // layout as m_cubeConstantBuffers above.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowCubeConstantBuffers;
    uint8_t* m_mappedShadowCubeConstantBuffers = nullptr;
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
