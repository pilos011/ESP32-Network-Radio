# Changelog

모든 변경 사항은 이 파일에 기록합니다.  
형식: [Keep a Changelog](https://keepachangelog.com/) | [Semantic Versioning](https://semver.org/)

---

## [Unreleased] — feat/speaker-8ohm-dual

### Changed
- 스피커: 4Ω 3W → 8Ω 5W × 2개 **병렬 배선** (합성 4Ω)
- MAX98357A GAIN 핀 저항: 100Ω → 100kΩ (하드웨어 게인 3dB → 6dB)
- `radio-proxy/config.ini`: `GAIN_DB=6→3`, `LIMIT=0.85→0.90` (하드웨어 게인 보상)
- `Config.cpp`: `maxVolume` 기본값 11 → 16 (5W 스피커 여유 반영)

---

## [1.0.0] — 2026-05-30

최초 안정 릴리즈. MQTT/HA 통합 완료.

### Added
- **ESP32-S3 N16R8 펌웨어** (PlatformIO / Arduino)
  - WiFi STA + AP 폴백 + Captive Portal
  - NVS 설정 저장/로드 + WebUI (http://\<radio-ip\>/)
  - NTP 시계 (KST) + D2Coding 24px 한글 폰트
  - 듀얼 코어 audio task + PSRAM 1MB 입력 버퍼
  - Stream watchdog (8초 무음 → 재연결, 10회 실패 → reboot)
  - TWDT 15초 자동 회복 + 24h 정기 재부팅
  - 버튼 2단계 롱프레스 상태머신
  - 정각 시보 (DND 23:00~08:00 포함)
- **radio-proxy v5** (Node.js, Windows NSSM 서비스)
  - HLS → MP3 변환 (ffmpeg, 128kbps)
  - 단일 스트림 설계 (좀비 ffmpeg 방지)
  - `taskkill /IM ffmpeg.exe` 신뢰성 확보
  - `config.ini`: LOG_ENABLED, PORT, GAIN_DB, LIMIT
- **HA custom component** (`silverline_radio`)
  - `media_player` 엔티티 — 4개 MQTT discovery 엔티티 연동
  - play/pause/stop/볼륨/소스선택/이전다음
  - 크로스-폰 실시간 동기화 (cmd 토픽 구독)
  - `configuration.yaml`의 `sources:` 목록 지원 (하드코딩 제거)
- MQTT / Home Assistant 통합
  - 4개 discovery 엔티티: switch(power), select(source), number(volume), sensor(station)
  - LWT `availability` 토픽 + 낙관적 업데이트

### Hardware (v1.0.0 기준)
- MAX98357A GAIN 핀: 100Ω → GND (3dB, 클리핑 감소)
- VIN: 16V 470µF + 22Ω 직렬 (전원 공진 노이즈 제거)
- 스피커: 4Ω 3W (→ v1.1.0에서 교체 예정)

---

[Unreleased]: https://github.com/pilos011/ESP32-Network-Radio/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/pilos011/ESP32-Network-Radio/releases/tag/v1.0.0
