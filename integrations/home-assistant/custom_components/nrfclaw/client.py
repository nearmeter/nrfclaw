from __future__ import annotations

import asyncio
import hashlib
import hmac
from dataclasses import dataclass

from bleak_retry_connector import establish_connection
from bleak import BleakClient

from .const import (
    RX_UUID, TX_UUID,
    NDP_INFO, NDP_CAPS, NDP_BOARD_INFO, NDP_BOARD_RESET, NDP_STATUS,
    NDP_SENSOR_READ, NDP_AUTH_BEGIN, NDP_AUTH_FINISH, NDP_ACCESS_STATUS,
    NDP_ACCESS_CONTROL,
    CAP_BATTERY, CAP_DS18B20, CAP_ACCEL, CAP_HALL,
    SENSOR_BATTERY, SENSOR_HALL, SENSOR_DS18B20, SENSOR_ACCEL_METRICS,
    BOARD_NAMES,
)


def _cap(mask: int, capability: int) -> bool:
    return bool(mask & (1 << (capability - 1)))


@dataclass
class NrfClawData:
    ndp_version: int | None = None
    vm_abi: int | None = None
    board_type: int | None = None
    board_name: str | None = None
    hw_rev: str | None = None
    firmware: str | None = None
    firmware_build: int | None = None
    device_id: str | None = None
    capabilities: int = 0
    reset_reason: int | None = None
    battery_v: float | None = None
    hall_mode: int = 0
    hall_value: int | None = None
    temperature_c: float | None = None
    vibration_rms_mg: int | None = None
    vibration_peak_mg: int | None = None

    def has_capability(self, capability: int) -> bool:
        return _cap(self.capabilities, capability)


class NrfClawClient:
    def __init__(self, ble_device, access_key: str | None = None):
        self.ble_device = ble_device
        self.seq = 0
        self.access_key = self._decode_access_key(access_key or "")

    @staticmethod
    def _decode_access_key(value: str) -> bytes | None:
        if not value:
            return None
        if value.startswith("hex:"):
            try:
                key = bytes.fromhex(value[4:])
            except ValueError as exc:
                raise ValueError("invalid hexadecimal nRFClaw NDP access key") from exc
        else:
            key = value.encode("utf-8")
        if len(key) != 32:
            raise ValueError("nRFClaw NDP access key must be exactly 32 bytes")
        return key

    def _frame(self, opcode: int, payload: bytes = b"") -> bytes:
        self.seq = (self.seq + 1) & 0xFF
        return bytes((0x00, opcode, self.seq, len(payload))) + payload

    async def _request(self, client: BleakClient, opcode: int,
                       payload: bytes = b"") -> tuple[int, bytes]:
        frame = self._frame(opcode, payload)
        expected_seq = frame[2]
        future = asyncio.get_running_loop().create_future()

        def notify(_sender, data: bytearray):
            raw = bytes(data)
            if len(raw) < 5 or raw[1] != opcode or raw[2] != expected_seq:
                return
            if not future.done():
                future.set_result(raw)

        await client.start_notify(TX_UUID, notify)
        try:
            await client.write_gatt_char(RX_UUID, frame, response=False)
            raw = await asyncio.wait_for(future, 5.0)
        finally:
            await client.stop_notify(TX_UUID)

        if raw[3] + 4 != len(raw):
            raise ValueError("invalid NDP response length")
        return raw[4], raw[5:]

    async def _ensure_access(self, client: BleakClient) -> None:
        # Open devices answer INFO directly. A provisioned device returns
        # UNAUTHORIZED until the optional 256-bit owner key authenticates.
        status, _ = await self._request(client, NDP_INFO)
        if status == 0:
            return
        if status != 5:  # NDP_UNAUTHORIZED
            raise PermissionError(f"nRFClaw NDP access probe failed ({status})")
        if self.access_key is None:
            raise PermissionError(
                "This nRFClaw has NDP protection enabled; configure its NDP access key"
            )

        status, p = await self._request(client, NDP_ACCESS_STATUS)
        if status != 0 or len(p) != 1:
            raise PermissionError("unable to read nRFClaw NDP access status")
        if p[0] >= NDP_ACCESS_CONTROL:
            return

        status, p = await self._request(
            client, NDP_AUTH_BEGIN, bytes((NDP_ACCESS_CONTROL,))
        )
        if status != 0 or len(p) != 13:
            raise PermissionError("nRFClaw NDP authentication challenge failed")

        challenge = p[:12]
        session_id = p[12]
        tag16 = hmac.new(
            self.access_key,
            challenge + bytes((NDP_ACCESS_CONTROL, session_id)),
            hashlib.sha256,
        ).digest()[:16]

        status, p = await self._request(client, NDP_AUTH_FINISH, tag16)
        if status != 0 or len(p) != 1 or p[0] < NDP_ACCESS_CONTROL:
            raise PermissionError("invalid nRFClaw NDP access key")

    async def read(self) -> NrfClawData:
        client = await establish_connection(
            BleakClient, self.ble_device, self.ble_device.name or "nRFClaw",
            max_attempts=2,
        )
        data = NrfClawData()
        try:
            await self._ensure_access(client)

            status, p = await self._request(client, NDP_INFO)
            if status == 0 and len(p) >= 3:
                data.ndp_version = p[0]
                data.vm_abi = p[1]
                data.board_type = p[2]
                data.board_name = BOARD_NAMES.get(p[2], f"BOARD-{p[2]}")

            status, p = await self._request(client, NDP_CAPS)
            if status == 0 and len(p) >= 2:
                data.capabilities = int.from_bytes(p[:2], "little")

            status, p = await self._request(client, NDP_BOARD_INFO, b"\x00")
            if status == 0 and len(p) >= 15:
                data.board_type = p[0]
                data.board_name = BOARD_NAMES.get(p[0], f"BOARD-{p[0]}")
                data.hw_rev = f"{p[1]}.{p[2]}"
                suffix = f"-pre{p[6]}" if p[6] else ""
                data.firmware = f"{p[3]}.{p[4]}.{p[5]}{suffix}"
                data.firmware_build = int.from_bytes(p[7:9], "little")
                data.ndp_version = p[9]
                data.vm_abi = p[10]

            status, p = await self._request(client, NDP_BOARD_INFO, b"\x01")
            if status == 0 and len(p) >= 8:
                d0 = int.from_bytes(p[:4], "little")
                d1 = int.from_bytes(p[4:8], "little")
                data.device_id = f"{d1:08X}{d0:08X}"

            status, p = await self._request(client, NDP_BOARD_RESET)
            if status == 0 and len(p) >= 4:
                data.reset_reason = int.from_bytes(p[:4], "little")

            status, p = await self._request(client, NDP_STATUS)
            if status == 0 and len(p) >= 2:
                data.hall_mode = p[0]

            if data.has_capability(CAP_BATTERY):
                # Battery acquisition is asynchronous. First request can return BUSY.
                for _ in range(2):
                    status, p = await self._request(
                        client, NDP_SENSOR_READ, bytes((SENSOR_BATTERY,))
                    )
                    if status == 0 and len(p) >= 2:
                        cv = int.from_bytes(p[:2], "little")
                        data.battery_v = cv / 100.0
                        break
                    if status != 4:  # NDP_BUSY
                        break
                    await asyncio.sleep(0.15)

            if data.has_capability(CAP_DS18B20):
                status, p = await self._request(
                    client, NDP_SENSOR_READ, bytes((SENSOR_DS18B20,))
                )
                if status == 4:
                    await asyncio.sleep(0.8)
                    status, p = await self._request(
                        client, NDP_SENSOR_READ, bytes((SENSOR_DS18B20,))
                    )
                if status == 0 and len(p) >= 4:
                    data.temperature_c = int.from_bytes(
                        p[:4], "little", signed=True
                    ) / 1000.0

            if data.has_capability(CAP_ACCEL):
                status, p = await self._request(
                    client, NDP_SENSOR_READ, bytes((SENSOR_ACCEL_METRICS,))
                )
                if status == 0 and len(p) >= 4:
                    data.vibration_rms_mg = int.from_bytes(p[:2], "little")
                    data.vibration_peak_mg = int.from_bytes(p[2:4], "little")

            if data.has_capability(CAP_HALL) and data.hall_mode:
                status, p = await self._request(
                    client, NDP_SENSOR_READ, bytes((SENSOR_HALL,))
                )
                if status == 0 and len(p) >= 4:
                    signed = data.hall_mode == 2
                    data.hall_value = int.from_bytes(
                        p[:4], "little", signed=signed
                    )
        finally:
            await client.disconnect()

        return data
