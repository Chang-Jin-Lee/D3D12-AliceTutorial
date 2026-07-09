## 02. RenderingTriangle
- 더 쉬운 설명: [GUIDE.md](GUIDE.md) (고등학생도 이해할 수 있는 버전)
- 내용: 1단계의 파이프라인 위에 루트 시그니처/PSO/정점 버퍼를 추가해서, 정점마다 다른 색을 가진 삼각형 하나를 그립니다.
- 주요 구현:
  - 빈 `ID3D12RootSignature` 생성 (`D3D12SerializeRootSignature` + `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`)
  - `D3DCompileFromFile`로 `Shaders.hlsl`의 `VSMain`/`PSMain`을 런타임 컴파일
  - `POSITION`/`COLOR` Input Layout과 `ID3D12PipelineState` (그래픽스 PSO) 생성
  - Upload 힙에 정점 버퍼 생성 (`Map`/`memcpy`/`Unmap`) 및 `D3D12_VERTEX_BUFFER_VIEW` 구성
  - `RSSetViewports`/`RSSetScissorRects` 설정 후 `IASetVertexBuffers` + `DrawInstanced(3, 1, 0, 0)`
- 결과: CornflowerBlue 배경 위에 빨강/초록/파랑이 보간되는 삼각형이 그려짐

<p align="center">
  <img src="../../docs/images/02_RenderingTriangle.png" width="60%" />
</p>
