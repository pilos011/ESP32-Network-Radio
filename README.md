# Silver Line 1호 — ESP32-S3 인터넷 라디오

> 📄 **전체 문서는 [HANDOVER.md](./HANDOVER.md) 를 참조하세요.**

핀 배치, 빌드 방법, Radio-Proxy, MQTT/HA 통합, 버튼 동작, 트러블슈팅 등  
모든 상세 내용이 HANDOVER.md 에 있습니다.

## 빠른 시작

```cmd
; 1. credentials.h 에 WiFi SSID/PW 설정
; 2. 빌드 & 업로드
pio run -t upload
pio device monitor

; 3. 프록시 서버 시작 (HLS 방송국 청취 시)
cd radio-proxy && node server.js
```

→ [HANDOVER.md](./HANDOVER.md)
