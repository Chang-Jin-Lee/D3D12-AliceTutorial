#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdexcept>

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw std::runtime_error("HRESULT failed");
    }
}

// Owns the full D3D12 pipeline (device, command queue, swap chain, RTVs,
// command list, fence) and clears the back buffer to a solid color every
// frame. No geometry/shaders yet - that starts in step 2.
class App
{
public:
    App(HWND hwnd, UINT width, UINT height);
    ~App();

    void Render();

private:
    static const UINT FrameCount = 2;

    void InitDevice();
    void InitCommandQueue();
    void InitSwapChain();
    void InitRenderTargets();
    void InitCommandList();
    void InitFence();
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

    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
};
