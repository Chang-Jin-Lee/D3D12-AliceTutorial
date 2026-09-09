# 16단계 DXR RayTracing 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 15단계 라스터 파이프라인 위에 DXR 1.0 정석 파이프라인(BLAS/TLAS → 상태 객체 → 셰이더 테이블 → `DispatchRays`)으로 레이트레이싱 그림자를 얹고, F키로 9단계 셰도우맵과 토글 비교할 수 있게 만든다.

**Architecture:** `Dx12/16_DXRRayTracing/`는 15단계를 복사해 확장한다. 프레임 앞부분에 TLAS 재빌드 + `DispatchRays`가 들어가 전체 화면 그림자 마스크(`R8_UNORM` UAV)를 만들고, 그 마스크가 바인드리스 힙 슬롯 4의 SRV로 전환되어 기존 PBR 픽셀 셰이더에서 셰도우맵 대신 샘플된다. MSAA·블룸·톤맵·워커 스레드 4개는 손대지 않는다.

**Tech Stack:** D3D12 (`ID3D12Device5`, `ID3D12GraphicsCommandList4`), DXR 1.0, DXC(`IDxcCompiler3`, `lib_6_3`), DirectXMath, Windows SDK 10.0.26100, Visual Studio 2022+ / MSBuild, x64 전용.

## Global Constraints

- 설계 문서: `docs/superpowers/specs/2026-09-09-step16-dxr-raytracing-design.md` — 충돌 시 스펙이 기준이다.
- **외부 의존성 금지.** Windows SDK만 사용한다. vcpkg·d3dx12.h·서드파티 라이브러리 모두 금지. `D3D12_STATE_SUBOBJECT` 배열은 손으로 구성한다.
- **작업 브랜치:** `16_DXRRayTracing` (이미 생성됨). 커밋 접두사 `[feat]` / `[docs]` / `[chore]`.
- **커밋 트레일러:** 모든 커밋 메시지 끝에 `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- **주석 언어는 영어**, 문서(README/GUIDE)는 한국어 — 15단계까지의 관행 그대로.
- **15단계 코드는 이유 없이 바꾸지 않는다.** DXR이 요구하는 변경만 가한다.
- MSBuild 경로: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
- 빌드 명령(모든 태스크 공통):
  ```
  "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" \
    Dx12/Dx12Tutorial.sln -t:16_DXRRayTracing -p:Configuration=Debug -p:Platform=x64 -m -v:m
  ```
- **테스트 프레임워크가 없다.** 이 레포의 검증 사이클은 `빌드 성공 → 실행 → D3D12 디버그 레이어 경고 0건 → 화면 확인`이다. 각 태스크의 "테스트"는 이 사이클을 뜻한다.
- 광선/조명 부호 규약: `lightDirection`은 **빛에서 나가는 방향**이다. 표면에서 빛을 향하는 벡터는 `L = -lightDirection.xyz` (`Shaders.hlsl:123`과 동일).
- 행렬 규약: C++에서 `XMMatrixTranspose`로 저장하고 HLSL에서 `mul(vector, matrix)`로 쓴다 (기존 단계 전부 동일).

---

### Task 1: 프로젝트 스캐폴딩

15단계를 복사해 16단계 프로젝트를 만들고, DXC 런타임 DLL이 출력 폴더에 놓이게 한다. 이 태스크가 끝나면 16단계는 **15단계와 픽셀 단위로 똑같은 화면**을 낸다 — DXR 코드는 아직 없다.

**Files:**
- Create: `Dx12/16_DXRRayTracing/` (15단계에서 복사, `x64/` 빌드 산출물은 제외)
- Create: `Dx12/16_DXRRayTracing/16_DXRRayTracing.vcxproj`, `.vcxproj.filters`
- Modify: `Dx12/16_DXRRayTracing/WinMain.cpp` (창 클래스명·타이틀)
- Modify: `Dx12/Dx12Tutorial.sln` (프로젝트 등록)

**Interfaces:**
- Consumes: 없음
- Produces: 빌드 가능한 `16_DXRRayTracing` 프로젝트. 출력 폴더 `Dx12/x64/Debug/`에 `dxcompiler.dll`, `dxil.dll`.

- [ ] **Step 1: 15단계를 복사하고 빌드 산출물 제거**

```bash
cd /c/Github/D3D12-AliceTutorial/Dx12
cp -r 15_MultiThreadedRendering 16_DXRRayTracing
rm -rf 16_DXRRayTracing/x64
mv 16_DXRRayTracing/15_MultiThreadedRendering.vcxproj 16_DXRRayTracing/16_DXRRayTracing.vcxproj
mv 16_DXRRayTracing/15_MultiThreadedRendering.vcxproj.filters 16_DXRRayTracing/16_DXRRayTracing.vcxproj.filters
```

- [ ] **Step 2: vcxproj의 프로젝트명·GUID 교체**

`16_DXRRayTracing.vcxproj`에서 `15_MultiThreadedRendering` → `16_DXRRayTracing`으로 전부 치환하고, `<ProjectGuid>`를 15단계와 겹치지 않는 새 GUID로 바꾼다. `.filters`의 GUID는 그대로 둬도 무방하다(필터 GUID는 프로젝트 간 충돌하지 않음).

- [ ] **Step 3: DXC 런타임 DLL 복사 항목 추가**

`16_DXRRayTracing.vcxproj`의 기존 `<CopyFileToFolders>` ItemGroup 바로 뒤에 추가한다. `$(WindowsSdkDir)`/`$(TargetPlatformVersion)`은 MSBuild가 채워준다.

```xml
  <!-- DXR needs DXIL (SM 6.3+), which FXC can't produce - so this step
       compiles RayTracing.hlsl with DXC at runtime (see
       App::InitRaytracingPipeline). Both DLLs ship with the Windows SDK:
       dxcompiler.dll is the compiler itself, and dxil.dll is what signs
       the resulting DXIL. Without a signature D3D12 rejects the shader
       outright unless Developer Mode is on, so copying only
       dxcompiler.dll produces a confusing runtime failure. -->
  <ItemGroup>
    <None Include="$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x64\dxcompiler.dll">
      <DeploymentContent>true</DeploymentContent>
    </None>
    <None Include="$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x64\dxil.dll">
      <DeploymentContent>true</DeploymentContent>
    </None>
  </ItemGroup>
  <Target Name="CopyDxcRuntime" AfterTargets="Build">
    <Copy SourceFiles="$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x64\dxcompiler.dll;$(WindowsSdkDir)bin\$(TargetPlatformVersion)\x64\dxil.dll"
          DestinationFolder="$(OutDir)"
          SkipUnchangedFiles="true" />
  </Target>
```

또한 새로 추가될 `RayTracing.hlsl`을 기존 HLSL들과 같은 방식으로 출력 폴더에 복사하도록 `<CopyFileToFolders Include="RayTracing.hlsl">` 항목을 추가한다 (내용은 Task 5에서 채운다 — 지금은 빈 파일이라도 만들어 둔다).

- [ ] **Step 4: 솔루션에 등록**

`Dx12/Dx12Tutorial.sln`에 15단계 항목을 본떠 `16_DXRRayTracing` 프로젝트 항목과 `x64|Debug`/`x64|Release` 구성 매핑을 추가한다. GUID는 Step 2에서 만든 것과 일치해야 한다.

- [ ] **Step 5: WinMain 이름 변경**

```cpp
constexpr wchar_t kWindowClassName[] = L"D3D12DXRRayTracingWindowClass";
constexpr wchar_t kWindowTitle[] = L"D3D12 Tutorial - 16. DXRRayTracing";
```

- [ ] **Step 6: 빌드하고 실행해 15단계와 같은 화면인지 확인**

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Dx12/Dx12Tutorial.sln -t:16_DXRRayTracing -p:Configuration=Debug -p:Platform=x64 -m -v:m
ls Dx12/x64/Debug/dxcompiler.dll Dx12/x64/Debug/dxil.dll
```

Expected: 빌드 성공, 두 DLL 존재. 실행하면 회전하는 큐브 4x4 그리드 + 그림자 + 블룸 — 15단계와 동일.

- [ ] **Step 7: 커밋**

```bash
git add Dx12/16_DXRRayTracing Dx12/Dx12Tutorial.sln
git commit -F- <<'MSG'
[chore] Scaffold step 16 by copying step 15

Adds the DXC runtime DLL copy step (dxcompiler.dll + dxil.dll) that the
raytracing shader compilation in later commits needs.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 2: DXR 지원 확인

`ID3D12Device5`를 얻고 레이트레이싱 티어를 확인한다. 지원하지 않는 GPU에서 알 수 없는 크래시 대신 명확한 메시지가 나오게 하는 것이 목적이다.

**Files:**
- Modify: `Dx12/16_DXRRayTracing/App.h` (멤버 + 함수 선언)
- Modify: `Dx12/16_DXRRayTracing/App.cpp` (`InitRaytracingSupport` 구현 + 생성자 호출)

**Interfaces:**
- Consumes: Task 1의 프로젝트
- Produces: `Microsoft::WRL::ComPtr<ID3D12Device5> m_dxrDevice` — Task 3~7이 `CreateStateObject` / AS 빌드에 쓴다.

- [ ] **Step 1: App.h에 선언 추가**

`InitDevice();` 선언 바로 뒤에 `void InitRaytracingSupport();`를, `m_device` 멤버 바로 뒤에 다음을 넣는다.

```cpp
    // DXR entry points (CreateStateObject, GetRaytracingAccelerationStructurePrebuildInfo)
    // live on ID3D12Device5, not the ID3D12Device above - see
    // InitRaytracingSupport, which also rejects GPUs without DXR.
    Microsoft::WRL::ComPtr<ID3D12Device5> m_dxrDevice;
```

- [ ] **Step 2: InitRaytracingSupport 구현**

```cpp
void App::InitRaytracingSupport()
{
    // Everything DXR needs hangs off ID3D12Device5 / ID3D12GraphicsCommandList4,
    // both added in Windows 10 1809. Steps 1-15 never needed more than the
    // base ID3D12Device, so this is the first QueryInterface in the tutorial.
    if (FAILED(m_device.As(&m_dxrDevice)))
    {
        throw std::runtime_error(
            "ID3D12Device5 is unavailable - DirectX Raytracing needs Windows 10 1809 or newer.");
    }

    // Reporting a device is not the same as supporting raytracing: the
    // feature is opt-in per adapter and older GPUs report TIER_NOT_SUPPORTED
    // even on a current OS.
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
        options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
    {
        throw std::runtime_error(
            "This step requires a GPU with DirectX Raytracing Tier 1.0 support (RTX 2000 series or newer, "
            "or an equivalent AMD/Intel part).");
    }
}
```

- [ ] **Step 3: 생성자에서 호출**

`InitDevice();` 바로 다음 줄에 `InitRaytracingSupport();`를 넣는다.

- [ ] **Step 4: 빌드하고 실행**

Expected: 빌드 성공. RTX 4080에서는 예외 없이 15단계와 같은 화면.

- [ ] **Step 5: 커밋**

```bash
git add Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp
git commit -F- <<'MSG'
[feat] Add DXR device and tier detection

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 3: 가속 구조 (BLAS 2개 + TLAS)

큐브와 바닥 각각의 BLAS를 최초 1회 빌드하고, 매 프레임 재빌드할 TLAS와 그 스크래치/인스턴스 버퍼를 준비한다.

**Files:**
- Modify: `Dx12/16_DXRRayTracing/App.h`
- Modify: `Dx12/16_DXRRayTracing/App.cpp`

**Interfaces:**
- Consumes: `m_dxrDevice` (Task 2), `m_vertexBuffer`/`m_indexBuffer`/`m_planeVertexBuffer`/`m_planeIndexBuffer` (기존 `InitSceneGeometry`)
- Produces:
  - `m_topLevelAS` — Task 4의 글로벌 루트 SRV(t0)가 가리킬 리소스
  - `void UpdateTopLevelAccelerationStructure(ID3D12GraphicsCommandList4* cmdList)` — Task 7이 프레임마다 호출
  - `m_cubeBlas` / `m_planeBlas` — Task 6의 셰이더 테이블이 지오메트리 순서(큐브=레코드 0, 바닥=레코드 1)를 맞출 때 참조

- [ ] **Step 1: App.h에 상수·멤버·선언 추가**

```cpp
    // TLAS instance layout: the CubeCount cube instances come first (all
    // sharing m_cubeBlas), then one instance for the ground plane. The
    // order matters - InstanceContributionToHitGroupIndex in
    // UpdateTopLevelAccelerationStructure picks the shader table's cube
    // record for the first CubeCount entries and the plane record for the
    // last one (see InitRaytracingShaderTable).
    static const UINT RaytracingInstanceCount = CubeCount + 1;
```

```cpp
    void InitRaytracingAccelerationStructures();
    void UpdateTopLevelAccelerationStructure(ID3D12GraphicsCommandList4* commandList);
```

```cpp
    // Bottom-level acceleration structures: one per unique mesh, built
    // once in InitRaytracingAccelerationStructures because neither mesh's
    // vertices ever change. The CubeCount spinning cubes all reference
    // m_cubeBlas - that reuse is the entire reason DXR splits acceleration
    // structures into two levels instead of one.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cubeBlas;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_planeBlas;
    // Top-level acceleration structure: rebuilt from scratch every frame
    // (see UpdateTopLevelAccelerationStructure) because the cubes spin.
    // At RaytracingInstanceCount instances a full rebuild costs the same
    // as a refit and keeps the code shorter.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_topLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasScratch;
    // Upload-heap array of D3D12_RAYTRACING_INSTANCE_DESC, kept mapped so
    // each frame's transforms can be written without a Map/Unmap pair.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasInstanceDescs;
    D3D12_RAYTRACING_INSTANCE_DESC* m_mappedTlasInstanceDescs = nullptr;
    // Object-to-world transform of every cube this frame, written by
    // Update() from the same matrix it feeds the raster constant buffers.
    // UpdateTopLevelAccelerationStructure copies these into the instance
    // descs, so the rasterizer and the ray tracer always agree on where
    // the geometry is.
    std::array<DirectX::XMFLOAT3X4, CubeCount> m_cubeInstanceTransforms;
```

- [ ] **Step 2: 익명 네임스페이스에 UAV 버퍼 헬퍼 추가**

기존 `CreateUploadBuffer` 옆에 둔다. 가속 구조는 `ALLOW_UNORDERED_ACCESS` 플래그가 **필수**다.

```cpp
    // Acceleration structures and their scratch buffers must live in a
    // DEFAULT heap with ALLOW_UNORDERED_ACCESS - the driver writes them
    // from the GPU, so neither an upload heap nor a plain SRV-only buffer
    // works. initialState is RAYTRACING_ACCELERATION_STRUCTURE for the
    // structures themselves and UNORDERED_ACCESS for scratch.
    ComPtr<ID3D12Resource> CreateUavBuffer(
        ID3D12Device* device, UINT64 size, D3D12_RESOURCE_STATES initialState)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        ComPtr<ID3D12Resource> buffer;
        ThrowIfFailed(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, initialState, nullptr, IID_PPV_ARGS(&buffer)));
        return buffer;
    }
```

- [ ] **Step 3: InitRaytracingAccelerationStructures 구현**

BLAS 두 개를 한 커맨드 리스트에 기록하고 즉시 실행·대기한다(초기화 시점이라 프레임 루프와 겹치지 않음). 지오메트리 desc는 정점 버퍼가 업로드 힙에 있어도 문제없다 — 업로드 힙 리소스는 항상 `GENERIC_READ`이고 여기에 `NON_PIXEL_SHADER_RESOURCE`가 포함된다.

핵심 구조:
```cpp
    auto makeGeometryDesc = [](D3D12_GPU_VIRTUAL_ADDRESS vertexAddress, UINT vertexCount,
                               D3D12_GPU_VIRTUAL_ADDRESS indexAddress, UINT indexCount)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
        desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // OPAQUE lets the traversal skip any-hit shaders entirely. Both
        // meshes here are fully opaque, and shadow rays get to stop at the
        // first hit because of it.
        desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        desc.Triangles.VertexBuffer.StartAddress = vertexAddress;
        desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        desc.Triangles.VertexCount = vertexCount;
        desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.IndexBuffer = indexAddress;
        desc.Triangles.IndexCount = indexCount;
        desc.Triangles.IndexFormat = DXGI_FORMAT_R16_UINT;
        desc.Triangles.Transform3x4 = 0;
        return desc;
    };
```

각 BLAS마다: `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS`(Type=BOTTOM_LEVEL, Flags=PREFER_FAST_TRACE, DescsLayout=ARRAY, NumDescs=1, pGeometryDescs) → `m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo` → `CreateUavBuffer(ResultDataMaxSizeInBytes, RAYTRACING_ACCELERATION_STRUCTURE)` + 스크래치 → `BuildRaytracingAccelerationStructure` → 두 BLAS 사이·뒤에 각각 UAV 배리어.

TLAS 쪽: `Type=TOP_LEVEL, NumDescs=RaytracingInstanceCount` 로 prebuild info를 받아 `m_topLevelAS`/`m_tlasScratch`를 만들고, `m_tlasInstanceDescs`는 `sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * RaytracingInstanceCount` 크기의 업로드 버퍼로 만들어 `Map`한 채 유지한다. 정적인 필드(`InstanceID`, `InstanceMask=0xFF`, `Flags`, `InstanceContributionToHitGroupIndex`, `AccelerationStructure`)는 여기서 한 번만 채우고, 매 프레임 바뀌는 것은 `Transform`뿐이다.

- 큐브 인스턴스 `i` (0..CubeCount-1): `AccelerationStructure = m_cubeBlas->GetGPUVirtualAddress()`, `InstanceContributionToHitGroupIndex = 0`
- 바닥 인스턴스 (CubeCount): `AccelerationStructure = m_planeBlas->GetGPUVirtualAddress()`, `InstanceContributionToHitGroupIndex = 1`, `Transform` = 항등 행렬(바닥은 월드 좌표로 이미 정의되어 있고 `planeWorld`가 항등이다)

- [ ] **Step 4: Update()에서 큐브 인스턴스 트랜스폼 채우기**

`Update()`의 큐브 루프 안, `cubeWorld`를 만든 직후에 한 줄 추가한다.

```cpp
        // The same matrix the raster constant buffer above gets, in the
        // 3x4 row-major layout D3D12_RAYTRACING_INSTANCE_DESC wants.
        // XMStoreFloat3x4 transposes as it stores, which is exactly the
        // conversion from DirectXMath's row-vector convention to DXR's.
        XMStoreFloat3x4(&m_cubeInstanceTransforms[cubeIndex], cubeWorld);
```

- [ ] **Step 5: UpdateTopLevelAccelerationStructure 구현**

```cpp
void App::UpdateTopLevelAccelerationStructure(ID3D12GraphicsCommandList4* commandList)
{
    // Only the transforms change frame to frame - every other field was
    // filled once in InitRaytracingAccelerationStructures.
    for (UINT cubeIndex = 0; cubeIndex < CubeCount; ++cubeIndex)
    {
        memcpy(m_mappedTlasInstanceDescs[cubeIndex].Transform,
               &m_cubeInstanceTransforms[cubeIndex],
               sizeof(m_mappedTlasInstanceDescs[cubeIndex].Transform));
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = RaytracingInstanceCount;
    inputs.InstanceDescs = m_tlasInstanceDescs->GetGPUVirtualAddress();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_topLevelAS->GetGPUVirtualAddress();
    commandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

    // The DispatchRays below reads what this build just wrote. There's no
    // resource state transition for acceleration structures - they stay in
    // RAYTRACING_ACCELERATION_STRUCTURE forever - so a UAV barrier is the
    // only thing ordering the two.
    D3D12_RESOURCE_BARRIER tlasBarrier = {};
    tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlasBarrier.UAV.pResource = m_topLevelAS.Get();
    commandList->ResourceBarrier(1, &tlasBarrier);
}
```

- [ ] **Step 6: 생성자에서 호출 + 소멸자에서 Unmap**

`InitSceneGeometry();` 뒤에 `InitRaytracingAccelerationStructures();`를 넣는다(정점 버퍼가 있어야 한다). 소멸자에 `if (m_tlasInstanceDescs) m_tlasInstanceDescs->Unmap(0, nullptr);`를 추가한다.

- [ ] **Step 7: 빌드하고 실행**

아직 `UpdateTopLevelAccelerationStructure`를 프레임에서 부르지 않으므로 화면은 그대로다. 검증 포인트는 **BLAS 빌드가 디버그 레이어 경고 없이 통과**하는 것.

- [ ] **Step 8: 커밋**

```bash
git add Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp
git commit -F- <<'MSG'
[feat] Build bottom-level acceleration structures and TLAS buffers

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 4: 레이트레이싱 출력 리소스와 루트 시그니처

그림자 마스크 텍스처, 그 UAV 힙, 바인드리스 힙 슬롯 4의 SRV, 그리고 글로벌·로컬 루트 시그니처를 만든다.

**Files:**
- Modify: `Dx12/16_DXRRayTracing/App.h`
- Modify: `Dx12/16_DXRRayTracing/App.cpp`

**Interfaces:**
- Consumes: `m_dxrDevice` (Task 2), `m_srvHeap` (기존 바인드리스 힙)
- Produces:
  - `m_shadowMask`, `m_raytracingUavHeap` — Task 7의 `DispatchRays`가 쓴다
  - `m_raytracingGlobalRootSignature`, `m_raytracingLocalRootSignature` — Task 5의 상태 객체가 참조
  - `m_raytracingConstantBuffer` / `RaytracingConstantBuffer` 구조체 — Task 7이 프레임마다 채운다
  - `static const UINT ShadowMaskBindlessIndex = 4;` — Task 8의 픽셀 셰이더가 이 인덱스로 마스크를 읽는다

- [ ] **Step 1: App.h에 상수 버퍼 구조체 추가**

```cpp
// Global root signature b0 for the raytracing pass. Everything RayGen
// needs to turn a pixel coordinate into a world-space camera ray, plus
// the light direction the shadow ray aims at.
struct RaytracingConstantBuffer
{
    DirectX::XMFLOAT4X4 inverseViewProjection;
    DirectX::XMFLOAT4 cameraPosition;
    DirectX::XMFLOAT4 lightDirection; // points FROM the light, same as SceneConstantBuffer
};
```

- [ ] **Step 2: App.h에 멤버·상수·선언 추가**

```cpp
    // Bindless slot the raytraced shadow mask's SRV lives in - slots 0-3
    // are the two diffuse textures, the normal map, and the shadow map
    // (InitTextures), so the mask takes the next free one. Shaders.hlsl
    // reads it through BindlessMaterialIndices::shadowMaskIndex.
    static const UINT ShadowMaskBindlessIndex = 4;
```

```cpp
    void InitRaytracingOutput();
    void InitRaytracingRootSignatures();
```

```cpp
    // Full-resolution, single-sample visibility mask written by
    // DispatchRays and read by the color pass. R8_UNORM is enough: the
    // shader only ever writes 0 (occluded) or 1 (lit).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_shadowMask;
    // A separate one-descriptor heap for the mask's UAV. It can't live in
    // m_srvHeap alongside the mask's SRV, because the color pass keeps
    // m_srvHeap bound while the mask is in PIXEL_SHADER_RESOURCE - a heap
    // holding a UAV of a resource that isn't in UNORDERED_ACCESS is fine,
    // but keeping the two views in separate heaps makes the ownership
    // obvious.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_raytracingUavHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_raytracingConstantBuffer;
    RaytracingConstantBuffer* m_mappedRaytracingConstantBuffer = nullptr;
    // Shared by every raytracing shader in the state object.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_raytracingGlobalRootSignature;
    // Bound per shader table record instead of per command list - this is
    // what lets the cube and plane hit groups read different vertex and
    // index buffers from the same ClosestHit code.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_raytracingLocalRootSignature;
```

- [ ] **Step 3: InitRaytracingOutput 구현**

`m_width x m_height`, `DXGI_FORMAT_R8G8B8A8_UNORM`이 아니라 `DXGI_FORMAT_R8_UNORM`, `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, 초기 상태 `D3D12_RESOURCE_STATE_UNORDERED_ACCESS`로 DEFAULT 힙 텍스처를 만든다. 그 다음:
- `m_raytracingUavHeap`: `CBV_SRV_UAV`, `NumDescriptors = 1`, `SHADER_VISIBLE` → `CreateUnorderedAccessView(m_shadowMask, nullptr, &uavDesc, handle)` (`uavDesc.ViewDimension = TEXTURE2D`, `Format = R8_UNORM`)
- `m_srvHeap`의 `ShadowMaskBindlessIndex`번째 슬롯에 `CreateShaderResourceView`(`Format = R8_UNORM`, `TEXTURE2D`, `MipLevels = 1`, 기본 컴포넌트 매핑)
- `m_raytracingConstantBuffer`: 기존 `CreateConstantBuffer` 헬퍼로 `sizeof(RaytracingConstantBuffer)`

**호출 순서 주의:** `InitTextures()`가 `m_srvHeap`을 만들므로 `InitRaytracingOutput()`은 그 뒤에 와야 한다.

- [ ] **Step 4: InitRaytracingRootSignatures 구현**

글로벌:
```cpp
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0; // u0

    D3D12_ROOT_PARAMETER globalParameters[3] = {};
    globalParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    globalParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    globalParameters[0].DescriptorTable.pDescriptorRanges = &uavRange;
    // The TLAS binds as a plain root SRV rather than through a heap - an
    // acceleration structure SRV has no CPU descriptor to create, only a
    // GPU virtual address, so a root descriptor is the natural fit.
    globalParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    globalParameters[1].Descriptor.ShaderRegister = 0; // t0
    globalParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    globalParameters[2].Descriptor.ShaderRegister = 0; // b0
```
플래그는 `D3D12_ROOT_SIGNATURE_FLAG_NONE` (레이트레이싱 루트 시그니처에는 입력 어셈블러 플래그가 의미 없다).

로컬:
```cpp
    D3D12_ROOT_PARAMETER localParameters[2] = {};
    localParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    localParameters[0].Descriptor.ShaderRegister = 1; // t1 - vertex buffer
    localParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    localParameters[1].Descriptor.ShaderRegister = 2; // t2 - index buffer
    // This one flag is the entire difference between a global and a local
    // root signature. Its arguments come from the shader table record
    // rather than from SetComputeRootSignature calls on the command list.
    localDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
```

둘 다 `D3D12SerializeRootSignature` + `CreateRootSignature` (기존 `InitRootSignature`와 같은 방식).

- [ ] **Step 5: 생성자에서 호출**

`InitTextures();` 뒤에 `InitRaytracingOutput();`, `InitRaytracingRootSignatures();`를 넣는다. 소멸자에 `if (m_raytracingConstantBuffer) m_raytracingConstantBuffer->Unmap(0, nullptr);` 추가.

- [ ] **Step 6: 빌드하고 실행**

Expected: 빌드 성공, 화면 변화 없음, 디버그 레이어 경고 0건.

- [ ] **Step 7: 커밋**

```bash
git add Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp
git commit -F- <<'MSG'
[feat] Add raytracing output mask and root signatures

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 5: RayTracing.hlsl + DXC + 상태 객체

레이트레이싱 셰이더를 쓰고, DXC로 `lib_6_3` 컴파일하고, 서브오브젝트 배열을 손으로 엮어 `CreateStateObject`를 호출한다. 이 태스크가 이 단계의 핵심이다.

**Files:**
- Create: `Dx12/16_DXRRayTracing/RayTracing.hlsl`
- Modify: `Dx12/16_DXRRayTracing/App.h`, `App.cpp`

**Interfaces:**
- Consumes: `m_raytracingGlobalRootSignature`, `m_raytracingLocalRootSignature` (Task 4)
- Produces:
  - `m_raytracingStateObject` (`ID3D12StateObject`) — Task 7이 `SetPipelineState1`에 넘긴다
  - export 이름 상수 `kRayGenShaderName = L"RayGenShader"`, `kMissShaderName = L"MissShader"`, `kShadowMissShaderName = L"ShadowMissShader"`, `kCubeHitGroupName = L"CubeHitGroup"`, `kPlaneHitGroupName = L"PlaneHitGroup"` — Task 6이 `GetShaderIdentifier`에 쓴다

- [ ] **Step 1: RayTracing.hlsl 작성**

```hlsl
// Global root signature - shared by every shader in the state object.
RWTexture2D<float> g_shadowMask : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);

cbuffer RaytracingConstants : register(b0)
{
    matrix inverseViewProjection;
    float4 cameraPosition;
    float4 lightDirection; // points FROM the light, same as Shaders.hlsl
};

// Local root signature - bound per shader table record, so the cube hit
// group and the plane hit group see different buffers here even though
// both run the same ClosestHitShader below.
struct Vertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
};
StructuredBuffer<Vertex> l_vertices : register(t1);
ByteAddressBuffer l_indices : register(t2);

struct RayPayload
{
    // Distance along the camera ray to the surface, or -1 if the ray
    // escaped the scene entirely.
    float hitDistance;
    float3 worldNormal;
};

struct ShadowPayload
{
    bool isLit;
};

// ByteAddressBuffer only loads 32-bit words, but the index buffers are
// R16_UINT (see InitSceneGeometry). Three 16-bit indices span either the
// low+high halves of two dwords or one and a half - so load two dwords
// from the enclosing 4-byte-aligned address and unpack based on which
// half the triangle started in. This is the standard idiom from the
// official DirectX raytracing samples.
uint3 LoadTriangleIndices(uint primitiveIndex)
{
    const uint indicesPerTriangle = 3;
    const uint bytesPerIndex = 2;
    uint offsetBytes = primitiveIndex * indicesPerTriangle * bytesPerIndex;

    uint alignedOffset = offsetBytes & ~3;
    uint2 four16BitIndices = l_indices.Load2(alignedOffset);

    if (alignedOffset == offsetBytes)
    {
        return uint3(four16BitIndices.x & 0xffff,
                     four16BitIndices.x >> 16,
                     four16BitIndices.y & 0xffff);
    }

    return uint3(four16BitIndices.x >> 16,
                 four16BitIndices.y & 0xffff,
                 four16BitIndices.y >> 16);
}

[shader("raygeneration")]
void RayGenShader()
{
    uint2 pixel = DispatchRaysIndex().xy;
    float2 uv = (float2(pixel) + 0.5f) / float2(DispatchRaysDimensions().xy);
    // UV origin is top-left, NDC origin is centre with +Y up.
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    // Unproject the far plane to get a point the camera ray passes
    // through. Same matrix convention as every other shader here: the CPU
    // stored the transpose, so mul(vector, matrix) is a row-vector
    // multiply.
    float4 farPoint = mul(float4(ndc, 1.0f, 1.0f), inverseViewProjection);
    farPoint /= farPoint.w;

    RayDesc cameraRay;
    cameraRay.Origin = cameraPosition.xyz;
    cameraRay.Direction = normalize(farPoint.xyz - cameraPosition.xyz);
    cameraRay.TMin = 0.001f;
    cameraRay.TMax = 1000.0f;

    RayPayload payload;
    payload.hitDistance = -1.0f;
    payload.worldNormal = float3(0.0f, 1.0f, 0.0f);
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 1,
             /*MissShaderIndex*/ 0,
             cameraRay, payload);

    // Nothing there - that pixel is sky, and sky is never in shadow.
    if (payload.hitDistance < 0.0f)
    {
        g_shadowMask[pixel] = 1.0f;
        return;
    }

    float3 hitPosition = cameraRay.Origin + cameraRay.Direction * payload.hitDistance;
    float3 normal = normalize(payload.worldNormal);
    float3 toLight = -normalize(lightDirection.xyz);

    // A surface angled away from the light is shadowed by its own
    // geometry. Deciding that here costs one dot product and skips a ray
    // that would otherwise self-intersect and produce acne.
    if (dot(normal, toLight) <= 0.0f)
    {
        g_shadowMask[pixel] = 0.0f;
        return;
    }

    RayDesc shadowRay;
    // Offsetting along the normal, not just relying on TMin, keeps grazing
    // angles from re-hitting the surface the ray started on.
    shadowRay.Origin = hitPosition + normal * 0.01f;
    shadowRay.Direction = toLight;
    shadowRay.TMin = 0.001f;
    shadowRay.TMax = 1000.0f;

    ShadowPayload shadowPayload;
    shadowPayload.isLit = false;
    // A shadow ray only needs to know whether anything at all is in the
    // way, so it stops at the first hit and never runs a closest-hit
    // shader. The miss shader flipping isLit to true is the only way the
    // payload comes back lit.
    TraceRay(g_scene,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
             0xFF,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 1,
             /*MissShaderIndex*/ 1,
             shadowRay, shadowPayload);

    g_shadowMask[pixel] = shadowPayload.isLit ? 1.0f : 0.0f;
}

[shader("closesthit")]
void ClosestHitShader(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attributes)
{
    uint3 indices = LoadTriangleIndices(PrimitiveIndex());
    float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);

    float3 objectNormal =
        l_vertices[indices.x].normal * barycentrics.x +
        l_vertices[indices.y].normal * barycentrics.y +
        l_vertices[indices.z].normal * barycentrics.z;

    payload.hitDistance = RayTCurrent();
    // Every instance here is a rotation plus a uniform scale, so the
    // object-to-world 3x3 rotates normals correctly without needing an
    // inverse transpose.
    payload.worldNormal = mul((float3x3)ObjectToWorld3x4(), objectNormal);
}

[shader("miss")]
void MissShader(inout RayPayload payload)
{
    // Leave hitDistance at the -1 RayGenShader set: nothing was hit.
    payload.hitDistance = -1.0f;
}

[shader("miss")]
void ShadowMissShader(inout ShadowPayload payload)
{
    // Reaching the light without hitting anything is what "lit" means.
    payload.isLit = true;
}
```

- [ ] **Step 2: App.h에 export 이름 상수와 멤버 추가**

```cpp
    void InitRaytracingPipeline();
```

```cpp
    // The raytracing equivalent of a PSO. Unlike a graphics PSO it holds
    // every shader that can run during a DispatchRays, plus the root
    // signatures and payload/attribute sizes tying them together.
    Microsoft::WRL::ComPtr<ID3D12StateObject> m_raytracingStateObject;
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> m_raytracingStateObjectProperties;
```

`App.cpp`의 익명 네임스페이스에:
```cpp
    // Export names must match the function names in RayTracing.hlsl
    // exactly - a typo produces a null shader identifier at shader table
    // build time rather than a compile error.
    const wchar_t* kRayGenShaderName = L"RayGenShader";
    const wchar_t* kClosestHitShaderName = L"ClosestHitShader";
    const wchar_t* kMissShaderName = L"MissShader";
    const wchar_t* kShadowMissShaderName = L"ShadowMissShader";
    // Hit group names are invented here, not in the HLSL - a hit group is
    // a D3D-side grouping of up to three shader stages, and the two
    // groups below deliberately share the same ClosestHitShader.
    const wchar_t* kCubeHitGroupName = L"CubeHitGroup";
    const wchar_t* kPlaneHitGroupName = L"PlaneHitGroup";
```

- [ ] **Step 3: DXC 컴파일 헬퍼 작성**

`App.cpp` 상단에 `#include <dxcapi.h>`를 추가하고 익명 네임스페이스에:

```cpp
    // FXC (D3DCompileFromFile, used by every other shader in this project)
    // tops out at shader model 5.1 and emits DXBC. DXR requires DXIL from
    // shader model 6.3 or newer, which only DXC produces - so this one
    // shader goes through a completely separate compiler.
    ComPtr<IDxcBlob> CompileRaytracingLibrary(const wchar_t* fileName)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
            FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
        {
            throw std::runtime_error(
                "Failed to load dxcompiler.dll - it and dxil.dll must sit next to the executable "
                "(the project copies both from the Windows SDK after every build).");
        }

        ComPtr<IDxcBlobEncoding> source;
        ThrowIfFailed(utils->LoadFile(fileName, nullptr, &source));

        DxcBuffer sourceBuffer = {};
        sourceBuffer.Ptr = source->GetBufferPointer();
        sourceBuffer.Size = source->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_ACP;

        // No -E entry point: a library target exports every function
        // carrying a [shader("...")] attribute, which is how one file can
        // hold the raygen, closest-hit, and both miss shaders at once.
        std::vector<const wchar_t*> arguments = { L"-T", L"lib_6_3" };
#if defined(_DEBUG)
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Qembed_debug");
        arguments.push_back(L"-Od");
#endif

        ComPtr<IDxcResult> result;
        ThrowIfFailed(compiler->Compile(
            &sourceBuffer, arguments.data(), static_cast<UINT32>(arguments.size()),
            nullptr, IID_PPV_ARGS(&result)));

        HRESULT compileStatus = S_OK;
        ThrowIfFailed(result->GetStatus(&compileStatus));
        if (FAILED(compileStatus))
        {
            ComPtr<IDxcBlobUtf8> errors;
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
            throw std::runtime_error(
                errors && errors->GetStringLength() > 0
                    ? std::string("RayTracing.hlsl failed to compile:\n") + errors->GetStringPointer()
                    : "RayTracing.hlsl failed to compile.");
        }

        ComPtr<IDxcBlob> shader;
        ThrowIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr));
        return shader;
    }
```

`App.cpp`에 `#pragma comment(lib, "dxcompiler.lib")`를 추가하거나 vcxproj의 `AdditionalDependencies`에 `dxcompiler.lib`를 넣는다 — 기존 `d3d12.lib` 지정 방식을 따른다.

- [ ] **Step 4: InitRaytracingPipeline 구현 — 서브오브젝트 배열**

`d3dx12.h` 없이 `D3D12_STATE_SUBOBJECT`를 직접 채운다. 서브오브젝트가 가리키는 desc 구조체들은 `CreateStateObject` 호출 시점까지 살아 있어야 하므로 전부 스택 지역 변수로 두고 한 함수 안에서 끝낸다.

```cpp
    ComPtr<IDxcBlob> library = CompileRaytracingLibrary(L"RayTracing.hlsl");

    // Eight subobjects: library, two hit groups, shader config, local root
    // signature + its association, global root signature, pipeline config.
    D3D12_STATE_SUBOBJECT subobjects[8] = {};
    UINT subobjectIndex = 0;

    // 1. The compiled library, plus which of its exports this state object
    //    actually uses. Naming them explicitly (rather than passing 0
    //    exports to take everything) keeps the state object honest about
    //    its own contents.
    D3D12_EXPORT_DESC exportDescs[4] = {};
    exportDescs[0].Name = kRayGenShaderName;
    exportDescs[1].Name = kClosestHitShaderName;
    exportDescs[2].Name = kMissShaderName;
    exportDescs[3].Name = kShadowMissShaderName;

    D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
    libraryDesc.DXILLibrary.pShaderBytecode = library->GetBufferPointer();
    libraryDesc.DXILLibrary.BytecodeLength = library->GetBufferSize();
    libraryDesc.NumExports = _countof(exportDescs);
    libraryDesc.pExports = exportDescs;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[subobjectIndex].pDesc = &libraryDesc;
    ++subobjectIndex;

    // 2/3. Two hit groups over the *same* closest-hit shader. They exist
    //      as separate groups only so the shader table can give each one
    //      different local root arguments - the cube's vertex/index
    //      buffers for one, the plane's for the other.
    D3D12_HIT_GROUP_DESC cubeHitGroup = {};
    cubeHitGroup.HitGroupExport = kCubeHitGroupName;
    cubeHitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    cubeHitGroup.ClosestHitShaderImport = kClosestHitShaderName;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[subobjectIndex].pDesc = &cubeHitGroup;
    ++subobjectIndex;

    D3D12_HIT_GROUP_DESC planeHitGroup = {};
    planeHitGroup.HitGroupExport = kPlaneHitGroupName;
    planeHitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    planeHitGroup.ClosestHitShaderImport = kClosestHitShaderName;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[subobjectIndex].pDesc = &planeHitGroup;
    ++subobjectIndex;

    // 4. Payload and attribute sizes. The driver reserves per-ray stack
    //    space from these numbers, so they must cover the *largest*
    //    payload any shader here uses - RayPayload (16 bytes: one float
    //    plus a float3), not the 4-byte ShadowPayload.
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = 4 * sizeof(float);
    shaderConfig.MaxAttributeSizeInBytes = 2 * sizeof(float); // barycentrics
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[subobjectIndex].pDesc = &shaderConfig;
    ++subobjectIndex;

    // 5/6. The local root signature, and the association saying which
    //      exports it applies to. Without the association the runtime has
    //      no way to know the local signature belongs to the hit groups
    //      rather than to raygen or miss.
    ID3D12RootSignature* localRootSignature = m_raytracingLocalRootSignature.Get();
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    subobjects[subobjectIndex].pDesc = &localRootSignature;
    const D3D12_STATE_SUBOBJECT* localRootSignatureSubobject = &subobjects[subobjectIndex];
    ++subobjectIndex;

    const wchar_t* hitGroupExports[] = { kCubeHitGroupName, kPlaneHitGroupName };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
    association.pSubobjectToAssociate = localRootSignatureSubobject;
    association.NumExports = _countof(hitGroupExports);
    association.pExports = hitGroupExports;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subobjects[subobjectIndex].pDesc = &association;
    ++subobjectIndex;
```

여기서 서브오브젝트 배열 크기가 8이 되도록 글로벌 루트 시그니처와 파이프라인 설정을 마저 넣는다:

```cpp
    // 7. The global root signature - no association needed, it applies to
    //    everything by default.
    ID3D12RootSignature* globalRootSignature = m_raytracingGlobalRootSignature.Get();
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[subobjectIndex].pDesc = &globalRootSignature;
    ++subobjectIndex;

    // 8. Recursion depth. Both rays are fired from RayGenShader rather
    //    than the shadow ray being spawned inside ClosestHitShader, so
    //    depth 1 is enough. Every extra level of recursion costs stack
    //    the driver has to reserve for every ray in flight, so the limit
    //    is worth keeping as tight as the shaders actually need.
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 1;
    subobjects[subobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[subobjectIndex].pDesc = &pipelineConfig;
    ++subobjectIndex;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = subobjectIndex;
    stateObjectDesc.pSubobjects = subobjects;
    ThrowIfFailed(m_dxrDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_raytracingStateObject)));
    // Needed to look up shader identifiers when building the shader table.
    ThrowIfFailed(m_raytracingStateObject.As(&m_raytracingStateObjectProperties));
```

위 코드의 `subobjects` 배열은 8칸으로 선언되어 있고, `subobjectIndex`가 정확히 8에서 끝나야 한다.

- [ ] **Step 5: 생성자에서 호출**

`InitRaytracingRootSignatures();` 뒤에 `InitRaytracingPipeline();`.

- [ ] **Step 6: 빌드하고 실행**

Expected: 빌드 성공, 실행 시 예외 없음(상태 객체 생성 성공), 화면은 아직 변화 없음. `CreateStateObject`가 실패하면 디버그 레이어가 어떤 서브오브젝트가 문제인지 구체적으로 알려준다.

- [ ] **Step 7: 커밋**

```bash
git add Dx12/16_DXRRayTracing/RayTracing.hlsl Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp Dx12/16_DXRRayTracing/16_DXRRayTracing.vcxproj
git commit -F- <<'MSG'
[feat] Compile RayTracing.hlsl with DXC and build the raytracing state object

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 6: 셰이더 테이블

`GetShaderIdentifier`로 얻은 식별자와 로컬 루트 인자를 정렬 규칙에 맞춰 업로드 버퍼에 기록한다.

**Files:**
- Modify: `Dx12/16_DXRRayTracing/App.h`, `App.cpp`

**Interfaces:**
- Consumes: `m_raytracingStateObjectProperties` (Task 5), `m_vertexBuffer`/`m_indexBuffer`/`m_planeVertexBuffer`/`m_planeIndexBuffer`
- Produces: `m_rayGenShaderTable`, `m_missShaderTable`, `m_hitGroupShaderTable` + 각 stride/size 멤버 — Task 7의 `D3D12_DISPATCH_RAYS_DESC`가 그대로 읽는다

- [ ] **Step 1: App.h에 멤버·선언 추가**

```cpp
    void InitRaytracingShaderTable();
```

```cpp
    // Shader tables: GPU-visible arrays of records, each record a 32-byte
    // shader identifier optionally followed by that record's local root
    // arguments. DispatchRays indexes into these by ray type and by the
    // hit group index the TLAS instance selected.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rayGenShaderTable;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_missShaderTable;
    UINT m_missShaderTableStride = 0;
    UINT m_missShaderTableSize = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hitGroupShaderTable;
    UINT m_hitGroupShaderTableStride = 0;
    UINT m_hitGroupShaderTableSize = 0;
```

- [ ] **Step 2: InitRaytracingShaderTable 구현**

```cpp
void App::InitRaytracingShaderTable()
{
    // Two alignment rules apply, and they're different numbers:
    //   - every record's stride is a multiple of 32
    //     (D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT), and
    //   - every table's start address is a multiple of 64
    //     (D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT).
    // Giving each table its own buffer satisfies the second rule for
    // free, since committed resources are already 64-byte aligned.
    const UINT identifierSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES; // 32
    auto alignUp = [](UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    };

    auto shaderIdentifier = [&](const wchar_t* exportName)
    {
        void* identifier = m_raytracingStateObjectProperties->GetShaderIdentifier(exportName);
        if (identifier == nullptr)
        {
            throw std::runtime_error("The raytracing state object has no export by that name.");
        }
        return identifier;
    };

    // RayGen: one record, no local root arguments.
    {
        const UINT tableSize = alignUp(identifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        std::vector<uint8_t> table(tableSize, 0);
        memcpy(table.data(), shaderIdentifier(kRayGenShaderName), identifierSize);
        m_rayGenShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), tableSize);
    }

    // Miss: two records, also with no local root arguments. Their order
    // here is what the MissShaderIndex argument of TraceRay selects -
    // index 0 is the camera ray's miss, index 1 the shadow ray's.
    {
        m_missShaderTableStride = alignUp(identifierSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_missShaderTableSize = m_missShaderTableStride * 2;
        std::vector<uint8_t> table(m_missShaderTableSize, 0);
        memcpy(table.data(), shaderIdentifier(kMissShaderName), identifierSize);
        memcpy(table.data() + m_missShaderTableStride, shaderIdentifier(kShadowMissShaderName), identifierSize);
        m_missShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), m_missShaderTableSize);
    }

    // Hit groups: two records, each carrying two root SRV addresses as
    // local root arguments (16 bytes on top of the 32-byte identifier).
    // This is what the stride is for - the records genuinely differ, so
    // the runtime can't just reuse one.
    {
        const UINT localArgumentSize = 2 * sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        m_hitGroupShaderTableStride =
            alignUp(identifierSize + localArgumentSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_hitGroupShaderTableSize = m_hitGroupShaderTableStride * 2;
        std::vector<uint8_t> table(m_hitGroupShaderTableSize, 0);

        auto writeHitGroupRecord = [&](UINT recordIndex, const wchar_t* hitGroupName,
                                       ID3D12Resource* vertexBuffer, ID3D12Resource* indexBuffer)
        {
            uint8_t* record = table.data() + static_cast<size_t>(recordIndex) * m_hitGroupShaderTableStride;
            memcpy(record, shaderIdentifier(hitGroupName), identifierSize);
            // Root descriptors in a shader record are raw GPU virtual
            // addresses, in the order the local root signature declares
            // them (t1 = vertices, t2 = indices).
            const D3D12_GPU_VIRTUAL_ADDRESS addresses[2] =
            {
                vertexBuffer->GetGPUVirtualAddress(),
                indexBuffer->GetGPUVirtualAddress(),
            };
            memcpy(record + identifierSize, addresses, sizeof(addresses));
        };

        // Record 0 is what TLAS instances with
        // InstanceContributionToHitGroupIndex == 0 (the cubes) select;
        // record 1 is the ground plane's.
        writeHitGroupRecord(0, kCubeHitGroupName, m_vertexBuffer.Get(), m_indexBuffer.Get());
        writeHitGroupRecord(1, kPlaneHitGroupName, m_planeVertexBuffer.Get(), m_planeIndexBuffer.Get());

        m_hitGroupShaderTable = CreateUploadBuffer(m_device.Get(), table.data(), m_hitGroupShaderTableSize);
    }
}
```

`<vector>` 인클루드가 이미 있는지 확인하고 없으면 추가한다.

- [ ] **Step 3: 생성자에서 호출**

`InitRaytracingPipeline();` 뒤에 `InitRaytracingShaderTable();`.

- [ ] **Step 4: 빌드하고 실행**

Expected: 빌드 성공, 예외 없음(모든 export 이름이 실제로 존재). 화면은 아직 변화 없음.

- [ ] **Step 5: 커밋**

```bash
git add Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp
git commit -F- <<'MSG'
[feat] Build the raytracing shader tables

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 7: DispatchRays와 프레임 통합

프레임마다 TLAS를 갱신하고 광선을 쏘아 그림자 마스크를 만든다. 이 태스크가 끝나면 마스크는 생기지만 아직 화면에 반영되지는 않는다.

**Files:**
- Modify: `Dx12/16_DXRRayTracing/App.h`, `App.cpp`

**Interfaces:**
- Consumes: Task 3~6의 모든 산출물
- Produces: 매 프레임 `PIXEL_SHADER_RESOURCE` 상태로 끝나는 `m_shadowMask` — Task 8의 픽셀 셰이더가 읽는다

- [ ] **Step 1: 커맨드 리스트를 ID3D12GraphicsCommandList4로 승격**

`App.h`의 `m_commandList` 타입을 `ComPtr<ID3D12GraphicsCommandList4>`로 바꾼다. `DispatchRays`와 `BuildRaytracingAccelerationStructure`가 이 인터페이스에만 있다. 워커 리스트와 후처리 리스트는 레이트레이싱을 하지 않으므로 `ID3D12GraphicsCommandList` 그대로 둔다. `CreateCommandList`는 `IID_PPV_ARGS`가 알아서 맞춰준다.

- [ ] **Step 2: App.h에 선언 추가**

```cpp
    void RenderRaytracedShadows(ID3D12GraphicsCommandList4* commandList);
```

- [ ] **Step 3: Update()에서 레이트레이싱 상수 버퍼 채우기**

`Update()` 끝부분, 스카이박스 상수 뒤에 추가한다. `view`/`projection`은 이미 그 함수 안에 있다.

```cpp
    // RayGenShader unprojects NDC through this to build camera rays, so it
    // has to be the inverse of exactly the view-projection the raster
    // passes use - otherwise the mask would be offset from the image it
    // gets composited into.
    const XMMATRIX viewProjection = view * projection;
    XMStoreFloat4x4(&m_mappedRaytracingConstantBuffer->inverseViewProjection,
                    XMMatrixTranspose(XMMatrixInverse(nullptr, viewProjection)));
    XMStoreFloat4(&m_mappedRaytracingConstantBuffer->cameraPosition, kEyePosition);
    XMStoreFloat4(&m_mappedRaytracingConstantBuffer->lightDirection, kLightDirection);
```

- [ ] **Step 4: RenderRaytracedShadows 구현**

```cpp
void App::RenderRaytracedShadows(ID3D12GraphicsCommandList4* commandList)
{
    // Raytracing root arguments go through the *compute* setters, not the
    // graphics ones - DispatchRays sits on the compute side of the API
    // even though what it produces here feeds a graphics pass.
    commandList->SetComputeRootSignature(m_raytracingGlobalRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_raytracingUavHeap.Get() };
    commandList->SetDescriptorHeaps(_countof(heaps), heaps);
    commandList->SetComputeRootDescriptorTable(0, m_raytracingUavHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootShaderResourceView(1, m_topLevelAS->GetGPUVirtualAddress());
    commandList->SetComputeRootConstantBufferView(2, m_raytracingConstantBuffer->GetGPUVirtualAddress());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable->GetDesc().Width;
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missShaderTableSize;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderTableStride;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderTableSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderTableStride;
    // One ray per pixel of the final image - the mask is full resolution.
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;

    // SetPipelineState1, not SetPipelineState: a state object isn't a PSO.
    commandList->SetPipelineState1(m_raytracingStateObject.Get());
    commandList->DispatchRays(&dispatchDesc);
}
```

- [ ] **Step 5: Render()에 배치**

`Render()`에서 `m_commandList->Reset(...)` 직후, `IASetPrimitiveTopology` 앞에 넣는다.

```cpp
    // The raytraced shadow mask has to exist before the color pass samples
    // it, and building it from camera rays (rather than from the color
    // pass's depth buffer) is what lets it run first without needing a
    // separate depth pre-pass.
    if (!m_isFirstFrame)
    {
        // Every frame after the first, the color pass left the mask as a
        // pixel shader resource; DispatchRays needs it writable again.
        D3D12_RESOURCE_BARRIER maskToWrite = {};
        maskToWrite.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        maskToWrite.Transition.pResource = m_shadowMask.Get();
        maskToWrite.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        maskToWrite.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        maskToWrite.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &maskToWrite);
    }

    UpdateTopLevelAccelerationStructure(m_commandList.Get());
    RenderRaytracedShadows(m_commandList.Get());

    D3D12_RESOURCE_BARRIER maskToRead = {};
    maskToRead.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    maskToRead.Transition.pResource = m_shadowMask.Get();
    maskToRead.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    maskToRead.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    maskToRead.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &maskToRead);
```

`m_shadowMask`는 `UNORDERED_ACCESS`로 생성되므로 첫 프레임은 전환을 건너뛴다 — 기존 `m_isFirstFrame` 관행 그대로다.

**주의:** `RenderRaytracedShadows`가 `SetDescriptorHeaps`로 `m_raytracingUavHeap`을 바인딩하는데, 색상 패스가 뒤에서 `m_srvHeap`을 다시 바인딩하므로 충돌하지 않는다.

- [ ] **Step 6: 빌드하고 실행 — PIX/디버그 레이어로 검증**

Expected: 빌드 성공, 화면은 아직 15단계와 동일(마스크를 읽는 쪽이 없음), 디버그 레이어 경고 0건. TLAS 빌드와 DispatchRays가 매 프레임 실행되므로 프레임 타임이 약간 늘어나는 것이 정상이다.

- [ ] **Step 7: 커밋**

```bash
git add Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp
git commit -F- <<'MSG'
[feat] Dispatch rays each frame to produce the shadow mask

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 8: 픽셀 셰이더 합성과 F키 토글

마스크를 실제로 화면에 반영하고, F키로 셰도우맵과 전환한다. **이 태스크에서 처음으로 눈에 보이는 변화가 생긴다.**

**Files:**
- Modify: `Dx12/16_DXRRayTracing/Shaders.hlsl`
- Modify: `Dx12/16_DXRRayTracing/App.h`, `App.cpp`
- Modify: `Dx12/16_DXRRayTracing/WinMain.cpp`

**Interfaces:**
- Consumes: `m_shadowMask`의 바인드리스 SRV(Task 4), 마스크 생성(Task 7)
- Produces: 없음 (최종 사용자 기능)

- [ ] **Step 1: BindlessMaterialIndices 확장**

`App.h`:
```cpp
struct BindlessMaterialIndices
{
    UINT diffuseTextureIndex;
    UINT normalMapIndex;
    UINT shadowMapIndex;
    // Which technique this draw call should get its shadowing from:
    // 0 = the step 9 shadow map at shadowMapIndex, 1 = the raytraced mask
    // at ShadowMaskBindlessIndex. Toggled at runtime with the F key so
    // both can be compared on the same frame's geometry.
    UINT shadowMode;
    UINT shadowMaskIndex;
    UINT padding[3];
};
```

루트 상수 개수가 4개에서 8개로 늘어나므로 `InitRootSignature`의 `Constants.Num32BitValues`를 `sizeof(BindlessMaterialIndices) / sizeof(UINT32)`로 맞춘다(하드코딩된 값이 있으면 교체).

- [ ] **Step 2: Shaders.hlsl 수정**

```hlsl
cbuffer MaterialIndices : register(b1)
{
    uint diffuseTextureIndex;
    uint normalMapIndex;
    uint shadowMapIndex;
    uint shadowMode;
    uint shadowMaskIndex;
    uint3 materialIndicesPadding;
};
```

`SampleShadow` 옆에 추가:
```hlsl
// The raytraced mask is a screen-space, one-sample-per-pixel visibility
// buffer, so it's indexed by pixel coordinate rather than by a projected
// light-space UV - no bias, no frustum test, no acne. The 0.3 floor
// matches SampleShadow above so the two techniques dim shadowed areas by
// the same amount and only their *shape* differs when toggling.
float SampleRaytracedShadow(float2 screenPosition)
{
    float visibility = g_bindlessTextures[shadowMaskIndex].Load(int3(int2(screenPosition), 0)).r;
    return lerp(0.3f, 1.0f, visibility);
}
```

`PSMain`의 `float shadowFactor = SampleShadow(input.lightSpacePosition);`를 다음으로 교체:
```hlsl
    float shadowFactor = (shadowMode == 0)
        ? SampleShadow(input.lightSpacePosition)
        : SampleRaytracedShadow(input.position.xy);
```

- [ ] **Step 3: App에 토글 상태와 핸들러 추가**

`App.h` public:
```cpp
    void OnKeyDown(WPARAM key);
```
private:
```cpp
    // 0 = shadow map (step 9), 1 = raytraced mask. Read by Render() when
    // it fills each draw's BindlessMaterialIndices.
    UINT m_shadowMode = 1;
```

`App.cpp`:
```cpp
void App::OnKeyDown(WPARAM key)
{
    if (key != 'F')
    {
        return;
    }

    m_shadowMode = (m_shadowMode == 0) ? 1u : 0u;
    SetWindowTextW(m_hwnd, m_shadowMode == 0
        ? L"D3D12 Tutorial - 16. DXRRayTracing [F] Shadow Map"
        : L"D3D12 Tutorial - 16. DXRRayTracing [F] Raytraced Shadows");
}
```

생성자 끝에서 초기 타이틀을 한 번 세팅한다(기본 모드가 DXR임을 창 제목이 바로 보여주도록).

- [ ] **Step 4: Render()와 RecordWorkerCommandList의 루트 상수 갱신**

바닥 그리기의 상수를:
```cpp
    const BindlessMaterialIndices planeIndices =
        { 1, 2, 3, m_shadowMode, ShadowMaskBindlessIndex, { 0, 0, 0 } };
```
로 바꾸고, `RecordWorkerCommandList`의 큐브 상수도 같은 방식으로 `m_shadowMode`와 `ShadowMaskBindlessIndex`를 포함하게 바꾼다. `m_shadowMode`는 워커 스레드가 **읽기만** 하고 프레임 중에는 바뀌지 않으므로(입력은 메시지 루프에서만 처리되고 `Render()`와 겹치지 않는다) 동기화가 필요 없다.

- [ ] **Step 5: WinMain에 키 입력 연결**

`WndProc`가 `App`에 접근할 수 있도록 `SetWindowLongPtr(hwnd, GWLP_USERDATA, ...)`로 포인터를 심고:
```cpp
    case WM_KEYDOWN:
    {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (app != nullptr)
        {
            app->OnKeyDown(wParam);
        }
        return 0;
    }
```
`App` 생성 직후 `SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));`를 호출하고, 스코프를 벗어나기 전에 `SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);`으로 되돌린다.

- [ ] **Step 6: 빌드하고 실행 — 눈으로 검증**

Expected:
- 실행하면 기본이 레이트레이싱 그림자. 큐브 그림자 윤곽이 셰도우맵보다 또렷하고, 셰도우맵의 계단·아크네가 없다.
- F키를 누르면 셰도우맵으로 바뀌고 창 제목이 따라 바뀐다. 그림자 위치는 두 모드에서 같은 자리에 있어야 한다 — 다르면 TLAS 트랜스폼이나 광선 방향 부호가 틀린 것이다.
- 디버그 레이어 경고 0건.

- [ ] **Step 7: 커밋**

```bash
git add Dx12/16_DXRRayTracing/Shaders.hlsl Dx12/16_DXRRayTracing/App.h Dx12/16_DXRRayTracing/App.cpp Dx12/16_DXRRayTracing/WinMain.cpp
git commit -F- <<'MSG'
[feat] Composite the raytraced shadow mask and add the F key toggle

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

---

### Task 9: 문서와 스크린샷

**Files:**
- Create: `Dx12/16_DXRRayTracing/README.md`, `GUIDE.md` (15단계 것을 덮어씀 — Task 1의 복사본이 15단계 내용을 담고 있다)
- Create: `docs/images/16_DXRRayTracing.png`, `docs/images/16_DXRRayTracing_Concepts.svg`
- Modify: `README.md` (루트), `Dx12/15_MultiThreadedRendering/README.md` (다음 단계 링크)

- [ ] **Step 1: 스크린샷 캡처**

DXR 모드로 실행한 화면을 `docs/images/16_DXRRayTracing.png`로 저장한다. 기존 이미지들과 비슷한 크기·구도를 유지한다.

- [ ] **Step 2: 개념도 SVG 작성**

`docs/images/16_DXRRayTracing_Concepts.svg` — 15단계 SVG의 스타일을 따라 다음 흐름을 그린다:
BLAS(큐브)/BLAS(바닥) → TLAS(인스턴스 17개) → 상태 객체 + 셰이더 테이블 → DispatchRays → 그림자 마스크 UAV → 색상 패스 합성. 셰이더 테이블 레코드 레이아웃(식별자 32B + 로컬 루트 인자)을 따로 확대해 보여준다.

- [ ] **Step 3: README.md 작성**

15단계 README 형식을 그대로 따른다 — 상단 이전/목차 링크, 개념도, `내용`/`주요 구현`/`결과` 항목, 하단 스크린샷.

- [ ] **Step 4: GUIDE.md 작성**

더 쉬운 설명. 반드시 담을 것:
- 라스터 그림자(빛에서 본 깊이맵)와 레이트레이싱 그림자(광선을 직접 쏨)의 개념 차이
- BLAS/TLAS를 두 단계로 나눈 이유 — 큐브 16개가 BLAS 하나를 공유
- 셰이더 테이블이 뭐고 왜 stride가 필요한가 — 히트 그룹 두 레코드가 서로 다른 버퍼 주소를 든다
- 로컬 루트 시그니처 vs 글로벌 루트 시그니처
- `MaxTraceRecursionDepth = 1`인 이유
- `dxil.dll` 함정 — 없으면 DXIL 서명이 안 돼 런타임 거부
- MSAA와의 절충 — 마스크는 픽셀당 1샘플

- [ ] **Step 5: 루트 README 그리드에 16번 추가**

15단계가 있는 마지막 표에 16번 열을 추가한다.

- [ ] **Step 6: 15단계 README에 다음 단계 링크 추가**

15단계 README 상단 네비게이션에 `[다음: 16. DXRRayTracing ▶](../16_DXRRayTracing/README.md)`를 추가한다(1~14단계가 하는 것과 동일).

- [ ] **Step 7: 커밋**

```bash
git add README.md docs/images Dx12/16_DXRRayTracing/README.md Dx12/16_DXRRayTracing/GUIDE.md Dx12/15_MultiThreadedRendering/README.md
git commit -F- <<'MSG'
[docs] Add step 16 README, guide, concept diagram, and screenshot

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
MSG
```

- [ ] **Step 8: 최종 검증 — Release 빌드**

```
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Dx12/Dx12Tutorial.sln -t:16_DXRRayTracing -p:Configuration=Release -p:Platform=x64 -m -v:m
```
Expected: 경고 없이 빌드 성공.

- [ ] **Step 9: PR 생성**

```bash
git push -u origin 16_DXRRayTracing
gh pr create --title "[feat] Add step 16: DXRRayTracing (raytraced shadows with a full DXR 1.0 pipeline)" --body "..."
```
