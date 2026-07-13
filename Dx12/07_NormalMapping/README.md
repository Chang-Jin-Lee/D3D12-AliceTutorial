[◀ 이전: 06. Lighting_BlinnPhong](../06_Lighting_BlinnPhong/README.md) · [🏠 전체 목차](../../README.md) · [다음: 08. Skybox ▶](../08_Skybox/README.md)

## 07. NormalMapping

<p align="center">
  <img src="../../docs/images/07_NormalMapping_Concepts.svg" width="100%" alt="07단계 핵심 개념 흐름도" />
</p>

- 더 쉬운 설명: [GUIDE.md](GUIDE.md)
- 내용: 6단계의 디퓨즈+스페큘러 라이팅은 그대로 두고, 큐브 면의 노멀을 노멀 맵으로 픽셀마다 살짝 비틀어서 평평한 면에 굴곡이 있는 것처럼 보이게 합니다.
- 주요 구현:
  - `Vertex`에 면당 고정된 탄젠트(`TANGENT`) 추가 (각 면의 UV +U 방향과 일치하도록 축 정렬로 계산)
  - CPU에서 8x8 범프 하이트필드(사인 곡선)를 만들고, 중심차분으로 기울기를 구해 노멀 맵(RGB) 텍스처로 인코딩 (이미지 파일/WIC 의존성 없음)
  - 루트 시그니처의 SRV 디스크립터 테이블을 2개 슬롯(t0 디퓨즈, t1 노멀 맵)으로 확장
  - 픽셀 셰이더에서 노멀/탄젠트/바이탄젠트로 TBN 행렬을 만들고, 노멀 맵 색을 `[-1,1]`로 복원해 TBN으로 월드 공간에 옮김
  - 6단계와 동일한 디퓨즈+스페큘러 계산에, 교란된(perturbed) 노멀만 새로 끼워 넣음
- 결과: 체커보드의 각 칸마다 둥근 범프 음영이 생겨, 평평한 면인데도 올록볼록한 질감으로 보임

<p align="center">
  <img src="../../docs/images/07_NormalMapping.png" width="60%" />
</p>
