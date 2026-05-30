# Silver Line 1호 — ESP32-S3 인터넷 라디오

> 📄 **전체 기술 문서 → [HANDOVER.md](./HANDOVER.md)**  
> 📋 **변경 이력 → [CHANGELOG.md](./CHANGELOG.md)**

ESP32-S3-WROOM-1-N16R8 기반 인터넷 라디오.  
한국 HLS 라디오(MBC/KBS/SBS/CBS)를 PC 프록시로 변환해 재생하며, Home Assistant 미디어 위젯으로 제어 가능.

---

## 구성

| 구성 요소 | 역할 |
|-----------|------|
| **ESP32-S3 펌웨어** | 오디오 재생, TFT 디스플레이, 버튼, MQTT |
| **radio-proxy** (Node.js) | HLS → MP3 변환 프록시, Windows NSSM 서비스 |
| **ha-custom-component** (Python) | Home Assistant `media_player` 엔티티 |

## 빠른 시작

```cmd
# 1. src/credentials.h.example → src/credentials.h 복사 후 값 입력
# 2. PlatformIO 빌드 & 업로드
pio run -t upload
pio device monitor

# 3. 프록시 서버 실행 (HLS 방송국 필수)
cd radio-proxy
npm install
node server.js
```

## 버전 다운로드

각 릴리즈의 소스 전체를 zip으로 다운로드:  
👉 **[Releases](https://github.com/pilos011/ESP32-Network-Radio/releases)**

| 버전 | 주요 내용 | 다운로드 |
|------|-----------|----------|
| [v1.0.0](https://github.com/pilos011/ESP32-Network-Radio/releases/tag/v1.0.0) | 최초 안정 릴리즈, MQTT/HA 통합 완료 | [zip](https://github.com/pilos011/ESP32-Network-Radio/archive/refs/tags/v1.0.0.zip) |

## 하드웨어 (v1.0.0)

- **MCU**: ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB Octal PSRAM)
- **앰프**: MAX98357A (I2S, GAIN 핀 100Ω→GND, 3dB)
- **디스플레이**: ST7789V 240×320 TFT
- **버튼**: 2개 (볼륨, 채널, 전원, AP 모드)
- **스피커**: 4Ω 3W (→ v1.1.0에서 8Ω 5W 듀얼로 업그레이드 예정)

## 브랜치 전략

| 브랜치 | 용도 |
|--------|------|
| `main` | 안정 릴리즈만 머지. 태그 = GitHub Release |
| `feat/*` | 새 기능 개발. 검증 완료 후 main으로 머지 |

현재 진행 중인 브랜치:
- `feat/speaker-8ohm-dual` — 8Ω 5W 듀얼 스피커 교체 대응

---

📄 상세 내용 (핀 배치, 빌드, MQTT, 트러블슈팅): **[HANDOVER.md](./HANDOVER.md)**
