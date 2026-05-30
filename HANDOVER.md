# Silver Line 1호 — ESP32-S3 인터넷 라디오 완전 핸드오버

> **최종 업데이트**: 2026-05-30  
> **프로젝트 상태**: 안정 운용 중 (v1.0.0). MQTT/HA 통합 완료. 프록시 서버 v5 (단일 스트림 설계).  
> **GitHub**: https://github.com/pilos011/ESP32-Network-Radio

---

## 0. 다음 세션이 먼저 읽을 것 — 사용자 스타일

- **한국어로 대화**, 반말 OK.
- **전체 파일 교체** 선호 (부분 diff 실수 유발). 수정 파일은 통째로 zip 제공.
- **다운로드 가능한 zip** 으로 수정 파일 묶어서 `present_files` 로 전달.
- **최소 수정 원칙**. 큰 변경엔 사용자 동의 필요.
- **실용주의** — 이론 설명보다 작동하는 코드 우선. 변경 이유 1~2줄 명시.
- **단계적 진단**: 가설 1~3개 → 시리얼 로그/raw 데이터 → 정확한 수정.
- **빌드는 사용자가** PlatformIO로 직접 함. AI는 코드만 만들고 zip 제공.

---

## 1. 프로젝트 개요

**ESP32-S3-WROOM-1-N16R8** (16MB Flash, 8MB Octal PSRAM) 기반 인터넷 라디오.  
이름: **Silver Line 1호**

### 시스템 아키텍처

```
ESP32-S3 (스피커 + ST7789V TFT + 버튼 2개)
    ↕ HTTP GET (MP3 stream)
Windows 11 PC — Node.js radio-proxy (NSSM 서비스)
    ├→ resolveSource(): .pls/.m3u → 실제 stream URL (5분 캐시)
    └→ ffmpeg: HLS/MP3 입력 → +GAIN_DB 게인 + alimiter → libmp3lame 128k MP3
Home Assistant (같은 네트워크)
    └→ MQTT 브로커 (Mosquitto) ↔ ESP32 ↔ HA custom component (media_player)
```

한국 라디오 HLS 방송국은 ESP32-audioI2S 3.0.12가 직접 재생 못함 → PC 프록시가 MP3로 변환.  
방송국 원본은 `serpent0.duckdns.org:8088/*.pls` 외부 프록시에서 가져옴 (이중 프록시 구조).

---

## 2. 하드웨어

### 핀 배치 (`src/pins_config.h`)

| 부품 | 신호 | GPIO | 비고 |
|------|------|------|------|
| **MAX98357A 앰프** | BCLK | **4** | I2S_NUM_0 |
| | LRC | **5** | I2S_NUM_0 |
| | DIN | **6** | I2S_NUM_0 |
| | SD | 3.3V 직결 | ⚠️ GPIO 아님. always-on + RIGHT 채널 |
| | GAIN | GND (100Ω) | ★ 3dB 고정 (기본 floating=9dB 대비 클리핑 감소) |
| | VIN | 5V (VBUS) | ⚠️ 반드시 5V |
| **ST7789V TFT** | CS | **10** | SPI |
| | MOSI | **11** | |
| | SCLK | **12** | |
| | DC | **18** | |
| | RST | **38** | |
| | BL | VCC 직결 | PCB 내부 연결 (소프트웨어 제어 불가) |
| **INMP441 마이크** | SCK | **15** | I2S_NUM_1 ★ NUM_0는 앰프가 점유 |
| | WS | **16** | |
| | SD | **17** | |
| | L/R | GND | LEFT 채널 |
| **버튼** | LEFT | **1** | INPUT_PULLUP |
| | RIGHT | **2** | INPUT_PULLUP |
| **LED** | STATUS | **41** | 220Ω → GND |

### 금지 GPIO

| 핀 | 이유 |
|----|------|
| 33~37 | Octal PSRAM (N16R8) 전용 |
| 0 | BOOT 모드 |
| 19/20 | USB JTAG |
| 43/44 | UART0 (시리얼 모니터) |

### 하드웨어 수정 이력

| 날짜 | 수정 | 효과 |
|------|------|------|
| 2026-05 | GAIN 핀 → 100Ω → GND | 앰프 게인 9dB → 3dB, 클리핑 여유 +6dB |
| 2026-05 | VIN에 16V 470µF + 22Ω 직렬 | 전원 공진 노이즈(삐이이) 제거, supply droop 흡수 |

**스피커**: 4Ω 3W로는 풀파워에서 물리적 클리핑 발생. **4Ω 5W 이상** 권장.

---

## 3. 버튼 동작 (`src/ButtonControl.h/cpp`)

2단계 롱프레스 (뗄 때 판별):

| 버튼 | 시간 | 시계 모드 | 라디오 모드 |
|------|------|-----------|------------|
| RIGHT | < 1s | — | 볼륨 +1 |
| LEFT | < 1s | — | 볼륨 -1 |
| RIGHT | 1~2s | — | 다음 방송국 |
| LEFT | 1~2s | — | 이전 방송국 |
| RIGHT | ≥ 2s | 라디오 ON | 시계 모드 전환 |
| LEFT | ≥ 2s | 라디오 ON | 시계 모드 전환 |
| 양쪽 동시 | ≥ 10s | AP 설정 모드 | AP 설정 모드 |

> **설계 이유**: LEFT/RIGHT LONG 둘 다 시계↔라디오 전환으로 통일. AP 진입은 양쪽 동시 10s만.

---

## 4. 빌드 환경 (`platformio.ini`)

```ini
[env:esp32-s3-n16r8]
platform = espressif32@^6.6.0     ; Arduino-ESP32 2.0.14 (ESP-IDF 4.4)
board    = esp32-s3-devkitc-1
framework = arduino

board_build.arduino.memory_type = dio_opi   ; flash=dio, psram=octal
board_build.flash_mode          = dio
board_build.partitions          = partitions_16MB.csv
board_upload.flash_size         = 16MB

build_unflags = -std=gnu++11
build_flags =
  -DARDUINO_USB_CDC_ON_BOOT=0
  -DARDUINO_USB_MODE=1
  -DCORE_DEBUG_LEVEL=2
  -DBOARD_HAS_PSRAM
  -DU8G2_USE_LARGE_FONTS=1
  -std=gnu++17

lib_deps =
  https://github.com/schreibfaul1/ESP32-audioI2S.git#3.0.12
  adafruit/Adafruit ST7735 and ST7789 Library @ ^1.10.4
  adafruit/Adafruit GFX Library @ ^1.11.5
  adafruit/Adafruit BusIO @ ^1.16.1
  olikraus/U8g2_for_Adafruit_GFX @ ^1.8.0
  knolleary/PubSubClient@^2.8      ; MQTT (HA 통합)
```

**주의**: ESP32-audioI2S는 `#3.0.12` 고정. 3.1+은 ESP-IDF 5.x 필요.

---

## 5. 소스 파일 구조

```
src/
├── main.cpp                  setup() + loop() + 버튼 핸들러 + MQTT 통합
├── pins_config.h             핀 배치
├── credentials.h             WiFi SSID/PW, RADIO_PROXY IP (gitignore)
├── stations.h                첫 부팅 기본 방송국 목록
├── Config.h/.cpp             NVS 설정 저장/로드 (MQTT 설정 포함)
├── WebUI.h/.cpp              HTTP 설정 페이지 + Captive Portal
├── DisplayUI.h/.cpp          ST7789 + u8g2 한글
├── StreamPlayer.h/.cpp       ESP32-audioI2S 래퍼
├── RadioStationManager.h/.cpp
├── ButtonControl.h/.cpp      2단계 롱프레스 상태머신
└── fonts/d2coding24.h        634KB D2Coding 24px 한글 폰트

radio-proxy/
├── server.js                 HLS→MP3 프록시 v5 (단일 스트림 설계)
├── config.ini                로그/포트/게인 설정 ★
├── stations.json             방송국 목록
├── chime.mp3                 정각 시보
└── package.json

ha-custom-component/          (별도 zip 배포)
silverline_radio/
├── __init__.py
├── manifest.json
└── media_player.py           HA custom component
```

---

## 6. Config 시스템 — NVS + WebUI

### MQTT 설정 필드 (Config.h)

```cpp
String   mqttBroker   = "";          // 비워두면 MQTT 비활성
uint16_t mqttPort     = 1883;
String   mqttUser     = "";
String   mqttPass     = "";
String   mqttDeviceId = "silverline1"; // MQTT 토픽 prefix
```

WebUI에서 설정 가능. 저장 후 재부팅하면 MQTT에 자동 연결.

### 새 Config 필드 추가 패턴 (3곳)
1. `Config.h` 필드 선언
2. `Config.cpp` loadFromNvs / saveToNvs / printAll
3. `WebUI.cpp` HTML 폼 + substitute + handleSave 파싱

---

## 7. Radio-Proxy 서버 (`radio-proxy/server.js`)

### v5 핵심: 단일 스트림 설계

이전 버전들(v1~v4)은 `Map` 기반으로 복잡한 상태 관리 → 경쟁 조건 + 좀비 ffmpeg 누적.  
v5는 전역 단일 `S` 객체 하나만 유지:

```js
const S = {
  ver: 0,        // teardown 시 증가 — 구버전 콜백 무효화
  ff: null,      // 현재 ffmpeg 프로세스
  res: null,     // 현재 HTTP 응답
  stallTimer: null,
};

function teardown(reason) {
  S.ver++;
  // 1. 현재 ffmpeg PID 기반 kill
  // 2. Windows: taskkill /IM ffmpeg.exe (이름으로 일괄 종료)
  // 3. res.end() 호출
}
```

**Windows에서 `child.kill()` 이 네트워크 I/O 중인 ffmpeg에서 실패하는 경우가 있음**  
→ `taskkill /F /IM ffmpeg.exe` 로 이름 기반 일괄 종료가 핵심 해결책.

### config.ini

```ini
LOG_ENABLED=true    # false 로 설정하면 로그 I/O/CPU 절약
PORT=8088
GAIN_DB=6
LIMIT=0.85
```

환경변수(NSSM)가 있으면 환경변수가 우선.

### NSSM 서비스 설정

```cmd
nssm install ClaudeRadio
; Path:              node.exe
; Arguments:         server.js
; Startup directory: C:\...\radio-proxy
; Environment:       GAIN_DB=6  LIMIT=0.85
nssm start ClaudeRadio
```

### ffmpeg 파라미터

```
-reconnect 1 -reconnect_streamed 1 -reconnect_on_network_error 1
-rw_timeout 15000000
-af volume=${GAIN_DB}dB,alimiter=limit=${LIMIT}:attack=5:release=50
-c:a libmp3lame -b:a 128k -ar 44100 -ac 2
-write_xing 0 -id3v2_version 0 -flush_packets 1
```

### 엔드포인트

| URL | 동작 |
|-----|------|
| `GET /` | 방송국 목록 JSON |
| `GET /:id` | MP3 스트림 |
| `GET /health` | `ok pid=NNN ver=N id=station` |
| `GET /chime` | chime.mp3 반환 |

---

## 8. MQTT / Home Assistant 통합

### ESP32 → MQTT 토픽

| 방향 | 토픽 | 내용 |
|------|------|------|
| 발행 | `silverline1/state` | `{"power":"ON","state":"playing","station":"MBC FM","volume":6,"volume_pct":0.545,"max_volume":11}` |
| 발행 | `silverline1/availability` | `online` / `offline` (LWT) |
| 수신 | `silverline1/cmd/power` | `ON` / `OFF` |
| 수신 | `silverline1/cmd/volume` | `0~11` 정수 |
| 수신 | `silverline1/cmd/volume_pct` | `0.0~1.0` 소수 |
| 수신 | `silverline1/cmd/station` | 방송국명 / `next` / `prev` |
| 수신 | `silverline1/cmd/control` | `play` / `stop` (media_player용) |

### MQTT Discovery (4개 엔티티, HA 자동 등록)

ESP32 연결 시 `homeassistant/{type}/silverline1_*/config` 에 retained 발행:
- `switch.silverline1_power` — 전원
- `select.silverline1_source` — 방송국 선택
- `number.silverline1_volume` — 볼륨 (0~11)
- `sensor.silverline1_station` — 현재 방송국명

> **주의**: HA 2024.7 기준 MQTT discovery는 `media_player` 타입 미지원  
> → 별도 custom component 사용 (섹션 9 참조)

### ESP32 WebUI MQTT 설정

HA Mosquitto 브로커 IP를 입력 후 저장/재부팅 → 자동 연결 + discovery 발행.

---

## 9. HA Custom Component — `silverline_radio`

HA 2024.7에서 `media_player: platform: template` / `platform: mqtt` 모두 제거됨.  
→ `custom_components/silverline_radio/` 직접 작성.

### 설치 경로

```
/config/custom_components/silverline_radio/
├── __init__.py
├── manifest.json
└── media_player.py
```

`configuration.yaml` 에 추가:
```yaml
media_player:
  - platform: silverline_radio
```

### 생성되는 엔티티

`media_player.silver_line_1ho` (이름 "Silver Line 1호" → HA 슬러그화)

### 지원 기능

| 기능 | 동작 |
|------|------|
| ▶ Play | 라디오 ON (IDLE 상태에서 표시) |
| ⏸ Pause | 라디오 OFF → 시계 모드 |
| ⏹ Stop | 라디오 OFF → 시계 모드 |
| 볼륨 슬라이더 | 0.0~1.0 → 0~11 변환 |
| 소스 선택 | 방송국 드롭다운 |
| ⏮⏭ 이전/다음 | 채널 이동 |

### 상태 매핑

| ESP32 상태 | HA 상태 | 위젯 표시 |
|-----------|---------|---------|
| power=ON | `PLAYING` | ⏸ pause 버튼 |
| power=OFF | `IDLE` | ▶ play 버튼 |

> **중요**: `OFF` 대신 `IDLE` 사용. `OFF` 상태에서는 위젯에 power 버튼만 표시됨.

### 크로스-폰 실시간 동기화

`cmd/power`, `cmd/volume`, `cmd/station` 토픽을 **모두 구독** → 다른 폰에서 명령 시 즉시 반영:

```python
# 다른 폰이 보낸 power 명령 → 즉시 상태 반영 (ESP32 응답 전)
@callback
def _on_cmd_power(self, msg): ...

# 다른 폰이 보낸 station 명령 → 즉시 source 반영
@callback
def _on_cmd_station(self, msg): ...
```

next/prev는 결과를 모르므로 ESP32 state 응답으로 처리.

### 낙관적 업데이트

```python
async def async_media_play(self):
    self._state = MediaPlayerState.PLAYING   # 즉시 UI 반영
    self.async_write_ha_state()
    await mqtt.async_publish(...)            # 그 후 MQTT 전송
```

---

## 10. 안정성 메커니즘

### 주요 가드 (loop)

```cpp
esp_task_wdt_reset();         // TWDT 갱신 (15초 미갱신 시 panic 재부팅)
if (heap < 25000) restart;    // 힙 부족 재부팅
if (millis > 86400000) restart; // 24h 정기 재부팅
```

### WiFi 재연결

```cpp
WiFi.disconnect(true);   // 내부 상태까지 비움
delay(200);
WiFi.mode(WIFI_STA);
WiFi.setSleep(false);
WiFi.begin(ssid, pass);
// 3회 실패 시 ESP.restart()
```

### Stream Watchdog

8초 무음 감지 → 재연결. 10회 누적 실패 → `ESP.restart()`.

### MQTT 재연결

MQTT 연결 끊기면 30초마다 재연결 시도. 재연결 시 discovery + state 재발행.

---

## 11. 정각 시보

- 매 시 HH:59:50에 트리거 (정각 10초 전)
- DND 시간 설정 가능 (기본 23:00~08:00)
- `/chime` 엔드포인트에서 MP3 스트림
- 라디오 모드: 현재 URL 저장 → chime → 자동 복귀

---

## 12. 빌드 / 업로드

```cmd
cd C:\esp32-s3-radio
pio run               ; 빌드
pio run -t upload     ; 빌드 + 업로드
pio device monitor    ; 시리얼 모니터 115200
```

**첫 빌드 전**: `src/credentials.h` 에 WiFi SSID/PW, RADIO_PROXY IP 설정.

---

## 13. AP 모드 진입

| 방법 | 조건 |
|------|------|
| 자동 | 첫 부팅 (credentials.h placeholder) |
| 자동 | WiFi 30초 연결 실패 |
| 수동 | 양쪽 버튼 동시 10초 |
| 수동 | WebUI Factory Reset 버튼 |

AP SSID: `ClaudeRadio-Setup-XXXX` / 암호 없음 / IP: `192.168.4.1`

---

## 14. 트러블슈팅

| 증상 | 원인 / 해결 |
|------|------------|
| ffmpeg 쌓임 | server.js v5 확인. `taskkill /IM ffmpeg.exe` 동작 확인 |
| 삐이이 소리 | 캐패시터 공진 → VIN에 22Ω 직렬 저항 추가 |
| 지지직 클리핑 | GAIN 핀이 GND 연결됐는지 확인. 스피커 4Ω 5W 이상 필요 |
| HA 위젯 play 버튼이 pause 표시 | MQTT state 업데이트 미수신 → ESP32 MQTT 연결 확인 |
| HA 다른 폰 변경이 반영 안 됨 | custom component 최신버전 확인 (cmd 토픽 구독 포함) |
| `Integration media_player is not supported` 로그 | 정상. ESP32 discovery는 HA에서 무시됨 (custom component 사용 중) |
| INMP441 마이크 추가 시 | I2S_NUM_1 사용 (NUM_0는 앰프). GPIO 15/16/17 사용 가능 |
| `byte* is ambiguous` 컴파일 에러 | `byte*` → `uint8_t*` 로 변경 (C++17 std::byte 충돌) |

---

## 15. NSSM 관리

```cmd
nssm restart ClaudeRadio
nssm status ClaudeRadio
nssm set ClaudeRadio AppEnvironmentExtra GAIN_DB=6 LIMIT=0.85
```

로그 끄려면 `config.ini` 에서 `LOG_ENABLED=false`.

---

## 16. 현재 상태 (2026-05-27)

### ✅ 완료

- WiFi STA + AP 폴백 + Captive Portal
- NVS 설정 저장/로드 + Factory Reset + WebUI
- NTP 시계 (KST)
- D2Coding 24px 한글 폰트 (634KB PROGMEM)
- 듀얼 코어 audio task 분리
- PSRAM 1MB 입력 버퍼
- Stream watchdog + TWDT 15초 자동 회복
- 버튼: 2단계 롱프레스 (1s=채널변경, 2s=모드전환)
- 정각 시보 (DND 포함)
- **MQTT/HA 통합**: 4개 discovery 엔티티 + media_player custom component
- **크로스-폰 실시간 동기화** (cmd 토픽 구독)
- **Radio-Proxy v5**: 단일 스트림 설계, Windows `taskkill /IM` 신뢰성 확보
- **config.ini**: 로그 on/off, 포트, 게인 설정
- **하드웨어**: GAIN 3dB 고정, 470µF 디커플링 캐패시터

### 🔲 미완료

- INMP441 마이크 연동 (핀 준비됨, 코드 미구현)
- 스피커 교체 (현재 4Ω 3W → 4Ω 5W 이상 필요)

---

## 17. 재사용 가능한 핵심 패턴

| 패턴 | 위치 |
|------|------|
| Config + NVS + WebUI 3곳 규칙 | 섹션 6 |
| Captive Portal 첫 부팅 셋업 | main.cpp AP 분기 |
| 단일 스트림 Node.js proxy | radio-proxy/server.js |
| Windows ffmpeg 강제 종료 | `taskkill /F /IM ffmpeg.exe` |
| MQTT discovery + HA custom component | 섹션 8, 9 |
| TWDT 행업 자동 회복 | 섹션 10 |
| 크로스-폰 실시간 동기화 (cmd 구독) | 섹션 9 |

---

**End of Handover**
