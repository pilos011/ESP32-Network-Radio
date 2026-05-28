"""Silver Line 1 Internet Radio — HA Media Player custom component."""
from __future__ import annotations

import json
import logging
from typing import Any

import voluptuous as vol

from homeassistant.components import mqtt
from homeassistant.components.media_player import (
    PLATFORM_SCHEMA as MEDIA_PLAYER_SCHEMA,
    MediaPlayerEntity,
    MediaPlayerEntityFeature,
    MediaPlayerState,
)
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.typing import ConfigType, DiscoveryInfoType

_LOGGER = logging.getLogger(__name__)

STATE_TOPIC = "silverline1/state"
AVAIL_TOPIC = "silverline1/availability"
CMD_POWER   = "silverline1/cmd/power"
CMD_VOLUME  = "silverline1/cmd/volume"
CMD_STATION = "silverline1/cmd/station"

CONF_SOURCES = "sources"

# sources 생략 시 사용되는 기본 목록 (stations.h / stations.json 기준)
DEFAULT_SOURCES: list[str] = [
    "MBC 표준 FM",
    "MBC FM4U",
    "CBS 표준 FM",
    "CBS Music FM",
    "SBS 파워 FM",
    "SBS 러브 FM",
    "KBS 제1라디오",
    "KBS 쿨FM",
    "KBS 클래식FM",
    "KBS 해피FM",
    "NPR News",
]

# configuration.yaml 스키마
# 예시:
#   media_player:
#     - platform: silverline_radio
#       sources:
#         - "MBC 표준 FM"
#         - "MBC FM4U"
PLATFORM_SCHEMA = MEDIA_PLAYER_SCHEMA.extend({
    vol.Optional(CONF_SOURCES, default=DEFAULT_SOURCES): vol.All(
        cv.ensure_list, [cv.string]
    ),
})

SUPPORT_FLAGS = (
    MediaPlayerEntityFeature.TURN_ON
    | MediaPlayerEntityFeature.TURN_OFF
    | MediaPlayerEntityFeature.PLAY        # IDLE 상태 → ▶ 버튼
    | MediaPlayerEntityFeature.PAUSE       # PLAYING 상태 → ⏸ 버튼
    | MediaPlayerEntityFeature.STOP
    | MediaPlayerEntityFeature.VOLUME_SET
    | MediaPlayerEntityFeature.VOLUME_STEP
    | MediaPlayerEntityFeature.SELECT_SOURCE
    | MediaPlayerEntityFeature.NEXT_TRACK
    | MediaPlayerEntityFeature.PREVIOUS_TRACK
)


async def async_setup_platform(
    hass: HomeAssistant,
    config: ConfigType,
    async_add_entities: AddEntitiesCallback,
    discovery_info: DiscoveryInfoType | None = None,
) -> None:
    sources: list[str] = config.get(CONF_SOURCES, DEFAULT_SOURCES)
    async_add_entities([SilverLineRadio(hass, sources)])


class SilverLineRadio(MediaPlayerEntity):
    _attr_unique_id          = "silverline1_mediaplayer"
    _attr_name               = "Silver Line 1호"
    _attr_should_poll        = False
    _attr_supported_features = SUPPORT_FLAGS

    def __init__(self, hass: HomeAssistant, sources: list[str]) -> None:
        self.hass             = hass
        self._source_list     = sources
        self._state           = MediaPlayerState.IDLE
        self._volume          = 0.5
        self._max_vol         = 11
        self._source: str | None = None
        self._unsubs: list[Any] = []

    @property
    def source_list(self) -> list[str]:
        return self._source_list

    async def async_added_to_hass(self) -> None:
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, STATE_TOPIC, self._on_state, qos=1
            )
        )
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, AVAIL_TOPIC, self._on_availability, qos=1
            )
        )
        # 크로스-폰 실시간 동기화: cmd 토픽 구독
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, CMD_POWER, self._on_cmd_power, qos=1
            )
        )
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, CMD_VOLUME, self._on_cmd_volume, qos=1
            )
        )
        self._unsubs.append(
            await mqtt.async_subscribe(
                self.hass, CMD_STATION, self._on_cmd_station, qos=1
            )
        )

    async def async_will_remove_from_hass(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()

    @callback
    def _on_availability(self, msg: Any) -> None:
        """ESP32 재연결 시 시계모드(IDLE)로 초기화."""
        avail = str(msg.payload).strip()
        _LOGGER.debug("silverline availability: %s", avail)
        if avail == "online":
            self._state = MediaPlayerState.IDLE
            self.async_write_ha_state()

    @callback
    def _on_cmd_power(self, msg: Any) -> None:
        cmd = str(msg.payload).strip()
        if cmd == "ON":
            self._state = MediaPlayerState.PLAYING
        elif cmd == "OFF":
            self._state = MediaPlayerState.IDLE
        self.async_write_ha_state()

    @callback
    def _on_cmd_volume(self, msg: Any) -> None:
        try:
            vol = int(str(msg.payload).strip())
            self._volume = round(vol / self._max_vol, 3)
            self.async_write_ha_state()
        except (ValueError, TypeError):
            pass

    @callback
    def _on_cmd_station(self, msg: Any) -> None:
        cmd = str(msg.payload).strip()
        if cmd in ("next", "prev"):
            return
        if cmd:
            self._source = cmd
            self.async_write_ha_state()

    @callback
    def _on_state(self, msg: Any) -> None:
        try:
            d = json.loads(msg.payload)
        except (json.JSONDecodeError, TypeError):
            _LOGGER.warning("silverline: bad payload: %s", msg.payload)
            return

        _LOGGER.debug("silverline state: %s", d)

        state_str = d.get("state") or ("playing" if d.get("power") == "ON" else "off")
        self._state = (
            MediaPlayerState.PLAYING
            if state_str == "playing"
            else MediaPlayerState.IDLE
        )

        max_v = max(int(d.get("max_volume", 11)), 1)
        self._max_vol = max_v
        self._volume  = round(int(d.get("volume", 0)) / max_v, 3)

        station = d.get("station") or None
        if station:
            self._source = station

        self.async_write_ha_state()

    # ── 상태 Properties ───────────────────────────────────────────────

    @property
    def state(self) -> MediaPlayerState:
        return self._state

    @property
    def volume_level(self) -> float:
        return self._volume

    @property
    def source(self) -> str | None:
        return self._source

    @property
    def media_title(self) -> str | None:
        return self._source

    # ── 전원 ─────────────────────────────────────────────────────────

    async def async_turn_on(self) -> None:
        self._state = MediaPlayerState.PLAYING
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_POWER, "ON")

    async def async_turn_off(self) -> None:
        self._state = MediaPlayerState.IDLE
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_POWER, "OFF")

    async def async_media_play(self) -> None:
        self._state = MediaPlayerState.PLAYING
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_POWER, "ON")

    async def async_media_pause(self) -> None:
        self._state = MediaPlayerState.IDLE
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_POWER, "OFF")

    async def async_media_stop(self) -> None:
        self._state = MediaPlayerState.IDLE
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_POWER, "OFF")

    # ── 볼륨 ─────────────────────────────────────────────────────────

    async def async_set_volume_level(self, volume: float) -> None:
        await mqtt.async_publish(
            self.hass, CMD_VOLUME, str(round(volume * self._max_vol))
        )

    async def async_volume_up(self) -> None:
        level = min(self._max_vol, round(self._volume * self._max_vol) + 1)
        await mqtt.async_publish(self.hass, CMD_VOLUME, str(level))

    async def async_volume_down(self) -> None:
        level = max(0, round(self._volume * self._max_vol) - 1)
        await mqtt.async_publish(self.hass, CMD_VOLUME, str(level))

    # ── 채널 ─────────────────────────────────────────────────────────

    async def async_select_source(self, source: str) -> None:
        self._source = source
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_STATION, source)

    async def async_media_next_track(self) -> None:
        self._source = self._adjacent_source(+1)
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_STATION, "next")

    async def async_media_previous_track(self) -> None:
        self._source = self._adjacent_source(-1)
        self.async_write_ha_state()
        await mqtt.async_publish(self.hass, CMD_STATION, "prev")

    def _adjacent_source(self, delta: int) -> str | None:
        if not self._source:
            return self._source_list[0] if self._source_list else None
        try:
            idx = self._source_list.index(self._source)
            return self._source_list[(idx + delta) % len(self._source_list)]
        except ValueError:
            return self._source
