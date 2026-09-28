"""CONFIG controls for remote NinaLink Nodes."""
from __future__ import annotations
import asyncio
import logging
from typing import Any

from homeassistant.components.select import SelectEntity
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

WRITABLE = 0x08
SUPPORTED = 0x01
HALL_MODE_CAPABILITY = 0x0802
ENUM8 = 8

_HALL_OPTIONS = ["Off", "Counter HALL1", "Counter HALL2", "Quadrature"]
_HALL_OPTION_TO_VALUE = {
    "Off": 0,
    "Counter HALL1": 1,
    "Counter HALL2": 2,
    "Quadrature": 3,
}
_HALL_VALUE_TO_OPTION = {v: k for k, v in _HALL_OPTION_TO_VALUE.items()}


def _raw_node(data: dict[str, Any] | None, node_id: int) -> dict[str, Any] | None:
    if not data:
        return None
    for node in data.get("model", {}).get("nodes", []):
        if int(node.get("node_id_raw", -1)) == int(node_id):
            return node
    return None


def _ha_device(data: dict[str, Any] | None, node_id: int) -> dict[str, Any] | None:
    if not data:
        return None
    for device in data.get("ha", {}).get("devices", []):
        if int(device.get("node_id_raw", -1)) == int(node_id):
            return device
    return None


def _inventory_entry(data, node_id: int, capability_id: int, channel: int):
    node = _raw_node(data, node_id)
    if node is None:
        return None
    for desc in node.get("inventory", []):
        if (
            int(desc.get("capability_id_raw", -1)) == int(capability_id)
            and int(desc.get("channel", -1)) == int(channel)
        ):
            return desc
    return None


def _current_typed_value(data, node_id: int, capability_id: int, channel: int):
    node = _raw_node(data, node_id)
    if node is None:
        return None
    for cap in node.get("capabilities", []):
        if (
            int(cap.get("capability_id_raw", -1)) == int(capability_id)
            and int(cap.get("channel", -1)) == int(channel)
        ):
            value = cap.get("typed_value", cap.get("value"))
            if value is None:
                return None
            try:
                return int(value)
            except (TypeError, ValueError):
                return None
    return None


def _node_online(data, node_id: int) -> bool:
    device = _ha_device(data, node_id)
    return bool(device is not None and device.get("online", True))


def _device_info(device: dict[str, Any], bridge_device_id: str) -> DeviceInfo:
    return DeviceInfo(
        identifiers={(DOMAIN, device["device_key"])},
        name=device.get("name"),
        manufacturer=device.get("manufacturer"),
        model=device.get("model"),
        via_device_id=bridge_device_id,
    )



BOOL = 1
EVENT_DETECTION_CAPABILITY = 0x0800
EVENT_SENSITIVITY_CAPABILITY = 0x0801
VIBRATION_MONITORING_CAPABILITY = 0x0803
VIBRATION_SENSITIVITY_CAPABILITY = 0x0804

_EVENT_OPTIONS = ["Off", "Motion", "Tap", "Fall"]
_EVENT_OPTION_TO_VALUE = {
    "Off": 0,
    "Motion": 1,
    "Tap": 2,
    "Fall": 3,
}
_EVENT_VALUE_TO_OPTION = {v: k for k, v in _EVENT_OPTION_TO_VALUE.items()}

_SENSITIVITY_OPTIONS = ["Low", "Normal", "High"]
_SENSITIVITY_OPTION_TO_VALUE = {
    "Low": 0,
    "Normal": 1,
    "High": 2,
}
_SENSITIVITY_VALUE_TO_OPTION = {
    v: k for k, v in _SENSITIVITY_OPTION_TO_VALUE.items()
}

_VIBRATION_OPTIONS = ["Off", "Auto"]
_VIBRATION_OPTION_TO_VALUE = {"Off": 0, "Auto": 1}
_VIBRATION_VALUE_TO_OPTION = {0: "Off", 1: "Auto"}


def _remote_control_supported(
    data: dict[str, Any] | None,
    node_id: int,
    capability_id: int,
    value_type: int,
) -> bool:
    desc = _inventory_entry(data, node_id, capability_id, 0)
    return bool(
        desc is not None
        and int(desc.get("value_type", -1)) == int(value_type)
        and (int(desc.get("behavior_flags", 0)) & WRITABLE)
        and (int(desc.get("state_flags", 0)) & SUPPORTED)
    )


class _NinaLinkRemoteSelect(CoordinatorEntity, SelectEntity):
    """Authoritative remote CAP_SET Select with local pending presentation."""

    _capability_id: int
    _value_type: int
    _option_to_value: dict[str, int]
    _value_to_option: dict[int, str]
    _query_endpoints: tuple[tuple[int, int], ...] = ()

    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator)
        self._node_id = int(device["node_id_raw"])
        self._attr_unique_id = (
            f"ninalink:{self._node_id:08X}:config:"
            f"0x{self._capability_id:04X}:0"
        )
        self._attr_device_info = _device_info(device, bridge_device_id)

        self._pending_display_value: int | None = None
        self._pending_command_seq: int | None = None
        self._pending_stage: str | None = None
        self._pending_task = None

        self._readback_task = None
        self._readback_signature: tuple[tuple[int, int], ...] | None = None

    def _authoritative_value(self) -> int | None:
        return _current_typed_value(
            self.coordinator.data,
            self._node_id,
            self._capability_id,
            0,
        )

    @property
    def current_option(self) -> str | None:
        if self._pending_display_value is not None:
            return self._value_to_option.get(self._pending_display_value)
        value = self._authoritative_value()
        return None if value is None else self._value_to_option.get(value)

    @property
    def assumed_state(self) -> bool:
        return self._pending_display_value is not None

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        if self._pending_display_value is None:
            return {
                "change_pending": False,
                "state_source": "confirmed",
            }

        return {
            "change_pending": True,
            "state_source": "pending_confirmation",
            "requested_option": self._value_to_option.get(
                self._pending_display_value
            ),
            "command_seq": self._pending_command_seq,
            "sequence_stage": self._pending_stage,
        }

    def _semantic_available(self) -> bool:
        return True

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and _node_online(self.coordinator.data, self._node_id)
            and _remote_control_supported(
                self.coordinator.data,
                self._node_id,
                self._capability_id,
                self._value_type,
            )
            and self._semantic_available()
        )

    def _clear_pending(self) -> None:
        self._pending_display_value = None
        self._pending_command_seq = None
        self._pending_stage = None

    def _readback_needed(self) -> bool:
        if not self._query_endpoints:
            return False
        return any(
            _current_typed_value(
                self.coordinator.data,
                self._node_id,
                cap_id,
                channel,
            )
            is None
            for cap_id, channel in self._query_endpoints
        )

    def _schedule_readback(self) -> None:
        if self.hass is None or not self._readback_needed():
            return
        if self._readback_task is not None and not self._readback_task.done():
            return

        signature = tuple(self._query_endpoints)
        if self._readback_signature == signature:
            return

        self._readback_signature = signature
        self._readback_task = self.hass.async_create_task(
            self._async_queue_readback(signature),
            (
                f"nRFClaw config readback {self._node_id:08X} "
                f"0x{self._capability_id:04X}"
            ),
        )

    async def _async_queue_readback(
        self,
        signature: tuple[tuple[int, int], ...],
    ) -> None:
        last_exc: Exception | None = None
        for delay in (0.0, 2.0, 5.0, 10.0):
            if delay:
                await asyncio.sleep(delay)
            try:
                await self.coordinator.runtime.builder.remote_query_capabilities(
                    self._node_id,
                    list(signature),
                )
                return
            except Exception as exc:
                last_exc = exc

        self._readback_signature = None
        _LOGGER.debug(
            "NinaLink config readback could not be queued for %08X "
            "cap=0x%04X: %s",
            self._node_id,
            self._capability_id,
            last_exc,
        )

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self._schedule_readback()

    async def async_will_remove_from_hass(self) -> None:
        for task in (self._pending_task, self._readback_task):
            if task is not None and not task.done():
                task.cancel()
        await super().async_will_remove_from_hass()

    def _handle_coordinator_update(self) -> None:
        authoritative = self._authoritative_value()

        if (
            self._pending_display_value is not None
            and authoritative == self._pending_display_value
        ):
            self._clear_pending()

        if not self._readback_needed():
            self._readback_signature = None
        self._schedule_readback()
        super()._handle_coordinator_update()

    async def _await_write_confirmation(
        self,
        command_seq: int,
        capability_id: int,
        requested: int,
    ) -> bool:
        """Wait for APP_RESULT OK or authoritative cache confirmation.

        Polling selector 55 is Bridge-BLE-only and creates no LoRa traffic.
        A matching authoritative cache value is also sufficient because m6b
        commits CAP_SET values only after APP_RESULT OK.
        """
        delay = 1.0

        while True:
            if (
                _current_typed_value(
                    self.coordinator.data,
                    self._node_id,
                    capability_id,
                    0,
                )
                == requested
            ):
                return True

            await asyncio.sleep(delay)
            delay = min(delay * 1.5, 10.0)

            try:
                status = (
                    await self.coordinator.runtime.builder
                    .remote_cap_set_status()
                )
            except Exception as exc:
                _LOGGER.debug(
                    "NinaLink remote CAP_SET status read failed for "
                    "%08X seq=%u: %s",
                    self._node_id,
                    command_seq,
                    exc,
                )
                continue

            if int(status.get("command_seq", -1)) != command_seq:
                # Another HA operation may have replaced the Bridge's
                # last-status record. In that case only authoritative cache
                # confirmation is accepted.
                continue

            if bool(status.get("pending", False)):
                continue

            result = int(status.get("result", 255))
            return result == 0

    async def _watch_single_write(
        self,
        command_seq: int,
        requested: int,
    ) -> None:
        ok = await self._await_write_confirmation(
            command_seq,
            self._capability_id,
            requested,
        )
        if ok:
            # Keep the presentation overlay until B5.5 publishes the
            # authoritative cache value.
            return

        if (
            self._pending_command_seq == command_seq
            and self._pending_display_value == requested
        ):
            _LOGGER.warning(
                "NinaLink remote write failed for node %08X cap=0x%04X "
                "seq=%u",
                self._node_id,
                self._capability_id,
                command_seq,
            )
            self._clear_pending()
            self.async_write_ha_state()

    async def _queue_single_write(
        self,
        requested: int,
        *,
        stage: str = "cap_set",
    ) -> None:
        if self._pending_display_value is not None:
            raise HomeAssistantError(
                "A remote configuration change is already pending"
            )

        ticket = await self.coordinator.async_write_remote_capability(
            self._node_id,
            self._capability_id,
            0,
            self._value_type,
            requested,
        )

        command_seq = int(ticket["command_seq"])
        self._pending_display_value = requested
        self._pending_command_seq = command_seq
        self._pending_stage = stage

        self._pending_task = self.hass.async_create_task(
            self._watch_single_write(command_seq, requested),
            (
                f"nRFClaw remote config {self._node_id:08X} "
                f"0x{self._capability_id:04X}"
            ),
        )
        self.async_write_ha_state()

    async def async_select_option(self, option: str) -> None:
        if option not in self._option_to_value:
            raise ValueError(
                f"unsupported {self._attr_name} option: {option}"
            )
        await self._queue_single_write(self._option_to_value[option])


class NinaLinkEventDetectionSelect(_NinaLinkRemoteSelect):
    _attr_name = "Event detection"
    _attr_options = _EVENT_OPTIONS
    _capability_id = EVENT_DETECTION_CAPABILITY
    _value_type = ENUM8
    _option_to_value = _EVENT_OPTION_TO_VALUE
    _value_to_option = _EVENT_VALUE_TO_OPTION
    _query_endpoints = (
        (EVENT_DETECTION_CAPABILITY, 0),
        (EVENT_SENSITIVITY_CAPABILITY, 0),
    )

    def _semantic_available(self) -> bool:
        vib = _current_typed_value(
            self.coordinator.data,
            self._node_id,
            VIBRATION_MONITORING_CAPABILITY,
            0,
        )
        return vib != 1


class NinaLinkEventSensitivitySelect(_NinaLinkRemoteSelect):
    _attr_name = "Event sensitivity"
    _attr_options = _SENSITIVITY_OPTIONS
    _capability_id = EVENT_SENSITIVITY_CAPABILITY
    _value_type = ENUM8
    _option_to_value = _SENSITIVITY_OPTION_TO_VALUE
    _value_to_option = _SENSITIVITY_VALUE_TO_OPTION

    def _semantic_available(self) -> bool:
        vib = _current_typed_value(
            self.coordinator.data,
            self._node_id,
            VIBRATION_MONITORING_CAPABILITY,
            0,
        )
        event_mode = _current_typed_value(
            self.coordinator.data,
            self._node_id,
            EVENT_DETECTION_CAPABILITY,
            0,
        )
        return vib != 1 and event_mode in (1, 2, 3)


class NinaLinkVibrationMonitoringSelect(_NinaLinkRemoteSelect):
    _attr_name = "Vibration monitoring"
    _attr_options = _VIBRATION_OPTIONS
    _capability_id = VIBRATION_MONITORING_CAPABILITY
    _value_type = BOOL
    _option_to_value = _VIBRATION_OPTION_TO_VALUE
    _value_to_option = _VIBRATION_VALUE_TO_OPTION
    _query_endpoints = (
        (VIBRATION_MONITORING_CAPABILITY, 0),
        (VIBRATION_SENSITIVITY_CAPABILITY, 0),
    )

    async def _queue_with_retry(
        self,
        capability_id: int,
        value_type: int,
        value: int,
    ) -> dict[str, Any]:
        last_exc: Exception | None = None
        for delay in (0.0, 2.0, 5.0, 10.0):
            if delay:
                await asyncio.sleep(delay)
            try:
                return await self.coordinator.async_write_remote_capability(
                    self._node_id,
                    capability_id,
                    0,
                    value_type,
                    value,
                )
            except Exception as exc:
                last_exc = exc

        raise HomeAssistantError(
            f"NinaLink remote CAP_SET could not be queued: {last_exc}"
        )

    async def _run_auto_sequence(self, first_seq: int) -> None:
        # Step 1 was already queued by async_select_option():
        # EVENT_DETECTION -> Off. Do not queue VIB_AUTO until that persistent
        # operation is separately confirmed.
        off_ok = await self._await_write_confirmation(
            first_seq,
            EVENT_DETECTION_CAPABILITY,
            0,
        )
        if not off_ok:
            if self._pending_display_value == 1:
                self._clear_pending()
                self.async_write_ha_state()
            return

        if self._pending_display_value != 1:
            return

        # Step 2: only after the first APP_RESULT/cache confirmation.
        try:
            ticket = await self._queue_with_retry(
                VIBRATION_MONITORING_CAPABILITY,
                BOOL,
                1,
            )
        except Exception as exc:
            _LOGGER.warning(
                "NinaLink VIB_AUTO second CAP_SET could not be queued "
                "for node %08X after Event detection Off was confirmed: %s",
                self._node_id,
                exc,
            )
            self._clear_pending()
            self.async_write_ha_state()
            return

        second_seq = int(ticket["command_seq"])
        self._pending_command_seq = second_seq
        self._pending_stage = "vibration_monitoring_auto"
        self.async_write_ha_state()

        auto_ok = await self._await_write_confirmation(
            second_seq,
            VIBRATION_MONITORING_CAPABILITY,
            1,
        )
        if auto_ok:
            # B5.5/cache reconciliation clears the presentation overlay.
            return

        if (
            self._pending_display_value == 1
            and self._pending_command_seq == second_seq
        ):
            self._clear_pending()
            self.async_write_ha_state()

    async def async_select_option(self, option: str) -> None:
        if option not in _VIBRATION_OPTION_TO_VALUE:
            raise ValueError(
                f"unsupported vibration-monitoring option: {option}"
            )

        requested = _VIBRATION_OPTION_TO_VALUE[option]

        if requested == 0:
            await self._queue_single_write(
                0,
                stage="vibration_monitoring_off",
            )
            return

        if self._pending_display_value is not None:
            raise HomeAssistantError(
                "A remote configuration change is already pending"
            )

        # Two separately confirmed persistent CAP_SETs are mandatory:
        #   1) EVENT_DETECTION -> Off
        #   2) wait APP_RESULT/cache confirmation
        #   3) VIBRATION_MONITORING -> Auto
        #   4) wait APP_RESULT/cache confirmation
        first = await self.coordinator.async_write_remote_capability(
            self._node_id,
            EVENT_DETECTION_CAPABILITY,
            0,
            ENUM8,
            0,
        )
        first_seq = int(first["command_seq"])

        self._pending_display_value = 1
        self._pending_command_seq = first_seq
        self._pending_stage = "event_detection_off"
        self._pending_task = self.hass.async_create_task(
            self._run_auto_sequence(first_seq),
            f"nRFClaw VIB_AUTO sequence {self._node_id:08X}",
        )
        self.async_write_ha_state()


class NinaLinkVibrationSensitivitySelect(_NinaLinkRemoteSelect):
    _attr_name = "Vibration sensitivity"
    _attr_options = _SENSITIVITY_OPTIONS
    _capability_id = VIBRATION_SENSITIVITY_CAPABILITY
    _value_type = ENUM8
    _option_to_value = _SENSITIVITY_OPTION_TO_VALUE
    _value_to_option = _SENSITIVITY_VALUE_TO_OPTION

class NinaLinkHallModeSelect(CoordinatorEntity, SelectEntity):
    _attr_has_entity_name = True
    _attr_name = "Hall mode"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _HALL_OPTIONS

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator)
        self._node_id = int(device["node_id_raw"])
        self._attr_unique_id = (
            f"ninalink:{self._node_id:08X}:config:0x0802:0"
        )
        self._attr_device_info = _device_info(device, bridge_device_id)

        self._last_seen_mode: int | None = _current_typed_value(
            coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
        )
        self._last_query_signature: tuple[Any, ...] | None = None
        self._pending_readback_mode: int | None = None
        self._readback_task = None

        # Presentation-only pending state. Never mutate coordinator/cache
        # optimistically.
        self._pending_display_mode: int | None = None
        self._pending_command_seq: int | None = None
        self._pending_result_ok = False
        self._pending_status_task = None

    def _authoritative_mode(self) -> int | None:
        return _current_typed_value(
            self.coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
        )

    @staticmethod
    def _expected_quantity_capability(mode: int | None) -> int | None:
        if mode in (1, 2):
            return 0x0302
        if mode == 3:
            return 0x0303
        return None

    def _authoritative_bundle_confirmed(self, mode: int) -> bool:
        if self._authoritative_mode() != mode:
            return False

        quantity_cap = self._expected_quantity_capability(mode)
        if quantity_cap is None:
            return True

        return (
            _current_typed_value(
                self.coordinator.data,
                self._node_id,
                quantity_cap,
                0,
            )
            is not None
        )

    def _readback_endpoints_for_mode(
        self,
        mode: int | None,
    ) -> list[tuple[int, int]]:
        if mode is None:
            # Bootstrap remains within the B4.11 two-endpoint limit.
            return [(HALL_MODE_CAPABILITY, 0), (0x0302, 0)]

        if mode in (1, 2, 3):
            # Keep this spelling for the m6c1c regression contract.
            counter_cap = 0x0303 if mode == 3 else 0x0302
            return [(HALL_MODE_CAPABILITY, 0), (counter_cap, 0)]

        # Off still gets an explicit authoritative HALL_MODE readback when
        # forced after CAP_SET.
        return [(HALL_MODE_CAPABILITY, 0)]

    @property
    def current_option(self) -> str | None:
        if self._pending_display_mode is not None:
            return _HALL_VALUE_TO_OPTION.get(self._pending_display_mode)

        value = self._authoritative_mode()
        return None if value is None else _HALL_VALUE_TO_OPTION.get(value)

    @property
    def assumed_state(self) -> bool:
        return self._pending_display_mode is not None

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        authoritative = self._authoritative_mode()
        authoritative_option = (
            None
            if authoritative is None
            else _HALL_VALUE_TO_OPTION.get(authoritative)
        )

        if self._pending_display_mode is None:
            return {
                "change_pending": False,
                "state_source": "confirmed",
                "authoritative_option": authoritative_option,
            }

        return {
            "change_pending": True,
            "state_source": (
                "pending_readback"
                if self._pending_result_ok
                else "pending_confirmation"
            ),
            "requested_option": _HALL_VALUE_TO_OPTION.get(
                self._pending_display_mode
            ),
            "authoritative_option": authoritative_option,
            "command_seq": self._pending_command_seq,
        }

    @property
    def available(self) -> bool:
        desc = _inventory_entry(
            self.coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
        )
        return bool(
            super().available
            and _node_online(self.coordinator.data, self._node_id)
            and desc is not None
            and int(desc.get("value_type", -1)) == ENUM8
            and (int(desc.get("behavior_flags", 0)) & WRITABLE)
            and (int(desc.get("state_flags", 0)) & SUPPORTED)
        )

    def _clear_pending_display(self) -> None:
        self._pending_display_mode = None
        self._pending_readback_mode = None
        self._pending_command_seq = None
        self._pending_result_ok = False

    def _status_matches_pending(
        self,
        status: dict[str, Any],
        command_seq: int,
        requested: int,
    ) -> bool:
        # command_seq alone is not a sufficient transaction identity. A stale
        # selector-55 record can reuse the same sequence after Bridge/session
        # churn and must never make the Hall Select revert.
        return bool(
            int(status.get("command_seq", -1)) == int(command_seq)
            and int(status.get("node_id", -1)) == self._node_id
            and int(status.get("capability_id", -1))
            == HALL_MODE_CAPABILITY
            and int(status.get("channel", -1)) == 0
            and int(status.get("value_type", -1)) == ENUM8
            and int(status.get("requested_value", -1)) == int(requested)
        )

    def _readback_needed(self, mode: int | None = None) -> bool:
        if mode is None:
            mode = self._authoritative_mode()

        if mode is None:
            return True

        quantity_cap = self._expected_quantity_capability(mode)
        if quantity_cap is None:
            return False

        return (
            _current_typed_value(
                self.coordinator.data,
                self._node_id,
                quantity_cap,
                0,
            )
            is None
        )

    def _schedule_readback(self, *, force: bool = False) -> None:
        if self.hass is None:
            return

        # Never let a bootstrap/readback query race the Hall CAP_SET. The
        # post-write path owns readback until the write is confirmed.
        if self._pending_command_seq is not None:
            return

        if self._readback_task is not None and not self._readback_task.done():
            return

        mode = self._authoritative_mode()
        if not force and not self._readback_needed(mode):
            self._last_query_signature = None
            return

        endpoints = self._readback_endpoints_for_mode(mode)
        signature = ("background", mode, *tuple(endpoints))

        if not force and signature == self._last_query_signature:
            return

        self._last_query_signature = signature
        self._readback_task = self.hass.async_create_task(
            self._async_background_readback(mode, endpoints),
            f"nRFClaw Hall readback {self._node_id:08X}",
        )

    async def _async_background_readback(
        self,
        mode: int | None,
        endpoints: list[tuple[int, int]],
    ) -> None:
        last_exc: Exception | None = None

        try:
            for delay in (0.0, 5.0, 10.0):
                if delay:
                    await asyncio.sleep(delay)

                if self._pending_command_seq is not None:
                    return

                current_mode = self._authoritative_mode()
                if (
                    current_mode is not None
                    and not self._readback_needed(current_mode)
                ):
                    return

                try:
                    await self.coordinator.runtime.builder.remote_query_capabilities(
                        self._node_id,
                        endpoints,
                    )
                except Exception as exc:
                    last_exc = exc
                    continue

                # Selector 42 only queues the RF query. Give the Node an
                # ordinary contact window to return the authoritative values.
                for _ in range(25):
                    await asyncio.sleep(1.0)
                    if self._pending_command_seq is not None:
                        return
                    current_mode = self._authoritative_mode()
                    if (
                        current_mode is not None
                        and not self._readback_needed(current_mode)
                    ):
                        return

            if last_exc is not None:
                _LOGGER.debug(
                    "NinaLink Hall background readback incomplete for "
                    "node %08X: %s",
                    self._node_id,
                    last_exc,
                )
        finally:
            self._last_query_signature = None
            self._readback_task = None

    def _start_authoritative_readback(
        self,
        command_seq: int,
        requested: int,
    ) -> None:
        if self.hass is None:
            return
        if (
            self._pending_command_seq != command_seq
            or self._pending_display_mode != requested
        ):
            return

        self._pending_result_ok = True
        self._pending_readback_mode = requested

        if self._authoritative_bundle_confirmed(requested):
            self._clear_pending_display()
            self.async_write_ha_state()
            return

        if self._readback_task is not None and not self._readback_task.done():
            return

        endpoints = self._readback_endpoints_for_mode(requested)
        self._readback_task = self.hass.async_create_task(
            self._async_post_write_readback(
                command_seq,
                requested,
                endpoints,
            ),
            f"nRFClaw Hall authoritative readback {self._node_id:08X}",
        )
        self.async_write_ha_state()

    async def _async_post_write_readback(
        self,
        command_seq: int,
        requested: int,
        endpoints: list[tuple[int, int]],
    ) -> None:
        last_exc: Exception | None = None
        need_background = False

        try:
            for delay in (0.0, 3.0, 8.0):
                if delay:
                    await asyncio.sleep(delay)

                if (
                    self._pending_command_seq != command_seq
                    or self._pending_display_mode != requested
                ):
                    return

                if self._authoritative_bundle_confirmed(requested):
                    self._clear_pending_display()
                    self.async_write_ha_state()
                    return

                try:
                    await self.coordinator.runtime.builder.remote_query_capabilities(
                        self._node_id,
                        endpoints,
                    )
                except Exception as exc:
                    last_exc = exc
                    continue

                # Wait for selector-42 result / CAP_REPORT reconciliation.
                for _ in range(25):
                    await asyncio.sleep(1.0)

                    if (
                        self._pending_command_seq != command_seq
                        or self._pending_display_mode != requested
                    ):
                        return

                    if self._authoritative_bundle_confirmed(requested):
                        self._clear_pending_display()
                        self.async_write_ha_state()
                        return

            mode = self._authoritative_mode()

            if mode == requested:
                # The configuration itself is authoritatively confirmed, but
                # its Hall quantity did not arrive yet. Do not snap the Select
                # back. Mark the Select confirmed and leave a background
                # readback to recover Counter/Position.
                _LOGGER.warning(
                    "NinaLink Hall mode confirmed for node %08X but "
                    "quantity readback is still missing: mode=%u cap=0x%04X",
                    self._node_id,
                    requested,
                    self._expected_quantity_capability(requested) or 0,
                )
                self._clear_pending_display()
                self.async_write_ha_state()
                need_background = self._readback_needed(requested)
                return

            _LOGGER.warning(
                "NinaLink Hall CAP_SET reported success but authoritative "
                "readback did not confirm node %08X: requested=%u "
                "authoritative=%s last_error=%s",
                self._node_id,
                requested,
                mode,
                last_exc,
            )
            self._clear_pending_display()
            self.async_write_ha_state()
            need_background = True
        finally:
            self._readback_task = None
            self._last_query_signature = None
            if need_background:
                self._schedule_readback(force=True)

    async def _async_watch_pending_write(
        self,
        command_seq: int,
        requested: int,
    ) -> None:
        """Wait for this exact Hall CAP_SET, then force authoritative readback."""
        delay = 1.0

        while (
            self._pending_display_mode == requested
            and self._pending_command_seq == command_seq
        ):
            # A B5.5 cache commit is already proof that APP_RESULT OK happened.
            if self._authoritative_mode() == requested:
                self._start_authoritative_readback(command_seq, requested)
                return

            await asyncio.sleep(delay)
            delay = min(delay * 1.5, 10.0)

            try:
                status = (
                    await self.coordinator.runtime.builder
                    .remote_cap_set_status()
                )
            except Exception as exc:
                _LOGGER.debug(
                    "NinaLink Hall pending-status read failed for %08X: %s",
                    self._node_id,
                    exc,
                )
                continue

            if not self._status_matches_pending(
                status,
                command_seq,
                requested,
            ):
                # Ignore stale/foreign selector-55 state. In particular, do
                # not revert the Select merely because command_seq happens to
                # match another transaction.
                continue

            if bool(status.get("pending", False)):
                continue

            result = int(status.get("result", 255))
            if result == 0:
                # Keep displaying the requested option until the coordinator
                # receives HALL_MODE plus the mode-specific Hall quantity.
                self._start_authoritative_readback(command_seq, requested)
                return

            # Definitive failure is accepted only for the exact transaction.
            _LOGGER.warning(
                "NinaLink Hall mode write failed for node %08X: "
                "seq=%u result=%u state=%s requested=%u",
                self._node_id,
                command_seq,
                result,
                status.get("state"),
                requested,
            )
            self._clear_pending_display()
            self.async_write_ha_state()
            return

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self._schedule_readback()

    async def async_will_remove_from_hass(self) -> None:
        for task in (self._pending_status_task, self._readback_task):
            if task is not None and not task.done():
                task.cancel()
        await super().async_will_remove_from_hass()

    def _handle_coordinator_update(self) -> None:
        mode = self._authoritative_mode()

        if mode != self._last_seen_mode:
            self._last_seen_mode = mode
            self._last_query_signature = None

        if self._pending_display_mode is not None:
            if mode == self._pending_display_mode:
                self._pending_result_ok = True

                if self._authoritative_bundle_confirmed(
                    self._pending_display_mode
                ):
                    self._clear_pending_display()
                elif self._pending_command_seq is not None:
                    self._start_authoritative_readback(
                        self._pending_command_seq,
                        self._pending_display_mode,
                    )

            # A pending CAP_SET owns the readback path. Do not enqueue a
            # bootstrap query against the old mode.
        else:
            self._schedule_readback()

        super()._handle_coordinator_update()

    async def async_select_option(self, option: str) -> None:
        if option not in _HALL_OPTION_TO_VALUE:
            raise ValueError(f"unsupported Hall mode option: {option}")

        if self._pending_display_mode is not None:
            raise HomeAssistantError(
                "A Hall configuration change is already pending"
            )

        requested = _HALL_OPTION_TO_VALUE[option]

        # Stop local bootstrap retry machinery before queuing CAP_SET. This
        # cannot retract a query already transmitted to the Bridge, but it
        # prevents new readbacks from racing the write.
        if self._readback_task is not None and not self._readback_task.done():
            self._readback_task.cancel()
        self._readback_task = None
        self._last_query_signature = None

        ticket = await self.coordinator.async_write_remote_capability(
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
            ENUM8,
            requested,
        )

        command_seq = int(ticket["command_seq"])
        self._pending_display_mode = requested
        self._pending_readback_mode = requested
        self._pending_command_seq = command_seq
        self._pending_result_ok = False

        if (
            self._pending_status_task is not None
            and not self._pending_status_task.done()
        ):
            self._pending_status_task.cancel()

        self._pending_status_task = self.hass.async_create_task(
            self._async_watch_pending_write(command_seq, requested),
            f"nRFClaw Hall pending {self._node_id:08X}",
        )

        # Presentation only; coordinator/model/cache stays authoritative.
        self.async_write_ha_state()

def build_ninalink_control_entities(
    coordinator: Any,
    bridge_device_id: str,
) -> list[SelectEntity]:
    data = coordinator.data or {}
    entities: list[SelectEntity] = []

    specs = (
        (
            EVENT_DETECTION_CAPABILITY,
            ENUM8,
            NinaLinkEventDetectionSelect,
        ),
        (
            EVENT_SENSITIVITY_CAPABILITY,
            ENUM8,
            NinaLinkEventSensitivitySelect,
        ),
        (
            HALL_MODE_CAPABILITY,
            ENUM8,
            NinaLinkHallModeSelect,
        ),
        (
            VIBRATION_MONITORING_CAPABILITY,
            BOOL,
            NinaLinkVibrationMonitoringSelect,
        ),
        (
            VIBRATION_SENSITIVITY_CAPABILITY,
            ENUM8,
            NinaLinkVibrationSensitivitySelect,
        ),
    )

    for device in data.get("ha", {}).get("devices", []):
        node_id = int(device.get("node_id_raw", -1))
        node = _raw_node(data, node_id)
        if node is None:
            continue
        if not bool(node.get("inventory_valid", False)):
            continue
        if not bool(node.get("inventory_complete", False)):
            continue

        for capability_id, value_type, entity_cls in specs:
            if not _remote_control_supported(
                data,
                node_id,
                capability_id,
                value_type,
            ):
                continue
            entities.append(
                entity_cls(
                    coordinator,
                    device,
                    bridge_device_id,
                )
            )

    return entities
