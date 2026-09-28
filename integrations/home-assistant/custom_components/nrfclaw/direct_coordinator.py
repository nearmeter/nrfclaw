"Coordinator for low-power Direct NDP + passive advertising telemetry/events."

from __future__ import annotations

import asyncio
from collections.abc import Callable
from dataclasses import replace
import logging
import time

from bleak.exc import BleakError
from bleak_retry_connector import BleakConnectionError, clear_cache
from homeassistant.components import bluetooth
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .client import NrfClawClient, NrfClawData, NrfClawGattLayoutError
from .const import CONF_ACCESS_KEY, DOMAIN
from .direct_adv import (
    MANUFACTURER_ID,
    DirectAdvEvent,
    DirectAdvAcceleration,
    parse_direct_adv_event,
    parse_direct_adv_acceleration,
    parse_direct_adv_semantic,
    parse_direct_adv_telemetry,
)

_LOGGER = logging.getLogger(__name__)

# Event page is held ~6.5 s by firmware. Keep a slightly longer duplicate
# window so repeated radio copies trigger exactly one HA event.
_EVENT_DUP_WINDOW_S = 10.0


class DirectNdpCoordinator(DataUpdateCoordinator[NrfClawData]):
    """Bootstrap once over GATT, then consume passive advertisements."""

    def __init__(
        self,
        hass: HomeAssistant,
        entry: ConfigEntry,
        ble_lock: asyncio.Lock,
    ) -> None:
        self.address = entry.data["address"]
        self._ble_lock = ble_lock
        self.client = NrfClawClient(
            self.address,
            entry.data.get(CONF_ACCESS_KEY, ""),
        )
        self._last_adv_sequence: int | None = None
        self._last_event_key: tuple[int, int, int, int, int, int] | None = None
        self._last_event_seen = 0.0
        self._event_listeners: list[Callable[[DirectAdvEvent], None]] = []

        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN}-direct-{self.address}",
            update_interval=None,
            always_update=False,
        )

    async def _async_get_ble_device(self, *, active_scan: bool = False):
        if active_scan:
            await bluetooth.async_request_active_scan(self.hass)
        return bluetooth.async_ble_device_from_address(
            self.hass, self.address, connectable=True
        )

    async def _async_connect(self, *, active_scan: bool = False) -> None:
        ble_device = await self._async_get_ble_device(active_scan=active_scan)
        if ble_device is None:
            raise ConnectionError(
                f"nRFClaw {self.address} is not visible to a connectable HA Bluetooth adapter"
            )
        suffix = self.address.replace(":", "").replace("-", "")[-6:].upper()
        await self.client.async_connect(
            ble_device,
            ble_device.name or f"nRFClaw(NDP)-{suffix}",
        )

    async def _async_update_data(self) -> NrfClawData:
        """One bootstrap/manual GATT refresh; runtime is advertisement-only."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError as err:
                    _LOGGER.warning(
                        "Stale Direct-NDP GATT layout for %s (%s); clearing BlueZ cache",
                        self.address,
                        err,
                    )
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                static_data = await self.client.async_read_static_snapshot()
                data = await self.client.async_read_runtime_snapshot(
                    static_data,
                    read_battery=True,
                )

                try:
                    armed = await self.client.async_prepare_low_power_disconnect()
                    if not armed:
                        _LOGGER.warning(
                            "nRFClaw %s did not arm LOW_POWER disconnect after bootstrap",
                            self.address,
                        )
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s LOW_POWER bootstrap disconnect hint failed (%s)",
                        self.address,
                        err,
                    )
                return data

            except PermissionError as err:
                raise UpdateFailed(str(err)) from err
            except (
                BleakConnectionError,
                BleakError,
                TimeoutError,
                ConnectionError,
            ) as err:
                raise UpdateFailed(str(err)) from err
            except Exception as err:
                raise UpdateFailed(
                    f"unexpected Direct-NDP bootstrap error: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    @callback
    def async_add_direct_event_listener(
        self,
        listener: Callable[[DirectAdvEvent], None],
    ) -> Callable[[], None]:
        self._event_listeners.append(listener)

        @callback
        def remove_listener() -> None:
            if listener in self._event_listeners:
                self._event_listeners.remove(listener)

        return remove_listener

    @callback
    def _publish_direct_event(self, event: DirectAdvEvent) -> None:
        key = (
            event.event_id,
            event.event_type_id,
            event.capability_id,
            event.channel,
            event.arg0,
            event.arg1,
        )
        now = time.monotonic()

        if (
            key == self._last_event_key
            and (now - self._last_event_seen) < _EVENT_DUP_WINDOW_S
        ):
            return

        self._last_event_key = key
        self._last_event_seen = now

        self.hass.bus.async_fire(
            "nrfclaw_direct_event",
            {
                "address": self.address,
                "event_id": event.event_id,
                "event_type": event.event_type,
                "capability_id": event.capability_id,
                "channel": event.channel,
                "arg0": event.arg0,
                "arg1": event.arg1,
                "sequence": event.sequence,
            },
        )

        for listener in tuple(self._event_listeners):
            listener(event)

    @callback
    def _async_bluetooth_event(
        self,
        service_info: bluetooth.BluetoothServiceInfoBleak,
        change: bluetooth.BluetoothChange,
    ) -> None:
        del change
        payload = service_info.manufacturer_data.get(MANUFACTURER_ID)
        if payload is None:
            return

        packet = bytes(payload)

        event = parse_direct_adv_event(packet)
        if event is not None:
            if event.role != 1:
                return
            self._publish_direct_event(event)
            return

        acceleration = parse_direct_adv_acceleration(packet)
        if acceleration is not None:
            if acceleration.role != 1:
                return
            if acceleration.sequence == self._last_adv_sequence:
                return
            self._last_adv_sequence = acceleration.sequence
            if self.data is None:
                return

            data = replace(self.data)
            data.active_capabilities = acceleration.active_capabilities
            if acceleration.accel_valid:
                data.acceleration_x_mg = acceleration.x_mg
                data.acceleration_y_mg = acceleration.y_mg
                data.acceleration_z_mg = acceleration.z_mg
            else:
                data.acceleration_x_mg = None
                data.acceleration_y_mg = None
                data.acceleration_z_mg = None
            self.async_set_updated_data(data)
            return

        semantic = parse_direct_adv_semantic(packet)
        if semantic is not None:
            if semantic.role != 1:
                return
            if semantic.sequence == self._last_adv_sequence:
                return
            self._last_adv_sequence = semantic.sequence
            if self.data is None:
                return
            data = replace(self.data)
            data.semantic_values = dict(self.data.semantic_values)
            data.semantic_values[(semantic.capability_id, semantic.channel)] = semantic
            self.async_set_updated_data(data)
            return

        telemetry = parse_direct_adv_telemetry(packet)
        if telemetry is None or telemetry.role != 1:
            return

        if telemetry.sequence == self._last_adv_sequence:
            return
        self._last_adv_sequence = telemetry.sequence

        if self.data is None:
            return

        data = replace(self.data)
        data.active_capabilities = telemetry.active_capabilities
        data.hall_mode = telemetry.hall_mode
        data.hall_channel = telemetry.hall_channel

        if telemetry.battery_v is not None:
            data.battery_v = telemetry.battery_v
        if telemetry.temperature_c is not None:
            data.temperature_c = telemetry.temperature_c

        if telemetry.hall_value is not None:
            data.hall_value = telemetry.hall_value
        elif telemetry.hall_mode == 0:
            data.hall_value = None

        data.vibration_rms_mg = None
        data.vibration_peak_mg = None
        data.vibration_peak_to_peak_mg = None
        data.vibration_zero_cross_hz = None

        self.async_set_updated_data(data)

    def async_start_advertisements(self):
        # Subscribe and immediately ingest Home Assistant's freshest BLE cache.
        cancel = bluetooth.async_register_callback(
            self.hass,
            self._async_bluetooth_event,
            {"address": self.address},
            bluetooth.BluetoothScanningMode.PASSIVE,
            replay=bluetooth.BluetoothCallbackReplay.NEWEST_FIRST,
        )

        # Explicit cache read covers startup even if callback replay ordering
        # changes. Sequence/event dedupe makes an overlap harmless.
        latest = bluetooth.async_last_service_info(
            self.hass,
            self.address,
            connectable=True,
        )
        if latest is not None:
            self._async_bluetooth_event(
                latest,
                bluetooth.BluetoothChange.ADVERTISEMENT,
            )

        return cancel

    async def async_write_vibration_monitoring(self, enabled: bool) -> NrfClawData:
        """Start/stop persistent autonomous vibration monitoring."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError:
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                # VIB_AUTO and the Direct Motion/Tap/Fall classifier share the
                # LIS2DH12.  Entering Auto explicitly persists Event detection
                # Off first instead of leaving two valid-looking owners for the
                # same interrupt/FIFO hardware.
                if enabled:
                    runtime_mode, persisted_mode, _ = (
                        await self.client.async_get_accel_event_control()
                    )
                    if runtime_mode != 0 or persisted_mode != 0:
                        await self.client.async_set_accel_event_mode(0)

                current, complete, state, profiles = (
                    await self.client.async_set_vib_auto_enabled(enabled)
                )
                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    vib_auto_enabled=current,
                    vib_auto_discovery_complete=complete,
                    vib_auto_state=state,
                    vib_auto_profile_count=profiles,
                    vib_auto_control_writable=True,
                    accel_event_mode=0,
                    accel_event_persisted_mode=0 if enabled else base.accel_event_persisted_mode,
                )
                self.async_set_updated_data(updated)
                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s vibration mode write committed but LOW_POWER disconnect hint failed (%s)",
                        self.address, err,
                    )
                return updated
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(
                    f"Direct vibration monitoring configuration failed: {err}"
                ) from err
            except Exception as err:
                raise HomeAssistantError(
                    f"Direct vibration monitoring configuration failed: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    async def async_write_vibration_sensitivity(self, sensitivity: int) -> NrfClawData:
        """Persist LOW/NORMAL/HIGH VIB_AUTO sensitivity and refresh HA state."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError as err:
                    _LOGGER.warning(
                        "Stale Direct-NDP GATT layout for %s during vibration sensitivity write (%s); clearing BlueZ cache",
                        self.address, err,
                    )
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                readback = await self.client.async_set_vib_auto_sensitivity(sensitivity)
                if readback != sensitivity:
                    raise HomeAssistantError(
                        "Vibration sensitivity readback does not match requested value"
                    )

                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    vib_auto_sensitivity=readback,
                    vib_auto_sensitivity_writable=True,
                )
                self.async_set_updated_data(updated)
                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s vibration sensitivity committed but LOW_POWER disconnect hint failed (%s)",
                        self.address, err,
                    )
                return updated
            except HomeAssistantError:
                raise
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(
                    f"Direct vibration sensitivity configuration failed: {err}"
                ) from err
            except Exception as err:
                raise HomeAssistantError(
                    f"Direct vibration sensitivity configuration failed: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    async def async_relearn_vibration_baseline(self) -> NrfClawData:
        """Clear VIB_AUTO profiles and restart learning while Auto remains enabled."""
        async with self._ble_lock:
            try:
                await self._async_connect()
                current, complete, state, profiles = await self.client.async_relearn_vib_auto()
                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    vib_auto_enabled=current,
                    vib_auto_discovery_complete=complete,
                    vib_auto_state=state,
                    vib_auto_profile_count=profiles,
                    vib_auto_control_writable=True,
                )
                self.async_set_updated_data(updated)
                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s vibration relearn committed but LOW_POWER disconnect hint failed (%s)",
                        self.address, err,
                    )
                return updated
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(f"Direct vibration relearn failed: {err}") from err
            except Exception as err:
                raise HomeAssistantError(f"Direct vibration relearn failed: {err}") from err
            finally:
                await self.client.async_disconnect()

    async def async_reset_accumulated_total(self) -> NrfClawData:
        """Reset a VM-backed retained total and its persistent accumulator."""
        async with self._ble_lock:
            try:
                await self._async_connect()
                semantic = await self.client.async_reset_semantic_accumulator()
                base = self.data or NrfClawData()
                values = dict(base.semantic_values)
                values[(semantic.capability_id, semantic.channel)] = semantic
                updated = replace(
                    base,
                    semantic_values=values,
                    semantic_total_reset_writable=True,
                )
                self.async_set_updated_data(updated)
                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s semantic total reset committed but LOW_POWER disconnect hint failed (%s)",
                        self.address, err,
                    )
                return updated
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(f"Direct accumulated-total reset failed: {err}") from err
            except Exception as err:
                raise HomeAssistantError(f"Direct accumulated-total reset failed: {err}") from err
            finally:
                await self.client.async_disconnect()

    async def async_write_accel_event_mode(self, mode: int) -> NrfClawData:
        """Persist one exclusive Direct accelerometer event mode with readback."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError as err:
                    _LOGGER.warning(
                        "Stale Direct-NDP GATT layout for %s during sensor/event write (%s); "
                        "clearing BlueZ cache",
                        self.address,
                        err,
                    )
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                runtime_mode, persisted_mode = (
                    await self.client.async_set_accel_event_mode(mode)
                )
                if runtime_mode != mode or persisted_mode != mode:
                    raise HomeAssistantError(
                        "Accelerometer event-mode readback does not match requested value"
                    )

                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    accel_event_mode=runtime_mode,
                    accel_event_persisted_mode=persisted_mode,
                    accel_event_control_writable=True,
                )
                self.async_set_updated_data(updated)

                try:
                    armed = await self.client.async_prepare_low_power_disconnect()
                    if not armed:
                        _LOGGER.warning(
                            "nRFClaw %s sensor/event write committed but LOW_POWER "
                            "disconnect hint was not armed",
                            self.address,
                        )
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s sensor/event write committed but LOW_POWER "
                        "disconnect hint failed (%s)",
                        self.address,
                        err,
                    )
                return updated

            except HomeAssistantError:
                raise
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (
                BleakConnectionError,
                BleakError,
                TimeoutError,
                ConnectionError,
            ) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(
                    f"Direct sensor/event configuration failed: {err}"
                ) from err
            except Exception as err:
                raise HomeAssistantError(
                    f"Direct sensor/event configuration failed: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    async def async_write_accel_event_sensitivity(
        self, sensitivity: int
    ) -> NrfClawData:
        """Persist a LOW/NORMAL/HIGH Direct event sensitivity preset."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError as err:
                    _LOGGER.warning(
                        "Stale Direct-NDP GATT layout for %s during sensitivity write (%s); clearing BlueZ cache",
                        self.address,
                        err,
                    )
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                readback = await self.client.async_set_accel_event_sensitivity(
                    sensitivity
                )
                if readback != sensitivity:
                    raise HomeAssistantError(
                        "Event sensitivity readback does not match requested value"
                    )

                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    accel_event_sensitivity=readback,
                    accel_event_sensitivity_persisted=True,
                    accel_event_sensitivity_writable=True,
                )
                self.async_set_updated_data(updated)

                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s sensitivity write committed but LOW_POWER disconnect hint failed (%s)",
                        self.address,
                        err,
                    )
                return updated
            except HomeAssistantError:
                raise
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(
                    f"Direct event sensitivity configuration failed: {err}"
                ) from err
            except Exception as err:
                raise HomeAssistantError(
                    f"Direct event sensitivity configuration failed: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    async def async_write_hall_mode(self, mode: int, channel: int) -> NrfClawData:
        """Persist Direct Hall behavior and update the local snapshot."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError as err:
                    _LOGGER.warning(
                        "Stale Direct-NDP GATT layout for %s during Hall write (%s); clearing BlueZ cache",
                        self.address,
                        err,
                    )
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                runtime_mode, runtime_channel = (
                    await self.client.async_set_direct_hall_control(mode, channel)
                )
                expected_channel = channel if mode == 1 else 1
                if runtime_mode != mode or runtime_channel != expected_channel:
                    raise HomeAssistantError(
                        "Hall mode readback does not match requested value"
                    )

                base = self.data or NrfClawData()
                updated = replace(
                    base,
                    hall_mode=runtime_mode,
                    hall_channel=runtime_channel,
                    hall_control_persisted_mode=runtime_mode,
                    hall_control_persisted_channel=runtime_channel,
                    hall_control_writable=True,
                    hall_value=None if runtime_mode == 0 else 0,
                )
                self.async_set_updated_data(updated)

                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s Hall write committed but LOW_POWER disconnect hint failed (%s)",
                        self.address,
                        err,
                    )
                return updated
            except HomeAssistantError:
                raise
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(
                    f"Direct Hall configuration failed: {err}"
                ) from err
            except Exception as err:
                raise HomeAssistantError(
                    f"Direct Hall configuration failed: {err}"
                ) from err
            finally:
                await self.client.async_disconnect()

    async def async_reset_hall_counter(self) -> NrfClawData:
        """Reset the active Hall value and confirm the command completed."""
        async with self._ble_lock:
            try:
                try:
                    await self._async_connect()
                except NrfClawGattLayoutError:
                    self.client.invalidate_gatt_cache()
                    await self.client.async_disconnect()
                    await clear_cache(self.address)
                    await self._async_connect(active_scan=True)

                await self.client.async_reset_direct_hall_value()
                base = self.data or NrfClawData()
                updated = replace(base, hall_value=0)
                self.async_set_updated_data(updated)
                try:
                    await self.client.async_prepare_low_power_disconnect()
                except Exception as err:
                    _LOGGER.warning(
                        "nRFClaw %s Hall reset completed but LOW_POWER disconnect hint failed (%s)",
                        self.address,
                        err,
                    )
                return updated
            except PermissionError as err:
                raise HomeAssistantError(str(err)) from err
            except (BleakConnectionError, BleakError, TimeoutError, ConnectionError) as err:
                self.client.invalidate_gatt_cache()
                raise HomeAssistantError(f"Direct Hall reset failed: {err}") from err
            except Exception as err:
                raise HomeAssistantError(f"Direct Hall reset failed: {err}") from err
            finally:
                await self.client.async_disconnect()

    async def async_shutdown(self) -> None:
        await self.client.async_disconnect()
        self._event_listeners.clear()
        await super().async_shutdown()
