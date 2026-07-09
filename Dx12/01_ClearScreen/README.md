## 01. ClearScreen
- 더 쉬운 설명: [GUIDE.md](GUIDE.md)
- 내용: D3D12 파이프라인의 기본 골격(디바이스~펜스 동기화)만 구성해서, 백버퍼를 매 프레임 단색으로 지웁니다.
- 주요 구현:
  - `D3D12CreateDevice` / `CreateDXGIFactory2`로 디바이스 및 DXGI 팩토리 생성
  - `ID3D12CommandQueue` 생성
  - `IDXGISwapChain3` 생성 (더블 버퍼링, `DXGI_SWAP_EFFECT_FLIP_DISCARD`)
  - RTV `ID3D12DescriptorHeap` 생성 및 백버퍼별 RTV 구성
  - `ID3D12CommandAllocator` / `ID3D12GraphicsCommandList` 생성
  - 리소스 배리어(`PRESENT` ↔ `RENDER_TARGET`)와 `ClearRenderTargetView`
  - `ID3D12Fence` 기반 프레임 동기화 (`WaitForPreviousFrame`)
  - Debug 빌드에서 `ID3D12Debug` 디버그 레이어 활성화
- 결과: 지오메트리/셰이더 없이, 창 전체가 CornflowerBlue로 지속적으로 지워짐 (파이프라인이 정상 동작함을 확인)

<p align="center">
  <img src="../../docs/images/01_ClearScreen.png" width="60%" />
</p>
