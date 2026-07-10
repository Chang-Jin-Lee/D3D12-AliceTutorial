[◀ 이전: 05. Lighting](../05_Lighting/README.md) · [🏠 전체 목차](../../README.md)

## 06. Lighting_BlinnPhong

<p align="center">
  <img src="../../docs/images/06_Lighting_BlinnPhong_Concepts.svg" width="100%" alt="06단계 핵심 개념 흐름도" />
</p>

- 더 쉬운 설명: [GUIDE.md](GUIDE.md)
- 내용: 5단계의 디퓨즈 라이팅에 Blinn-Phong 스페큘러(반짝이는 하이라이트)를 추가합니다.
- 주요 구현:
  - 정점 셰이더가 월드 공간 위치(`worldPosition`)도 픽셀 셰이더로 넘김 (카메라까지의 방향을 계산하려면 표면 위치가 필요)
  - 하프 벡터(half vector) 계산: `normalize(toLight + toEye)`
  - 스페큘러 항: `pow(max(dot(normal, halfVector), 0), shininess)`, 빛을 등진 면은 `step()`으로 0 처리
  - 상수 버퍼에 카메라 월드 위치(`eyePosition`)와 스페큘러 색상/광택도(`specularColor.rgb`/`.a`) 추가
  - 최종 색 = 텍스처색 × (ambient + diffuse) + (스페큘러 하이라이트, 텍스처색과 무관하게 더해짐)
  - 큐브가 평평한 면 6개짜리 플랫 셰이딩이라, 하이라이트가 너무 좁으면(광택도가 높으면) 화면에 거의 안 보일 수 있어 광택도를 낮게(8) 잡아 하이라이트를 눈에 띄게 넓혔습니다
- 결과: 카메라 쪽을 향한 면에 밝은 하이라이트가 넓게 나타나고, 큐브가 회전하면서 하이라이트 위치도 함께 바뀝니다

<p align="center">
  <img src="../../docs/images/06_Lighting_BlinnPhong.png" width="60%" />
</p>
