# D3D12-AliceTutorial — 01. ClearScreen 설계 문서

## 목표
D3D11-AliceTutorial과 같은 스타일(단계별 프로젝트 진행, 잘 꾸며진 README)로 D3D12 학습 튜토리얼을 시작한다. 이 문서는 가장 첫 번째, 가장 쉬운 단계인 `01_ClearScreen`의 설계를 다룬다.

## 배경 / 참고
- 참고 프로젝트: `C:\ChangJinGithub\D3D11-AliceTutorial` (단계별 vcxproj + 루트 README 그리드 + 단계별 README), `C:\ChangJinGithub\dx11-math-shader` (로컬 이미지 폴더 방식 README)
- D3D12는 D3D11 대비 초기화 보일러플레이트(디바이스/커맨드큐/디스크립터힙/펜스 동기화)가 훨씬 많으므로, D3D11의 1단계(사각형 렌더링)보다 한 단계 더 쪼개어 "화면 지우기까지만"을 1단계로 잡는다. 삼각형/사각형 렌더링은 2단계로 넘긴다.

## 리포 구조
```
D3D12-AliceTutorial/
├── README.md
├── LICENSE                        (MIT)
├── .gitignore
├── docs/
│   ├── superpowers/specs/...      (이 문서)
│   └── images/
│       └── 01_ClearScreen.png     (실행 스크린샷)
└── Dx12/
    ├── Dx12Tutorial.sln           (x64 Debug/Release)
    └── 01_ClearScreen/
        ├── 01_ClearScreen.vcxproj
        ├── 01_ClearScreen.vcxproj.filters
        ├── WinMain.cpp
        ├── App.h
        ├── App.cpp
        └── README.md
```

## 1단계 기술 범위
- Win32 창 생성: 1280×720, 타이틀 "D3D12 Tutorial - 01. ClearScreen"
- D3D12 디바이스 생성 (`D3D12CreateDevice`), DXGI 팩토리(`CreateDXGIFactory2`)
- Command Queue 생성 (`ID3D12CommandQueue`, DIRECT 타입)
- 스왑체인 생성 (`IDXGISwapChain3`, 더블 버퍼링, `DXGI_SWAP_EFFECT_FLIP_DISCARD`)
- RTV용 Descriptor Heap 생성 + 백버퍼 2개에 대한 RTV 생성
- Command Allocator + Command List 생성
- Fence 기반 프레임 동기화 (`WaitForPreviousFrame`: Signal → 필요시 `SetEventOnCompletion` + `WaitForSingleObject`)
- 매 프레임 렌더링:
  1. Command List/Allocator Reset
  2. 리소스 배리어: PRESENT → RENDER_TARGET
  3. `OMSetRenderTargets` + `ClearRenderTargetView` (CornflowerBlue: 0.392, 0.584, 0.929, 1.0)
  4. 리소스 배리어: RENDER_TARGET → PRESENT
  5. Command List Close → `ExecuteCommandLists`
  6. `Present(1, 0)` (vsync on)
  7. `WaitForPreviousFrame`
- Debug 빌드에서 `ID3D12Debug` 활성화 (`D3D12GetDebugInterface`)
- 지오메트리/셰이더/파이프라인 스테이트 없음 — 그리기 호출 없이 clear만 수행
- 외부 의존성 없음. Windows SDK(d3d12.h, dxgi1_6.h)만 사용

## 코드 구조
- `Common/` 프레임워크 없이 1단계는 완전히 독립적인 단일 프로젝트로 시작 (`App` 클래스가 D3D12 상태를 직접 소유). 이후 단계에서 중복이 발생하면 그때 공용 베이스를 뽑아낸다.
- `WinMain.cpp`: 창 클래스 등록, 창 생성, 메시지 루프에서 `App::OnRender()` 호출
- `App.h`/`App.cpp`: `Microsoft::WRL::ComPtr`로 D3D12 리소스 관리, `Initialize()`/`Render()`/`Cleanup()` 구조

## 빌드 설정
- Visual Studio 2022 (v143 툴셋), C++20, Unicode
- 플랫폼: x64만 (Win32 없음) — D3D12 실무 관행에 맞춤
- 링크: `d3d12.lib`, `dxgi.lib`
- Windows SDK: 시스템에 설치된 10.0.26100.0 사용

## 빌드/검증 절차
1. `MSBuild Dx12Tutorial.sln /p:Configuration=Debug /p:Platform=x64`로 빌드
2. 빌드된 exe 실행
3. PowerShell로 창을 캡처하여 `docs/images/01_ClearScreen.png`로 저장
4. 프로세스 종료
5. 스크린샷을 루트 README와 단계별 README에 상대경로로 삽입

## README 구성
- **루트 README.md**: 프로젝트 소개(한글), 프로젝트 바로가기 그리드 테이블(현재 1개 항목, 로컬 이미지 사용), 빌드 방식, 참고 자료, 라이선스(MIT) 섹션. D3D11판과 동일한 어조/구조를 따르되 이미지는 GitHub CDN 대신 리포 내 로컬 경로(`docs/images/...`) 참조.
- **`Dx12/01_ClearScreen/README.md`**: D3D11판의 단계별 README 패턴을 따름 — 내용/주요 구현/결과 + 스크린샷.

## Git 워크플로우
- 브랜치: `01_ClearScreen`
- 커밋 메시지: `[feat]`/`[chore]` 접두사 (예: `[feat] Add step 1: ClearScreen (D3D12 device/swapchain init)`)
- 푸시 → `gh pr create` → `gh pr merge --merge` (일반 merge commit, squash 아님)

## 범위 제외 (다음 단계 이후)
- 삼각형/사각형 렌더링, 셰이더 컴파일 (→ 02단계)
- Common 프레임워크 추출
- Win32 플랫폼 지원
- vcpkg/서드파티 라이브러리 도입
