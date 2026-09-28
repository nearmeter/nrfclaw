"""Application/NDP v1 client used by short-lived Direct-NDP sessions."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field, replace
import hashlib
import hmac
import logging
from typing import Any

from bleak import BleakClient
from bleak_retry_connector import BleakClientWithServiceCache, establish_connection

from .const import (
    APP_SERVICE_UUID,
    BOARD_NAMES,
    CAP_ACCEL,
    CAP_BATTERY,
    CAP_DS18B20,
    CAP_TEMPERATURE,
    CAP_HALL,
    CAP_VIB_AUTO,
    NDP_ACCESS_CONTROL,
    NDP_ACCESS_STATUS,
    NDP_AUTH_BEGIN,
    NDP_AUTH_FINISH,
    NDP_BLE_DISCONNECT_HINT,
    NDP_DIRECT_SENSOR_CONTROL,
    NDP_DIRECT_EVENT_SENSITIVITY,
    NDP_DIRECT_HALL_CONTROL,
    NDP_DIRECT_SEMANTIC_STATE,
    NDP_BOARD_INFO,
    NDP_BOARD_RESET,
    NDP_BUSY,
    NDP_CAPS,
    NDP_INFO,
    NDP_OK,
    NDP_SENSOR_READ,
    NDP_STATUS,
    NDP_UNAUTHORIZED,
    NDP_VIB_AUTO_START,
    NDP_VIB_AUTO_STOP,
    NDP_VIB_AUTO_STATUS,
    NDP_VIB_AUTO_CONFIG,
    NDP_VIB_AUTO_RESET,
    RX_UUID,
    SENSOR_ACCEL_METRICS,
    SENSOR_BATTERY,
    SENSOR_DS18B20,
    SENSOR_TEMPERATURE,
    SENSOR_HALL,
    TX_UUID,
)
from .protocol import NDPProtocolError, decode_access_key, decode_response, encode_request
from .direct_adv import DirectAdvSemantic, parse_semantic_state_record

_LOGGER = logging.getLogger(__name__)


class NrfClawGattLayoutError(ConnectionError):
    """Required Application/NDP GATT service or characteristic is missing."""


@dataclass(slots=True)
class NrfClawData:
    """One coherent Direct-NDP device snapshot."""

    # Static / infrequently refreshed identity and supported-capability data.
    ndp_version: int | None = None
    vm_abi: int | None = None
    board_type: int | None = None
    board_name: str | None = None
    hw_rev: str | None = None
    firmware: str | None = None
    firmware_build: int | None = None
    device_id: str | None = None
    git32: int | None = None
    capabilities: int = 0
    reset_reason: int | None = None

    # Runtime status. active_capabilities comes from NDP_STATUS selector 1.
    active_capabilities: int | None = None
    hall_mode: int = 0
    hall_channel: int = 1
    ds18b20_present: bool = False
    temperature_source: int | None = None
    accel_event_mode: int | None = None
    accel_event_persisted_mode: int | None = None
    accel_event_control_writable: bool = False
    accel_event_sensitivity: int | None = None
    accel_event_sensitivity_persisted: bool = False
    accel_event_sensitivity_writable: bool = False
    hall_control_persisted_mode: int | None = None
    hall_control_persisted_channel: int | None = None
    hall_control_writable: bool = False
    vib_auto_enabled: bool | None = None
    vib_auto_state: int | None = None
    vib_auto_profile_count: int | None = None
    vib_auto_discovery_complete: bool = False
    vib_auto_control_writable: bool = False
    vib_auto_sensitivity: int | None = None
    vib_auto_sensitivity_writable: bool = False
    semantic_total_reset_writable: bool = False

    # Runtime telemetry.
    acceleration_x_mg: int | None = None
    acceleration_y_mg: int | None = None
    acceleration_z_mg: int | None = None
    battery_v: float | None = None
    hall_value: int | None = None
    temperature_c: float | None = None
    vibration_rms_mg: int | None = None
    vibration_peak_mg: int | None = None
    vibration_peak_to_peak_mg: int | None = None
    vibration_zero_cross_hz: int | None = None
    semantic_values: dict[tuple[int, int], DirectAdvSemantic] = field(default_factory=dict)

    def has_capability(self, capability: int) -> bool:
        return bool(self.capabilities & (1 << (capability - 1)))

    def capability_active(self, capability: int) -> bool | None:
        if self.active_capabilities is None:
            return None
        return bool(self.active_capabilities & (1 << (capability - 1)))


class NrfClawClient:
    """Own one Application/NDP GATT connection for one Direct snapshot."""

    def __init__(self, address: str, access_key: str | None = None) -> None:
        self.address = address
        self.access_key = decode_access_key(access_key)
        self._client: BleakClient | None = None
        self._response_queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._sequence = 0
        self._request_lock = asyncio.Lock()
        self._notify_started = False
        self._authenticated = False
        self._gatt_layout_validated = False
        self._last_info_payload: bytes | None = None

    @property
    def is_connected(self) -> bool:
        return bool(self._client is not None and self._client.is_connected)

    def invalidate_gatt_cache(self) -> None:
        """Force the next Direct session to rediscover the GATT layout."""
        self._gatt_layout_validated = False

    def _notify(self, _sender: Any, data: bytearray) -> None:
        self._response_queue.put_nowait(bytes(data))

    def _next_sequence(self) -> int:
        self._sequence = (self._sequence + 1) & 0xFF
        return self._sequence

    async def async_connect(self, ble_device: Any, name: str) -> None:
        """Connect; after one clean discovery reuse the stable GATT service cache."""
        if self.is_connected:
            return

        await self.async_disconnect()
        self._response_queue = asyncio.Queue()
        self._authenticated = False
        self._last_info_payload = None

        client = await establish_connection(
            BleakClientWithServiceCache,
            ble_device,
            name,
            use_services_cache=self._gatt_layout_validated,
        )
        self._client = client

        try:
            services = client.services
            if services.get_service(APP_SERVICE_UUID) is None:
                raise NrfClawGattLayoutError(
                    f"Application service {APP_SERVICE_UUID} was not found"
                )
            if services.get_characteristic(RX_UUID) is None:
                raise NrfClawGattLayoutError(
                    f"Application RX characteristic {RX_UUID} was not found"
                )
            if services.get_characteristic(TX_UUID) is None:
                raise NrfClawGattLayoutError(
                    f"Application TX characteristic {TX_UUID} was not found"
                )

            self._gatt_layout_validated = True
            await client.start_notify(TX_UUID, self._notify)
            self._notify_started = True
            await self._async_ensure_access()
        except BaseException:
            await self.async_disconnect()
            raise

    async def async_disconnect(self) -> None:
        """Release the link without a redundant stop-notify GATT transaction."""
        client = self._client
        self._client = None
        self._authenticated = False
        self._notify_started = False

        if client is None:
            return

        try:
            if client.is_connected:
                # Notifications are connection scoped; link teardown removes the
                # CCCD subscription. Avoiding stop_notify saves one Direct GATT op.
                await client.disconnect()
        finally:
            # Allow BlueZ/SoftDevice to publish DISCONNECTED before another node.
            await asyncio.sleep(0.20)

    async def _request(
        self,
        opcode: int,
        payload: bytes = b"",
        *,
        timeout: float = 5.0,
    ) -> tuple[int, bytes]:
        client = self._client
        if client is None or not client.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")

        async with self._request_lock:
            sequence = self._next_sequence()
            frame = encode_request(opcode, sequence, payload)
            await client.write_gatt_char(RX_UUID, frame, response=False)

            deadline = asyncio.get_running_loop().time() + timeout
            while True:
                remaining = deadline - asyncio.get_running_loop().time()
                if remaining <= 0:
                    raise TimeoutError(
                        f"timeout waiting for NDP opcode=0x{opcode:02X} seq={sequence}"
                    )

                raw = await asyncio.wait_for(self._response_queue.get(), remaining)
                try:
                    response = decode_response(raw)
                except NDPProtocolError:
                    _LOGGER.debug("Ignoring malformed NDP notification from %s", self.address)
                    continue

                if response.opcode != opcode or response.sequence != sequence:
                    _LOGGER.debug(
                        "Ignoring stale NDP response from %s: op=0x%02X seq=%u",
                        self.address,
                        response.opcode,
                        response.sequence,
                    )
                    continue

                return response.status, response.payload

    async def _async_ensure_access(self) -> None:
        """Authenticate CONTROL when owner protection is enabled."""
        if self._authenticated:
            return

        status, payload = await self._request(NDP_INFO)
        if status == NDP_OK:
            self._last_info_payload = payload
            self._authenticated = True
            return

        if status != NDP_UNAUTHORIZED:
            raise PermissionError(f"NDP access probe failed with status {status}")
        if self.access_key is None:
            raise PermissionError(
                "NDP owner protection is enabled but no access key was configured"
            )

        status, payload = await self._request(NDP_ACCESS_STATUS)
        if status != NDP_OK or len(payload) != 1:
            raise PermissionError("unable to read NDP access status")
        if payload[0] >= NDP_ACCESS_CONTROL:
            self._authenticated = True
        else:
            status, payload = await self._request(
                NDP_AUTH_BEGIN,
                bytes((NDP_ACCESS_CONTROL,)),
            )
            if status != NDP_OK or len(payload) != 13:
                raise PermissionError("NDP authentication challenge failed")

            challenge = payload[:12]
            session_id = payload[12]
            tag16 = hmac.new(
                self.access_key,
                challenge + bytes((NDP_ACCESS_CONTROL, session_id)),
                hashlib.sha256,
            ).digest()[:16]
            status, payload = await self._request(NDP_AUTH_FINISH, tag16)
            if status != NDP_OK or len(payload) != 1 or payload[0] < NDP_ACCESS_CONTROL:
                raise PermissionError("invalid nRFClaw NDP access key")
            self._authenticated = True

        # Protected path's initial INFO was rejected; read it once after auth so
        # the first static snapshot does not need a second special access path.
        status, payload = await self._request(NDP_INFO)
        if status == NDP_OK:
            self._last_info_payload = payload

    async def _sensor_read(
        self,
        sensor_id: int,
        *,
        retries: int = 1,
        busy_delay: float = 0.2,
    ) -> tuple[int, bytes]:
        last = (NDP_BUSY, b"")
        for attempt in range(retries):
            last = await self._request(NDP_SENSOR_READ, bytes((sensor_id,)))
            if last[0] != NDP_BUSY:
                return last
            if attempt + 1 < retries:
                await asyncio.sleep(busy_delay)
        return last

    async def async_read_static_snapshot(self) -> NrfClawData:
        """Read identity, SUPPORTED capabilities and static-ish radio profile."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        data = NrfClawData()

        payload = self._last_info_payload
        if payload is None:
            status, payload2 = await self._request(NDP_INFO)
            payload = payload2 if status == NDP_OK else b""
        if len(payload) >= 3:
            data.ndp_version = payload[0]
            data.vm_abi = payload[1]
            data.board_type = payload[2]
            data.board_name = BOARD_NAMES.get(payload[2], f"BOARD-{payload[2]}")

        status, payload = await self._request(NDP_CAPS)
        if status == NDP_OK and len(payload) >= 2:
            data.capabilities = int.from_bytes(payload[:2], "little")

        status, payload = await self._request(NDP_BOARD_INFO, b"\x00")
        if status == NDP_OK and len(payload) == 15:
            data.board_type = payload[0]
            data.board_name = BOARD_NAMES.get(payload[0], f"BOARD-{payload[0]}")
            data.hw_rev = f"{payload[1]}.{payload[2]}"
            prerelease = f"-pre{payload[6]}" if payload[6] else ""
            data.firmware = f"{payload[3]}.{payload[4]}.{payload[5]}{prerelease}"
            data.firmware_build = int.from_bytes(payload[7:9], "little")
            data.ndp_version = payload[9]
            data.vm_abi = payload[10]

        status, payload = await self._request(NDP_BOARD_INFO, b"\x01")
        if status == NDP_OK and len(payload) == 8:
            d0 = int.from_bytes(payload[0:4], "little")
            d1 = int.from_bytes(payload[4:8], "little")
            data.device_id = f"{d1:08X}{d0:08X}"

        status, payload = await self._request(NDP_BOARD_INFO, b"\x02")
        if status == NDP_OK and len(payload) >= 4:
            data.git32 = int.from_bytes(payload[:4], "little")

        status, payload = await self._request(NDP_BOARD_RESET)
        if status == NDP_OK and len(payload) >= 4:
            data.reset_reason = int.from_bytes(payload[:4], "little")


        return data

    async def async_read_runtime_snapshot(
        self,
        base: NrfClawData,
        *,
        read_battery: bool = True,
    ) -> NrfClawData:
        """Read only runtime state/telemetry; preserve cached static metadata."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        data = replace(base)

        # Do not leak stale dynamic values if a runtime capability is disabled.
        data.active_capabilities = None
        data.accel_event_mode = None
        data.accel_event_control_writable = False
        data.accel_event_sensitivity = None
        data.accel_event_sensitivity_persisted = False
        data.accel_event_sensitivity_writable = False
        data.hall_control_writable = False
        data.vib_auto_enabled = None
        data.vib_auto_state = None
        data.vib_auto_profile_count = None
        data.vib_auto_discovery_complete = False
        data.vib_auto_control_writable = False
        data.vib_auto_sensitivity = None
        data.vib_auto_sensitivity_writable = False
        data.semantic_total_reset_writable = False
        data.acceleration_x_mg = None
        data.acceleration_y_mg = None
        data.acceleration_z_mg = None
        data.hall_value = None
        data.temperature_c = None
        data.vibration_rms_mg = None
        data.vibration_peak_mg = None
        data.vibration_peak_to_peak_mg = None
        data.vibration_zero_cross_hz = None
        data.semantic_values = {}

        status, payload = await self._request(NDP_DIRECT_SEMANTIC_STATE)
        if status == NDP_OK:
            semantic = parse_semantic_state_record(payload)
            if semantic is not None:
                data.semantic_values[(semantic.capability_id, semantic.channel)] = semantic
                control_status, control_payload = await self._request(
                    NDP_DIRECT_SEMANTIC_STATE, b"\x00"
                )
                if (
                    control_status == NDP_OK
                    and len(control_payload) == 3
                    and control_payload[0] == 1
                ):
                    data.semantic_total_reset_writable = bool(control_payload[1])

        status, payload = await self._request(NDP_STATUS)
        if status == NDP_OK and len(payload) >= 2:
            data.hall_mode = payload[0]
            data.ds18b20_present = bool(payload[1])

        status, payload = await self._request(NDP_STATUS, b"\x01")
        if status == NDP_OK and len(payload) >= 2:
            data.active_capabilities = int.from_bytes(payload[:2], "little")

        if data.has_capability(CAP_ACCEL):
            status, payload = await self._request(NDP_DIRECT_SENSOR_CONTROL)
            if status == NDP_OK and len(payload) == 5 and payload[0] == 1:
                data.accel_event_mode = payload[1]
                data.accel_event_persisted_mode = payload[2] if payload[3] else None
                data.accel_event_control_writable = True

            status, payload = await self._request(NDP_DIRECT_EVENT_SENSITIVITY)
            if status == NDP_OK and len(payload) == 4 and payload[0] == 1:
                data.accel_event_sensitivity = payload[1]
                data.accel_event_sensitivity_persisted = bool(payload[2])
                data.accel_event_sensitivity_writable = True

        if data.has_capability(CAP_VIB_AUTO):
            status, payload = await self._request(NDP_VIB_AUTO_STATUS)
            if status == NDP_OK and len(payload) == 15:
                data.vib_auto_enabled = bool(payload[0])
                data.vib_auto_discovery_complete = bool(payload[1])
                data.vib_auto_state = payload[2]
                data.vib_auto_profile_count = payload[3]
                data.vib_auto_control_writable = True

            cfg_status, cfg_page = await self._request(NDP_VIB_AUTO_CONFIG, b"\x81")
            if cfg_status == NDP_OK and len(cfg_page) == 13 and cfg_page[9] <= 2:
                data.vib_auto_sensitivity = cfg_page[9]
                data.vib_auto_sensitivity_writable = True

        if data.has_capability(CAP_HALL):
            status, payload = await self._request(NDP_DIRECT_HALL_CONTROL)
            if status == NDP_OK and len(payload) == 7 and payload[0] == 1:
                data.hall_mode = payload[1]
                data.hall_channel = payload[2]
                data.hall_control_persisted_mode = payload[3] if payload[5] else None
                data.hall_control_persisted_channel = payload[4] if payload[5] else None
                data.hall_control_writable = True

        if read_battery and data.has_capability(CAP_BATTERY):
            status, payload = await self._sensor_read(
                SENSOR_BATTERY,
                retries=3,
                busy_delay=0.15,
            )
            if status == NDP_OK and len(payload) >= 2:
                data.battery_v = int.from_bytes(payload[:2], "little") / 100.0

        if data.has_capability(CAP_TEMPERATURE):
            status, payload = await self._sensor_read(
                SENSOR_TEMPERATURE,
                retries=3,
                busy_delay=0.8,
            )
            if status == NDP_OK and len(payload) == 5:
                data.temperature_source = payload[0]
                data.temperature_c = int.from_bytes(
                    payload[1:5], "little", signed=True
                ) / 1000.0
        elif data.has_capability(CAP_DS18B20) and data.ds18b20_present:
            status, payload = await self._sensor_read(
                SENSOR_DS18B20,
                retries=3,
                busy_delay=0.8,
            )
            if status == NDP_OK and len(payload) == 4:
                data.temperature_source = 1
                data.temperature_c = int.from_bytes(
                    payload, "little", signed=True
                ) / 1000.0

        if data.has_capability(CAP_ACCEL):
            # B7.6f2m6c4: raw XYZ has no HA entity anymore. Do not spend one
            # synchronous NDP transaction refreshing dashboard-only telemetry.
            # Event control and VIB_AUTO remain independent of this read.
            status, payload = await self._sensor_read(SENSOR_ACCEL_METRICS)
            if status == NDP_OK and len(payload) >= 8:
                data.vibration_rms_mg = int.from_bytes(payload[0:2], "little")
                data.vibration_peak_mg = int.from_bytes(payload[2:4], "little")
                data.vibration_peak_to_peak_mg = int.from_bytes(payload[4:6], "little")
                data.vibration_zero_cross_hz = int.from_bytes(payload[6:8], "little")

        if data.has_capability(CAP_HALL) and data.hall_mode:
            status, payload = await self._sensor_read(SENSOR_HALL)
            if status == NDP_OK and len(payload) >= 4:
                data.hall_value = int.from_bytes(
                    payload[:4],
                    "little",
                    signed=data.hall_mode == 2,
                )

        return data

    async def async_read_snapshot(
        self,
        base: NrfClawData | None = None,
        *,
        refresh_static: bool = False,
        read_battery: bool = True,
    ) -> NrfClawData:
        """Compatibility wrapper: static discovery only when requested/needed."""
        if base is None or refresh_static:
            base = await self.async_read_static_snapshot()
        return await self.async_read_runtime_snapshot(base, read_battery=read_battery)

    async def async_get_vib_auto_status(self) -> tuple[bool, bool, int, int, bool]:
        """Return enabled, discovery-complete, state, profile count, persistence busy."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, payload = await self._request(NDP_VIB_AUTO_STATUS)
        if status != NDP_OK or len(payload) != 15:
            raise RuntimeError(f"VIB_AUTO_STATUS failed: status={status} payload={payload!r}")
        status1, page1 = await self._request(NDP_VIB_AUTO_STATUS, b"\x01")
        busy = bool(page1[12]) if status1 == NDP_OK and len(page1) == 13 else False
        return bool(payload[0]), bool(payload[1]), payload[2], payload[3], busy

    async def async_get_vib_auto_sensitivity(self) -> tuple[int, bytes]:
        """Return VIB_AUTO sensitivity and the frozen page-1 config payload."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, page = await self._request(NDP_VIB_AUTO_CONFIG, b"\x81")
        if status != NDP_OK or len(page) != 13:
            raise RuntimeError(
                f"VIB_AUTO_CONFIG page 1 failed: status={status} payload={page!r}"
            )
        sensitivity = page[9]
        if sensitivity > 2:
            raise RuntimeError(f"invalid VIB_AUTO sensitivity readback: {sensitivity}")
        return sensitivity, bytes(page)

    async def async_set_vib_auto_sensitivity(
        self, sensitivity: int, *, timeout: float = 6.0
    ) -> int:
        """Persist LOW/NORMAL/HIGH VIB_AUTO sensitivity without changing other tuning."""
        if sensitivity not in (0, 1, 2):
            raise ValueError(f"unsupported VIB_AUTO sensitivity: {sensitivity}")
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()

        current, page = await self.async_get_vib_auto_sensitivity()
        if current == sensitivity:
            return current

        updated = bytearray(page)
        updated[9] = sensitivity
        status, _ = await self._request(NDP_VIB_AUTO_CONFIG, b"\x01" + bytes(updated))
        if status != NDP_OK:
            raise RuntimeError(
                f"VIB_AUTO sensitivity SET failed with NDP status {status}"
            )

        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            readback, _ = await self.async_get_vib_auto_sensitivity()
            _, _, _, _, busy = await self.async_get_vib_auto_status()
            if readback == sensitivity and not busy:
                return readback
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("VIB_AUTO sensitivity persistence/readback timeout")
            await asyncio.sleep(0.05)

    async def async_set_vib_auto_enabled(self, enabled: bool) -> tuple[bool, bool, int, int]:
        """Start/stop autonomous vibration monitoring and verify persisted readback."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        if enabled:
            status, _ = await self._request(NDP_VIB_AUTO_START, b"\x00")
        else:
            status, _ = await self._request(NDP_VIB_AUTO_STOP)
        if status != NDP_OK:
            raise RuntimeError(
                f"VIB_AUTO {'START' if enabled else 'STOP'} failed with NDP status {status}"
            )

        deadline = asyncio.get_running_loop().time() + 5.0
        while True:
            current, complete, state, profiles, busy = await self.async_get_vib_auto_status()
            if current == enabled and not busy:
                return current, complete, state, profiles
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("VIB_AUTO persistence/readback timeout")
            await asyncio.sleep(0.05)

    async def async_relearn_vib_auto(self) -> tuple[bool, bool, int, int]:
        """Clear learned profiles while Auto is active and re-enter learning."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        enabled, _, _, _, _ = await self.async_get_vib_auto_status()
        if not enabled:
            raise RuntimeError("VIB_AUTO relearn requires Auto monitoring to be enabled")
        status, _ = await self._request(NDP_VIB_AUTO_RESET)
        if status != NDP_OK:
            raise RuntimeError(f"VIB_AUTO_RESET failed with NDP status {status}")

        deadline = asyncio.get_running_loop().time() + 5.0
        while True:
            current, complete, state, profiles, busy = await self.async_get_vib_auto_status()
            if current and not complete and profiles == 0 and not busy:
                return current, complete, state, profiles
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("VIB_AUTO relearn persistence/readback timeout")
            await asyncio.sleep(0.05)

    async def async_reset_semantic_accumulator(self) -> DirectAdvSemantic:
        """Reset a VM-backed retained semantic accumulator and verify zero readback."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, _ = await self._request(NDP_DIRECT_SEMANTIC_STATE, b"\xFF")
        if status != NDP_OK:
            raise RuntimeError(f"semantic accumulator reset failed with NDP status {status}")

        deadline = asyncio.get_running_loop().time() + 5.0
        while True:
            ctl_status, ctl = await self._request(NDP_DIRECT_SEMANTIC_STATE, b"\x00")
            sem_status, payload = await self._request(NDP_DIRECT_SEMANTIC_STATE)
            semantic = parse_semantic_state_record(payload) if sem_status == NDP_OK else None
            if (
                ctl_status == NDP_OK
                and len(ctl) == 3
                and ctl[0] == 1
                and ctl[2] == 0
                and semantic is not None
                and semantic.raw_value == 0
            ):
                return semantic
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("semantic accumulator reset persistence/readback timeout")
            await asyncio.sleep(0.05)

    async def async_get_accel_event_control(self) -> tuple[int, int | None, int]:
        """Return runtime mode, persisted preset (if any), and store status."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, payload = await self._request(NDP_DIRECT_SENSOR_CONTROL)
        if status != NDP_OK:
            raise RuntimeError(
                f"DIRECT_SENSOR_CONTROL GET failed with NDP status {status}"
            )
        if len(payload) != 5 or payload[0] != 1:
            raise RuntimeError(
                f"invalid DIRECT_SENSOR_CONTROL payload length/version: {payload!r}"
            )
        persisted = payload[2] if payload[3] else None
        return payload[1], persisted, payload[4]

    async def async_set_accel_event_mode(
        self,
        mode: int,
        *,
        timeout: float = 6.0,
    ) -> tuple[int, int | None]:
        """Persist a Direct event preset and return exact runtime/readback state."""
        if mode not in (0, 1, 2, 3):
            raise ValueError(f"unsupported Direct accelerometer event mode: {mode}")
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()

        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            status, _ = await self._request(
                NDP_DIRECT_SENSOR_CONTROL, bytes((mode,))
            )
            if status == NDP_OK:
                break
            if status != NDP_BUSY or asyncio.get_running_loop().time() >= deadline:
                raise RuntimeError(
                    f"DIRECT_SENSOR_CONTROL SET failed with NDP status {status}"
                )
            await asyncio.sleep(0.05)

        while True:
            runtime_mode, persisted_mode, store_status = (
                await self.async_get_accel_event_control()
            )
            if store_status == 2:
                raise RuntimeError("Direct sensor control persistence failed")
            if store_status == 0:
                if runtime_mode != mode or persisted_mode != mode:
                    raise RuntimeError(
                        "Direct sensor/event control readback mismatch"
                    )
                return runtime_mode, persisted_mode
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError(
                    "Direct sensor/event control persistence did not commit before timeout"
                )
            await asyncio.sleep(0.05)

    async def async_get_accel_event_sensitivity(self) -> tuple[int, bool, int]:
        """Return sensitivity preset, persisted flag, and store status."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, payload = await self._request(NDP_DIRECT_EVENT_SENSITIVITY)
        if status != NDP_OK:
            raise RuntimeError(
                f"DIRECT_EVENT_SENSITIVITY GET failed with NDP status {status}"
            )
        if len(payload) != 4 or payload[0] != 1:
            raise RuntimeError(
                f"invalid DIRECT_EVENT_SENSITIVITY payload: {payload!r}"
            )
        return payload[1], bool(payload[2]), payload[3]

    async def async_set_accel_event_sensitivity(
        self,
        sensitivity: int,
        *,
        timeout: float = 6.0,
    ) -> int:
        """Persist LOW/NORMAL/HIGH and return exact committed readback."""
        if sensitivity not in (0, 1, 2):
            raise ValueError(f"unsupported Direct event sensitivity: {sensitivity}")
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()

        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            status, _ = await self._request(
                NDP_DIRECT_EVENT_SENSITIVITY, bytes((sensitivity,))
            )
            if status == NDP_OK:
                break
            if status != NDP_BUSY or asyncio.get_running_loop().time() >= deadline:
                raise RuntimeError(
                    f"DIRECT_EVENT_SENSITIVITY SET failed with NDP status {status}"
                )
            await asyncio.sleep(0.05)

        while True:
            readback, persisted, store_status = (
                await self.async_get_accel_event_sensitivity()
            )
            if store_status == 2:
                raise RuntimeError("Direct event sensitivity persistence failed")
            if store_status == 0:
                if readback != sensitivity or not persisted:
                    raise RuntimeError("Direct event sensitivity readback mismatch")
                return readback
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError(
                    "Direct event sensitivity persistence did not commit before timeout"
                )
            await asyncio.sleep(0.05)

    async def async_get_direct_hall_control(
        self,
    ) -> tuple[int, int, int | None, int | None, int]:
        """Return runtime Hall mode/channel, persisted profile, and store state."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, payload = await self._request(NDP_DIRECT_HALL_CONTROL)
        if status != NDP_OK:
            raise RuntimeError(
                f"DIRECT_HALL_CONTROL GET failed with NDP status {status}"
            )
        if len(payload) != 7 or payload[0] != 1:
            raise RuntimeError(f"invalid DIRECT_HALL_CONTROL payload: {payload!r}")
        persisted_mode = payload[3] if payload[5] else None
        persisted_channel = payload[4] if payload[5] else None
        return payload[1], payload[2], persisted_mode, persisted_channel, payload[6]

    async def async_set_direct_hall_control(
        self,
        mode: int,
        channel: int,
        *,
        timeout: float = 6.0,
    ) -> tuple[int, int]:
        """Persist a Direct Hall mode and return exact runtime readback."""
        if mode not in (0, 1, 2):
            raise ValueError(f"unsupported Direct Hall mode: {mode}")
        if mode == 1 and channel not in (1, 2):
            raise ValueError(f"unsupported Direct Hall channel: {channel}")
        if mode != 1:
            channel = 1
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()

        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            status, _ = await self._request(
                NDP_DIRECT_HALL_CONTROL, bytes((mode, channel))
            )
            if status == NDP_OK:
                break
            if status != NDP_BUSY or asyncio.get_running_loop().time() >= deadline:
                raise RuntimeError(
                    f"DIRECT_HALL_CONTROL SET failed with NDP status {status}"
                )
            await asyncio.sleep(0.05)

        while True:
            runtime_mode, runtime_channel, persisted_mode, persisted_channel, store_status = (
                await self.async_get_direct_hall_control()
            )
            if store_status == 2:
                raise RuntimeError("Direct Hall control persistence failed")
            if store_status == 0:
                expected_channel = channel if mode == 1 else 1
                if (
                    runtime_mode != mode
                    or runtime_channel != expected_channel
                    or persisted_mode != mode
                    or persisted_channel != expected_channel
                ):
                    raise RuntimeError("Direct Hall control readback mismatch")
                return runtime_mode, runtime_channel
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError(
                    "Direct Hall control persistence did not commit before timeout"
                )
            await asyncio.sleep(0.05)

    async def async_reset_direct_hall_value(self) -> None:
        """Reset the active Hall counter/position without changing its mode."""
        if not self.is_connected:
            raise ConnectionError("nRFClaw BLE connection is not active")
        await self._async_ensure_access()
        status, _ = await self._request(NDP_DIRECT_HALL_CONTROL, b"\xFF")
        if status != NDP_OK:
            raise RuntimeError(
                f"DIRECT_HALL_CONTROL reset failed with NDP status {status}"
            )

    async def async_prepare_low_power_disconnect(self) -> bool:
        """Ask firmware to resume directly in SLOW after this connection closes."""
        if not self.is_connected:
            return False
        status, _ = await self._request(NDP_BLE_DISCONNECT_HINT)
        return status == NDP_OK
