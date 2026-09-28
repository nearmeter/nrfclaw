"""B7.5 persistent Home Assistant Application/NDP transport."""

from __future__ import annotations

import asyncio
import hashlib
import hmac
from typing import Any

from bleak import BleakClient
from bleak_retry_connector import establish_connection
from homeassistant.components import bluetooth
from homeassistant.core import HomeAssistant

from .const import (
    APP_RX_UUID,
    APP_SERVICE_UUID,
    APP_TX_UUID,
    NDP_ACCESS_CONTROL,
    NDP_ACCESS_STATUS,
    NDP_AUTH_BEGIN,
    NDP_AUTH_FINISH,
    NDP_INFO,
    NDP_NINALINK_BRIDGE,
)

_NDP_UNAUTHORIZED = 5
_DISCONNECTED = object()


def decode_access_key(value: str | None) -> bytes | None:
    if not value:
        return None
    if value.startswith("hex:"):
        try:
            key = bytes.fromhex(value[4:])
        except ValueError as exc:
            raise ValueError("invalid hexadecimal nRFClaw NDP key") from exc
    else:
        key = value.encode("utf-8")
    if len(key) != 32:
        raise ValueError("nRFClaw NDP key must be exactly 32 bytes")
    return key


class NinaLinkApplicationClient:
    """One persistent authenticated Application/NDP BLE connection."""

    def __init__(
        self,
        hass: HomeAssistant,
        address: str,
        access_key: str | None = None,
    ) -> None:
        self.hass = hass
        self.address = address
        self.access_key = decode_access_key(access_key)
        self.client: BleakClient | None = None
        self.response_queue: asyncio.Queue[Any] = asyncio.Queue()
        self.change_queue: asyncio.Queue[Any] = asyncio.Queue()
        self._seq = 0
        self._request_lock = asyncio.Lock()
        self._notify_started = False
        self._disconnected = asyncio.Event()

    @property
    def is_connected(self) -> bool:
        return bool(self.client and self.client.is_connected)

    def _on_disconnect(self, _client: BleakClient) -> None:
        self._disconnected.set()
        try:
            self.change_queue.put_nowait(_DISCONNECTED)
        except asyncio.QueueFull:
            pass

    def _notify(self, _sender: Any, data: bytearray) -> None:
        raw = bytes(data)
        if len(raw) == 20 and raw[0] == 0xE5 and raw[1] == 1:
            self.change_queue.put_nowait(raw)
            return
        self.response_queue.put_nowait(raw)

    async def connect(self) -> None:
        if self.is_connected:
            return

        self.response_queue = asyncio.Queue()
        self.change_queue = asyncio.Queue()
        self._disconnected.clear()

        device = bluetooth.async_ble_device_from_address(
            self.hass,
            self.address,
            connectable=True,
        )
        if device is None:
            await bluetooth.async_request_active_scan(self.hass)
            device = bluetooth.async_ble_device_from_address(
                self.hass,
                self.address,
                connectable=True,
            )
        if device is None:
            raise ConnectionError(
                f"nRFClaw {self.address} is not reachable by a connectable scanner"
            )

        client = await establish_connection(
            BleakClient,
            device,
            device.name or f"nRFClaw {self.address}",
            disconnected_callback=self._on_disconnect,
        )
        self.client = client

        try:
            services = client.services
            if services.get_service(APP_SERVICE_UUID) is None:
                raise ConnectionError(
                    f"Application service {APP_SERVICE_UUID} not present"
                )
            if services.get_characteristic(APP_RX_UUID) is None:
                raise ConnectionError(
                    f"Application RX {APP_RX_UUID} not present"
                )
            if services.get_characteristic(APP_TX_UUID) is None:
                raise ConnectionError(
                    f"Application TX {APP_TX_UUID} not present"
                )

            await client.start_notify(APP_TX_UUID, self._notify)
            self._notify_started = True
            await self.ensure_application_access()
        except BaseException:
            await self.close()
            raise

    async def close(self) -> None:
        client = self.client
        self.client = None
        if client is None:
            return

        if self._notify_started:
            try:
                if client.is_connected:
                    await client.stop_notify(APP_TX_UUID)
            except BaseException:
                pass
            self._notify_started = False

        try:
            if client.is_connected:
                await client.disconnect()
        except BaseException:
            pass

    async def ndp_command_raw(
        self,
        opcode: int,
        payload: bytes = b"",
        timeout: float = 5.0,
    ) -> tuple[int, bytes]:
        if len(payload) > 16:
            raise ValueError("Application NDP payload exceeds 16 bytes")
        if not self.is_connected or self.client is None:
            raise ConnectionError("Application BLE is not connected")

        async with self._request_lock:
            self._seq = (self._seq + 1) & 0xFF
            seq = self._seq
            frame = bytes([0, opcode & 0xFF, seq, len(payload)]) + payload
            if len(frame) > 20:
                raise ValueError("Application NDP frame exceeds ATT payload")

            await self.client.write_gatt_char(
                APP_RX_UUID,
                frame,
                response=False,
            )

            loop = asyncio.get_running_loop()
            end = loop.time() + timeout
            while True:
                remain = end - loop.time()
                if remain <= 0:
                    raise TimeoutError(
                        f"NDP timeout opcode=0x{opcode:02X} seq={seq}"
                    )
                raw = await asyncio.wait_for(
                    self.response_queue.get(),
                    remain,
                )
                if len(raw) < 5:
                    continue
                _flags, op, rx_seq, plen = raw[:4]
                if op != opcode or rx_seq != seq:
                    continue
                if len(raw) != 4 + plen:
                    raise RuntimeError("invalid Application NDP response length")
                body = raw[4:]
                if not body:
                    raise RuntimeError("Application NDP response missing status")
                return body[0], body[1:]

    async def ndp_command(
        self,
        opcode: int,
        payload: bytes = b"",
        timeout: float = 5.0,
    ) -> bytes:
        status, body = await self.ndp_command_raw(opcode, payload, timeout)
        if status != 0:
            raise RuntimeError(
                f"Application NDP 0x{opcode:02X} returned status {status}"
            )
        return body

    async def ensure_application_access(self) -> None:
        status, _ = await self.ndp_command_raw(NDP_INFO)
        if status == 0:
            return
        if status != _NDP_UNAUTHORIZED:
            raise PermissionError(
                f"nRFClaw Application access probe failed: status={status}"
            )
        if self.access_key is None:
            raise PermissionError(
                "This nRFClaw requires an NDP access key"
            )

        p = await self.ndp_command(
            NDP_AUTH_BEGIN,
            bytes([NDP_ACCESS_CONTROL]),
        )
        if len(p) != 13:
            raise PermissionError("invalid NDP authentication challenge")

        challenge, session_id = p[:12], p[12]
        tag16 = hmac.new(
            self.access_key,
            challenge + bytes([NDP_ACCESS_CONTROL, session_id]),
            hashlib.sha256,
        ).digest()[:16]

        p = await self.ndp_command(NDP_AUTH_FINISH, tag16)
        if len(p) != 1 or p[0] < NDP_ACCESS_CONTROL:
            raise PermissionError("NDP CONTROL authentication was not granted")

    @staticmethod
    def _decode_b55_change(raw: bytes) -> dict[str, Any]:
        if len(raw) != 20 or raw[0] != 0xE5 or raw[1] != 1:
            raise RuntimeError("invalid B5.5 change notification")
        flags = raw[2]
        return {
            "schema": raw[1],
            "flags": flags,
            "state_changed": bool(flags & 1),
            "event_changed": bool(flags & 2),
            "mask": raw[3],
            "state_revision": int.from_bytes(raw[4:8], "little"),
            "event_revision": int.from_bytes(raw[8:12], "little"),
            "newest_event_id": int.from_bytes(raw[12:16], "little"),
            "change_revision": int.from_bytes(raw[16:20], "little"),
        }

    @staticmethod
    def _decode_b55_status(p: bytes) -> dict[str, Any]:
        if len(p) != 15:
            raise RuntimeError(f"invalid B5.5 status length: {len(p)}")
        return {
            "schema": p[0],
            "mask": p[1],
            "pending_flags": p[2],
            "state_revision": int.from_bytes(p[3:7], "little"),
            "event_revision": int.from_bytes(p[7:11], "little"),
            "newest_event_id": int.from_bytes(p[11:15], "little"),
        }

    async def ninalink_change_subscribe(
        self,
        mask: int,
    ) -> dict[str, Any]:
        if mask < 0 or mask > 3:
            raise ValueError("B5.5 subscription mask must be 0..3")
        p = await self.ndp_command(
            NDP_NINALINK_BRIDGE,
            bytes([37, mask]),
        )
        return self._decode_b55_status(p)

    async def ninalink_wait_change(
        self,
        timeout: float = 60.0,
    ) -> dict[str, Any]:
        try:
            raw = await asyncio.wait_for(
                self.change_queue.get(),
                timeout,
            )
        except asyncio.TimeoutError as exc:
            raise TimeoutError(
                f"timeout waiting B5.5 change ({timeout}s)"
            ) from exc

        if raw is _DISCONNECTED:
            raise ConnectionError("Application BLE disconnected")
        return self._decode_b55_change(raw)
