# 16단계 — DXR RayTracing (설계)

## 목표

`docs/superpowers/specs/2026-07-14-post-step09-roadmap-design.md`가 캡스톤으로 예고한 16단계를 구현한다.
15단계까지 쌓아온 라스터 파이프라인은 그대로 두고, 그 위에 **DXR 1.0 정석 파이프라인**(가속 구조 →
상태 객체 → 셰이더 테이블 → `DispatchRays`)으로 레이트레이싱 그림자를 추가한다. 9단계 셰도우맵과
런타임에 토글 비교할 수 있게 만들어, 두 기법의 차이를 같은 화면에서 직접 확인하는 것이 학습 목표다.

## 배경 / 결정 사항

- **범위는 그림자까지.** 레이트레이싱 반사는 13단계 PBR 머티리얼과 조명을 반사 광선 안에서 다시
  평가해야 해서 분량이 크게 늘어난다. 반사는 17단계 이후로 미룬다.
- **인라인 레이트레이싱(DXR 1.1 `RayQuery`)이 아니라 DXR 1.0 정석 파이프라인을 쓴다.** `RayQuery`는
  픽셀 셰이더 안에서 바로 광선을 쏠 수 있어 구현이 훨씬 짧지만, 상태 객체·셰이더 테이블이라는 DXR의
  핵심 아키텍처를 통째로 건너뛴다. 이 레포의 목적이 "D3D12 저수준 아키텍처 학습"이므로 정석을 택한다.
- **DXC를 도입한다.** DXR은 DXIL(SM 6.3+)이 필수라 1~15단계가 쓰던 FXC(`D3DCompileFromFile`,
  SM 5.x)로는 컴파일할 수 없다. Windows SDK 10.0.26100에 `dxcapi.h` / `dxcompiler.dll` /
  `dxil.dll`이 모두 들어 있어 "외부 의존성 없음" 원칙은 유지된다.
- **런타임 컴파일 관행을 유지한다.** dxc.exe 오프라인 컴파일 대신 `IDxcCompiler3`로 실행 중에
  컴파일해서, "HLSL 고치고 다시 실행하면 끝"이라는 기존 개발 흐름을 그대로 둔다. DXR 셰이더만
  DXC로 가고 나머지 vs/ps/cs는 FXC 그대로다.
- **15단계 구조를 그대로 상속한다.** MSAA, HDR 블룸/톤맵, 바인드리스, PBR, 워커 스레드 4개는 손대지
  않는다. 단계마다 이전 단계를 복사해 확장하는 이 레포의 관행을 따른다.

## 산출물

```
Dx12/16_DXRRayTracing/
  16_DXRRayTracing.vcxproj / .filters
  App.h / App.cpp              15단계 + DXR 초기화·프레임 로직
  RayTracing.hlsl        (신규) RayGen / ClosestHit / Miss / ShadowMiss
  Shaders.hlsl                 픽셀 셰이더에 그림자 소스 분기 추가
  ShadowShaders.hlsl / SkyboxShaders.hlsl / BlurCompute.hlsl
  BrightPass.hlsl / Tonemap.hlsl        15단계와 동일
  WinMain.cpp                  F키 입력 처리 추가
  README.md / GUIDE.md
docs/images/16_DXRRayTracing.png
docs/images/16_DXRRayTracing_Concepts.svg
README.md (루트)               바로가기 그리드에 16번 추가
Dx12/Dx12Tutorial.sln          16번 프로젝트 등록
```

## 아키텍처

### 프레임 순서

15단계의 "메인 리스트 + 워커 4개 + 후처리 리스트를 `ExecuteCommandLists` 한 번에 제출" 구조를 그대로
유지하고, 메인 리스트 앞쪽에 DXR 두 패스를 추가한다.

```
m_commandList (메인 스레드)
  1. TLAS 재빌드           [신규]  큐브 16개가 회전하므로 매 프레임
  2. DispatchRays          [신규]  1280x720 그림자 마스크 UAV 생성
  3. 배리어 UAV -> PIXEL_SHADER_RESOURCE  [신규]
  4. 셰도우맵 패스                  비교/토글용으로 계속 유지
  5. 색상 패스: 클리어 + 스카이박스 + 바닥
워커 리스트 x4                     큐브 16개 (15단계 그대로)
m_postCommandList                  리졸브 + 블룸 + 톤맵 (15단계 그대로)
```

DXR 패스를 색상 패스보다 **앞에** 두는 이유: 그림자 마스크를 깊이 버퍼에서 역투영해 만들면 "색상
패스가 마스크를 필요로 하는데 마스크는 색상 패스의 깊이를 필요로 하는" 순환이 생긴다. RayGen이
카메라 광선을 직접 쏘면 깊이 버퍼가 아예 필요 없어져 순환이 사라지고, 깊이 프리패스도 추가할 필요가
없다.

### 가속 구조

| | 개수 | 갱신 주기 | 빌드 플래그 |
|---|---|---|---|
| BLAS | 2개 — 큐브 메시, 바닥 평면 | 최초 1회 | `PREFER_FAST_TRACE` |
| TLAS | 1개 — 인스턴스 17개(큐브 16 + 바닥 1) | 매 프레임 재빌드 | `PREFER_FAST_TRACE` |

- 큐브 16개는 같은 메시라 BLAS 하나를 인스턴스 16개가 공유한다 — 가속 구조를 두 단계로 나눈 이유가
  그대로 드러나는 배치다.
- 인스턴스의 3x4 트랜스폼은 `Update()`가 큐브 월드 행렬을 만드는 바로 그 소스에서 채운다. 라스터와
  레이트레이싱이 같은 데이터를 본다는 것이 두 결과를 비교 가능하게 만드는 전제다.
- TLAS 갱신은 refit(`PERFORM_UPDATE`)이 아니라 매 프레임 전체 재빌드다. 인스턴스 17개 규모에서는
  차이가 없고, 코드가 짧아진다.
- 스카이박스는 가속 구조에 넣지 않는다 — 카메라 광선이 빗나가면 그것이 곧 "하늘"이다.

### 상태 객체와 셰이더 테이블

이 레포는 `d3dx12.h`를 쓰지 않으므로 `D3D12_STATE_SUBOBJECT` 배열을 직접 구성한다. 헬퍼가 감추는
구조가 다 보여서 오히려 교육적이다.

```
DXIL_LIBRARY  ->  HIT_GROUP x2  ->  SHADER_CONFIG
LOCAL_ROOT_SIGNATURE + SUBOBJECT_TO_EXPORTS_ASSOCIATION
GLOBAL_ROOT_SIGNATURE  ->  RAYTRACING_PIPELINE_CONFIG(MaxRecursionDepth = 1)
```

셰이더 테이블 레코드 (`ID3D12StateObjectProperties::GetShaderIdentifier` 32바이트 + 로컬 루트 인자):

| 영역 | 레코드 | 로컬 루트 인자 | stride |
|---|---|---|---|
| RayGen | 1 | 없음 | 32B -> 64B 정렬 |
| Miss | 2 (기본 / 그림자) | 없음 | 32B |
| HitGroup | 2 (큐브 / 바닥) | 정점 버퍼 SRV VA + 인덱스 버퍼 SRV VA | 48B -> 64B |

히트 그룹 두 레코드가 서로 다른 정점·인덱스 버퍼 주소를 들고 있어서 "셰이더 테이블에 왜 stride가
필요한가"가 억지 예제 없이 드러난다. TLAS 인스턴스의 `InstanceContributionToHitGroupIndex`로
큐브 인스턴스는 레코드 0, 바닥 인스턴스는 레코드 1을 고른다.

정렬 규칙 두 가지를 코드와 문서 양쪽에 남긴다:
`D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT`(32) — 레코드 stride,
`D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT`(64) — 각 테이블 시작 주소.

### 루트 시그니처

**글로벌** (모든 레이트레이싱 셰이더가 공유)

| 슬롯 | 종류 | 바인딩 | 내용 |
|---|---|---|---|
| 0 | 디스크립터 테이블 | u0 | 그림자 마스크 UAV |
| 1 | 루트 SRV | t0 | TLAS |
| 2 | 루트 CBV | b0 | 역 뷰프로젝션, 카메라 위치, 빛 방향 |

**로컬** (히트 그룹 전용)

| 슬롯 | 종류 | 바인딩 | 내용 |
|---|---|---|---|
| 0 | 루트 SRV | t1 | 정점 버퍼 (`StructuredBuffer<Vertex>`) |
| 1 | 루트 SRV | t2 | 인덱스 버퍼 (`ByteAddressBuffer`) |

`D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE` 플래그가 로컬 루트 시그니처를 구분한다.

### 광선 흐름 (재귀 없음)

```
RayGen  픽셀 -> 카메라 광선 TraceRay
        +- Miss        -> 하늘. 마스크 = 1.0 (그림자 없음)
        +- ClosestHit  -> RayTCurrent() + 보간 노멀을 페이로드에 기록
RayGen  히트 지점 + 노멀 오프셋에서 빛 방향으로 그림자 광선 TraceRay
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
        +- ShadowMiss  -> 가려지지 않음. 마스크 = 1.0
        +- (miss 안 함) -> 가려짐. 마스크 = 0.0
```

두 광선 모두 RayGen에서 쏘므로 `MaxRecursionDepth = 1`이면 충분하다. ClosestHit 안에서 그림자
광선을 재귀 호출하면 깊이 2가 필요해지고, 재귀 깊이는 드라이버 스택 예약량을 늘려 성능에 직접
영향을 준다 — GUIDE에서 설명한다.

인덱스 버퍼는 15단계와 같은 `R16_UINT`다. HLSL `ByteAddressBuffer`는 32비트 단위로만 읽을 수
있으므로, 삼각형 하나의 16비트 인덱스 3개를 얻으려면 4바이트 정렬 주소에서 dword 2개를 읽어
언팩하는 표준 관용구를 쓴다.

### 라스터 합성과 토글

- 그림자 마스크는 `R8_UNORM`, 백버퍼와 같은 1280x720 해상도.
- 마스크 SRV는 15단계 바인드리스 힙(`BindlessHeapCapacity = 16`, 현재 4칸 사용)의 슬롯 4에 넣는다.
  새 힙을 만들지 않고 14단계 바인드리스 구조를 그대로 재사용한다.
- `BindlessMaterialIndices` 루트 상수에 `shadowMaskIndex`와 `shadowMode`를 추가한다.
  픽셀 셰이더는 `shadowMode == 0`이면 기존 셰도우맵을 샘플하고, `1`이면 마스크를
  `SV_Position.xy`로 직접 `Load`한다.
- **F키**로 두 모드를 전환하고 창 제목에 현재 모드를 표시한다 (`WM_KEYDOWN` -> `App::OnKeyDown`).
- 셰도우맵 패스는 DXR 모드에서도 계속 실행한다. 토글이 즉시 반응해야 하고, 매 프레임 두 기법의
  비용을 나란히 두는 편이 비교 목적에 맞다.

**MSAA와의 절충**: 그림자 마스크는 픽셀당 1샘플이라 MSAA 서브샘플 4개가 같은 마스크 값을 공유한다.
실제 엔진의 스크린스페이스 그림자와 같은 절충이며, 큐브 실루엣 자체는 여전히 MSAA로
안티에일리어싱된다. GUIDE에 명시한다.

### DXC 통합

- `InitRaytracingPipeline`이 `DxcCreateInstance`로 `IDxcUtils` / `IDxcCompiler3`를 만들고
  `RayTracing.hlsl`을 `lib_6_3`으로 컴파일한다. 진입점(`-E`)은 지정하지 않는다 — 라이브러리
  타깃은 `[shader("...")]` 어트리뷰트가 붙은 모든 함수를 내보낸다.
- vcxproj가 Windows SDK bin에서 `dxcompiler.dll`과 `dxil.dll`을 **둘 다** 출력 폴더로 복사한다.
  `dxil.dll`이 없으면 DXC가 DXIL에 서명하지 못하고, 개발자 모드가 꺼진 환경에서는 D3D12가 서명
  없는 셰이더를 거부한다. 흔히 걸리는 함정이라 GUIDE에 기록한다.

## 컴포넌트 분해

`App`에 추가되는 함수는 각각 하나의 DXR 개념에 대응한다.

| 함수 | 책임 | 의존 |
|---|---|---|
| `InitRaytracingSupport()` | `ID3D12Device5` QI, `OPTIONS5.RaytracingTier` 확인 | 디바이스 |
| `InitRaytracingAccelerationStructures()` | BLAS 2개 빌드, TLAS/스크래치/인스턴스 버퍼 할당 | 정점·인덱스 버퍼 |
| `InitRaytracingRootSignatures()` | 글로벌 + 로컬 루트 시그니처 | 디바이스 |
| `InitRaytracingPipeline()` | DXC 컴파일 + 서브오브젝트 배열 + `CreateStateObject` | 루트 시그니처 |
| `InitRaytracingShaderTable()` | `GetShaderIdentifier` -> 정렬된 레코드 기록 | 상태 객체 |
| `InitRaytracingOutput()` | 그림자 마스크 텍스처 + UAV 힙 + 바인드리스 SRV | 바인드리스 힙 |
| `UpdateTopLevelAccelerationStructure(cmdList)` | 인스턴스 트랜스폼 갱신 + TLAS 빌드 + UAV 배리어 | TLAS 버퍼 |
| `RenderRaytracedShadows(cmdList)` | 글로벌 루트 인자 바인딩 + `SetPipelineState1` + `DispatchRays` | 상태 객체, 셰이더 테이블 |

각 함수는 `App`의 기존 `Init*` 관행과 같은 모양이고, 서로의 내부를 알 필요 없이 멤버 변수를 통해서만
연결된다.

## 에러 처리

- **DXR 미지원 GPU**: `RaytracingTier < D3D12_RAYTRACING_TIER_1_0`이면 "이 단계는 DXR Tier 1.0
  이상을 지원하는 GPU가 필요합니다"라는 메시지로 예외를 던진다. `WinMain`의 기존 try/catch가
  MessageBox로 띄우고 종료한다.
- **`ID3D12Device5` QI 실패**: 같은 경로로 "Windows 10 1809 이상이 필요합니다" 메시지.
- **DXC 로드 실패**: `dxcompiler.dll`을 찾지 못하면 출력 폴더 복사가 안 된 상황이므로, 그 사실을
  가리키는 메시지를 낸다.
- **DXC 컴파일 실패**: `IDxcResult`의 에러 문자열을 예외 메시지에 넣는다 — 기존 FXC 경로와 동일한 방식.

## 검증

- x64 Debug / Release 양쪽 빌드 성공
- 실행 후 F키로 셰도우맵 <-> DXR 전환이 즉시 반영되는지 확인
- D3D12 디버그 레이어 경고·에러 0건
- DXR 모드 스크린샷을 `docs/images/16_DXRRayTracing.png`로 저장

## 범위 제외

- **레이트레이싱 반사** — 반사 광선 안에서 PBR 조명을 다시 평가해야 해 분량이 크게 늘어난다. 17단계 이후 재검토.
- **인라인 레이트레이싱(`RayQuery`)** — 정석 파이프라인과 비교하는 별도 단계로 다루는 편이 낫다.
- **소프트 섀도우 / 디노이징** — 광선 여러 개와 시간적 누적이 필요해 별도 주제다.
- **AS refit, 컴팩션** — 인스턴스 17개 규모에서는 이득이 드러나지 않는다.

## 진행 방식

기존 관행대로 `16_DXRRayTracing` 브랜치에서 작업하고 PR을 생성한다. 커밋 접두사 `[feat]` / `[docs]`
관행을 유지하고, 루트 `README.md` 그리드와 단계별 `README.md` / `GUIDE.md`, `docs/images/`의
스크린샷·개념도(SVG)를 함께 추가한다.
