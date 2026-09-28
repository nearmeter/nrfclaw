"""B7.4 real Home Assistant entity classes for NinaLink."""

from __future__ import annotations

import asyncio

from typing import Any

from homeassistant.components.binary_sensor import (
    BinarySensorDeviceClass,
    BinarySensorEntity,
)
from homeassistant.components.event import EventDeviceClass, EventEntity
from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import PERCENTAGE
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

try:
    from .battery import battery_percentage_from_voltage
    from .const import DOMAIN
    from .ninalink_entity_model import (
        EventIdGate,
        find_entity,
        iter_platform_entities,
        matching_events,
    )
except ImportError:  # host gate
    from battery import battery_percentage_from_voltage
    from const import DOMAIN
    from ninalink_entity_model import (
        EventIdGate,
        find_entity,
        iter_platform_entities,
        matching_events,
    )


_SENSOR_CLASS_ALIASES = {
    "carbon_dioxide": "co2",
    "pressure": "atmospheric_pressure",
}


def _enum_or_none(enum_cls, value):
    if value is None:
        return None
    value = _SENSOR_CLASS_ALIASES.get(value, value)
    try:
        return enum_cls(value)
    except (TypeError, ValueError):
        return None


def _device_info(device: dict[str, Any], bridge_device_id: str) -> DeviceInfo:
    return DeviceInfo(
        identifiers={(DOMAIN, device["device_key"])},
        name=device["name"],
        manufacturer=device.get("manufacturer"),
        model=device.get("model"),
        via_device_id=bridge_device_id,
    )


class NinaLinkEntity(CoordinatorEntity):
    """Common NinaLink entity bound to one projected semantic key."""

    _attr_has_entity_name = True

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator)
        self._device = device
        self._description = description
        self._key = description["unique_key"]

        self._attr_unique_id = f"ninalink:{self._key}"
        self._attr_name = description["name"]
        self._attr_device_info = _device_info(device, bridge_device_id)

    @property
    def description(self) -> dict[str, Any]:
        current = find_entity(self.coordinator.data, self._key)
        return current if current is not None else self._description

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        current = self.description
        return {
            "node_online": current.get("node_online"),
            "last_seen_age_s": current.get("last_seen_age_s"),
            "offline_after_s": current.get("offline_after_s"),
            "expected_report_interval_s": current.get(
                "expected_report_interval_s"
            ),
        }

    @property
    def available(self) -> bool:
        current = find_entity(self.coordinator.data, self._key)
        return bool(
            super().available
            and current is not None
            and current.get("available", False)
        )


class NinaLinkBridgeNetworkIdSensor(CoordinatorEntity, SensorEntity):
    """Read-only NinaLink network identifier used to provision more nodes."""

    _attr_has_entity_name = True
    _attr_name = "NinaLink network ID"
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: Any) -> None:
        super().__init__(coordinator)
        bridge_key = coordinator.data["bridge_key"]
        self._attr_unique_id = f"{bridge_key}:ninalink_network_id"
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, bridge_key)})

    @property
    def _bridge(self) -> dict[str, Any]:
        data = self.coordinator.data or {}
        return data.get("model", {}).get("bridge", {})

    @property
    def native_value(self) -> str | None:
        value = self._bridge.get("network_id")
        if value is None:
            return None
        return f"0x{int(value) & 0xFFFF:04X}"

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        bridge = self._bridge
        return {
            "network_id_decimal": bridge.get("network_id"),
            "persisted": bridge.get("network_persisted"),
            "foreign_rx": bridge.get("foreign_rx"),
        }


class NinaLinkRadioDiagnosticSensor(CoordinatorEntity, SensorEntity):
    """Last LoRa RF quality measured by the bridge for one NinaLink node."""

    _attr_has_entity_name = True
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        bridge_device_id: str,
        *,
        metric: str,
    ) -> None:
        super().__init__(coordinator)
        self._node_id_raw = int(device["node_id_raw"])
        self._metric = metric
        suffix = "lora_rssi" if metric == "rssi_dbm" else "lora_snr"
        self._attr_unique_id = f"ninalink:{self._node_id_raw:08X}:{suffix}"
        self._attr_device_info = _device_info(device, bridge_device_id)
        if metric == "rssi_dbm":
            self._attr_name = "LoRa RSSI"
            self._attr_device_class = SensorDeviceClass.SIGNAL_STRENGTH
            self._attr_native_unit_of_measurement = "dBm"
        elif metric == "snr_db":
            self._attr_name = "LoRa SNR"
            self._attr_native_unit_of_measurement = "dB"
        else:
            raise ValueError(f"Unsupported NinaLink radio metric: {metric}")

    @property
    def _current_device(self) -> dict[str, Any] | None:
        data = self.coordinator.data or {}
        for device in data.get("ha", {}).get("devices", []):
            if int(device.get("node_id_raw", -1)) == self._node_id_raw:
                return device
        return None

    @property
    def available(self) -> bool:
        device = self._current_device
        return bool(
            super().available
            and device is not None
            and device.get("online", False)
            and device.get(self._metric) is not None
        )

    @property
    def native_value(self):
        device = self._current_device
        if device is None:
            return None
        return device.get(self._metric)

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        device = self._current_device or {}
        return {
            "node_online": device.get("online"),
            "last_seen_age_s": device.get("age_s"),
            "offline_after_s": device.get("offline_after_s"),
            "expected_report_interval_s": device.get("expected_report_interval_s"),
        }


class NinaLinkSensor(NinaLinkEntity, SensorEntity):
    """Projected MEASUREMENT / COUNTER / POSITION entity."""

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator, device, description, bridge_device_id)
        self._attr_device_class = _enum_or_none(
            SensorDeviceClass,
            description.get("device_class"),
        )
        self._attr_state_class = _enum_or_none(
            SensorStateClass,
            description.get("state_class"),
        )
        self._attr_native_unit_of_measurement = description.get("native_unit")
        cap_id = int(description.get("capability_id_raw", -1))
        if cap_id in (0x0001, 0x0003):
            self._attr_suggested_display_precision = 2
        elif cap_id in (0x0601, 0x0606):
            self._attr_suggested_display_precision = 3

    @property
    def native_value(self):
        return self.description.get("value")


class NinaLinkBatteryLevelSensor(CoordinatorEntity, SensorEntity):
    """Synthetic HA battery percentage derived from BATTERY_VOLTAGE."""

    _attr_has_entity_name = True
    _attr_name = "Battery level"
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_native_unit_of_measurement = PERCENTAGE
    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        voltage_description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator)
        self._voltage_key = voltage_description["unique_key"]
        self._attr_unique_id = f"ninalink:{int(device['node_id_raw']):08X}:battery_level"
        self._attr_device_info = _device_info(device, bridge_device_id)

    @property
    def _voltage_description(self) -> dict[str, Any] | None:
        return find_entity(self.coordinator.data, self._voltage_key)

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        current = self._voltage_description or {}
        return {
            "node_online": current.get("node_online"),
            "last_seen_age_s": current.get("last_seen_age_s"),
            "offline_after_s": current.get("offline_after_s"),
            "expected_report_interval_s": current.get(
                "expected_report_interval_s"
            ),
        }

    @property
    def available(self) -> bool:
        current = self._voltage_description
        return bool(
            super().available
            and current is not None
            and current.get("available", False)
            and current.get("value") is not None
        )

    @property
    def native_value(self) -> int | None:
        current = self._voltage_description
        if current is None:
            return None
        return battery_percentage_from_voltage(current.get("value"))


class NinaLinkBinarySensor(NinaLinkEntity, BinarySensorEntity):
    """Projected boolean STATE / STATUS entity."""

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator, device, description, bridge_device_id)
        self._attr_device_class = _enum_or_none(
            BinarySensorDeviceClass,
            description.get("device_class"),
        )

    @property
    def available(self) -> bool:
        # B7.6f2m6c1e Hall state is meaningful only for HALL1/HALL2.
        if not super().available:
            return False
        cap_id = int(self._description.get("capability_id_raw", -1))
        if cap_id != 0x0301:
            return True

        node_id = int(self._device.get("node_id_raw", -1))
        mode_cap = _raw_capability(
            self.coordinator.data,
            node_id,
            0x0802,
            0,
        )
        if mode_cap is None:
            return False
        raw_mode = mode_cap.get("typed_value", mode_cap.get("value"))
        try:
            mode_value = int(raw_mode)
        except (TypeError, ValueError):
            return False
        return mode_value in (1, 2)

    @property
    def is_on(self) -> bool | None:
        value = self.description.get("value")
        if value is None:
            return None
        return bool(value)


class NinaLinkEvent(NinaLinkEntity, EventEntity):
    """Projected NinaLink EVENT with event-id replay suppression."""

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator, device, description, bridge_device_id)

        self._node_id_raw = int(device["node_id_raw"])
        self._capability_id_raw = int(description["capability_id_raw"])
        self._channel = int(description["channel"])
        self._event_type = description["event_type"]

        self._attr_event_types = list(description["event_types"])
        self._attr_device_class = _enum_or_none(
            EventDeviceClass,
            description.get("event_device_class"),
        )

        self._event_gate = EventIdGate()
        self._event_gate.prime(
            matching_events(
                coordinator.data,
                self._node_id_raw,
                self._capability_id_raw,
                self._channel,
            )
        )

    async def async_get_last_event_data(self):
        """Do not restore a stale HA EventEntity occurrence on reload/re-add."""
        return None

    def _handle_coordinator_update(self) -> None:
        data = self.coordinator.data

        if data and data.get("stream_reset"):
            self._event_gate.reset_stream()

        for event in matching_events(
            data,
            self._node_id_raw,
            self._capability_id_raw,
            self._channel,
        ):
            event_id = int(event["event_id"])
            if not self._event_gate.accept(event_id):
                continue

            self._trigger_event(
                self._event_type,
                {
                    "event_id": event_id,
                    "node_id": event["node_id"],
                    "capability_id": event["capability_id"],
                    "channel": event["channel"],
                    "value": event.get("value"),
                    "raw": event.get("raw"),
                    "sequence": event.get("sequence"),
                },
            )

        # CoordinatorEntity performs the single state write after any event
        # trigger(s), matching EventEntity's required _trigger_event + write.
        super()._handle_coordinator_update()



def _raw_node_for_sensor(data: dict[str, Any] | None, node_id: int) -> dict[str, Any] | None:
    if not data:
        return None
    for node in data.get("model", {}).get("nodes", []):
        if int(node.get("node_id_raw", -1)) == int(node_id):
            return node
    return None


def _raw_capability(
    data: dict[str, Any] | None,
    node_id: int,
    capability_id: int,
    channel: int = 0,
) -> dict[str, Any] | None:
    node = _raw_node_for_sensor(data, node_id)
    if node is None:
        return None
    for cap in node.get("capabilities", []):
        if (
            int(cap.get("capability_id_raw", -1)) == int(capability_id)
            and int(cap.get("channel", -1)) == int(channel)
        ):
            return cap
    return None


def _inventory_has(
    data: dict[str, Any] | None,
    node_id: int,
    capability_id: int,
    channel: int = 0,
) -> bool:
    node = _raw_node_for_sensor(data, node_id)
    if node is None:
        return False
    return any(
        int(item.get("capability_id_raw", -1)) == int(capability_id)
        and int(item.get("channel", -1)) == int(channel)
        for item in node.get("inventory", [])
    )


def _hall_counter_value(data: dict[str, Any] | None, node_id: int) -> int | None:
    mode_cap = _raw_capability(data, node_id, 0x0802, 0)
    mode = None
    if mode_cap is not None:
        raw_mode = mode_cap.get("typed_value", mode_cap.get("value"))
        try:
            mode = int(raw_mode) if raw_mode is not None else None
        except (TypeError, ValueError):
            mode = None

    # m6a HALL_MODE: 0 Off, 1 HALL1, 2 HALL2, 3 Quadrature.
    if mode == 0:
        return None

    wanted = 0x0303 if mode == 3 else 0x0302 if mode in (1, 2) else None
    if wanted is not None:
        cap = _raw_capability(data, node_id, wanted, 0)
        if cap is None:
            return None
        value = cap.get("typed_value", cap.get("value"))
        return None if value is None else int(value)

    # If HALL_MODE has not yet entered the cache, show a value only if exactly
    # one authoritative Hall quantity is present. Never guess between two.
    present = []
    for cid in (0x0302, 0x0303):
        cap = _raw_capability(data, node_id, cid, 0)
        if cap is None:
            continue
        value = cap.get("typed_value", cap.get("value"))
        if value is not None:
            present.append(int(value))
    return present[0] if len(present) == 1 else None


class NinaLinkHallCounterSensor(CoordinatorEntity, SensorEntity):
    """One stable HA Counter for single-Hall and quadrature modes."""

    _attr_has_entity_name = True
    _attr_name = "Counter"
    _attr_native_unit_of_measurement = "count"

    def __init__(self, coordinator: Any, device: dict[str, Any], bridge_device_id: str) -> None:
        super().__init__(coordinator)
        self._node_id = int(device["node_id_raw"])
        self._attr_unique_id = f"ninalink:{self._node_id:08X}:hall_counter"
        self._attr_device_info = _device_info(device, bridge_device_id)

    @property
    def available(self) -> bool:
        device = next(
            (
                d for d in (self.coordinator.data or {}).get("ha", {}).get("devices", [])
                if int(d.get("node_id_raw", -1)) == self._node_id
            ),
            None,
        )
        return bool(
            super().available
            and device is not None
            and bool(device.get("online", True))
            and self.native_value is not None
        )

    @property
    def native_value(self) -> int | None:
        return _hall_counter_value(self.coordinator.data, self._node_id)


MOTION_CAPABILITY_ID = 0x0200
EVENT_DETECTION_CAPABILITY_ID = 0x0800
MOTION_EVENT_MODE = 1
MOTION_HOLD_SECONDS = 5.0


def _motion_classifier_enabled(data: dict[str, Any] | None, node_id: int) -> bool:
    """Return the best authoritative indication that Motion classifier is active."""
    if not data:
        return False

    cfg = _raw_capability(
        data,
        node_id,
        EVENT_DETECTION_CAPABILITY_ID,
        0,
    )
    if cfg is not None:
        raw = cfg.get("typed_value", cfg.get("value"))
        try:
            return int(raw) == MOTION_EVENT_MODE
        except (TypeError, ValueError):
            return False

    for node in data.get("model", {}).get("nodes", []):
        if int(node.get("node_id_raw", -1)) != node_id:
            continue
        for inv in node.get("inventory", []):
            if (
                int(inv.get("capability_id_raw", -1)) == MOTION_CAPABILITY_ID
                and int(inv.get("channel", -1)) == 0
            ):
                return bool(int(inv.get("state_flags", 0)) & 0x04)
    return False


def _motion_node_online(data: dict[str, Any] | None, node_id: int) -> bool:
    if not data:
        return False
    for device in data.get("ha", {}).get("devices", []):
        if int(device.get("node_id_raw", -1)) == node_id:
            return bool(device.get("online", True))
    return False


class NinaLinkMotionBinarySensor(NinaLinkBinarySensor):
    """Direct-parity 5-second Motion state driven by NinaLink CAP_EVENT."""

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator, device, description, bridge_device_id)
        self._node_id_raw = int(device["node_id_raw"])
        self._channel = int(description["channel"])
        self._motion = False
        self._clear_task: asyncio.Task | None = None
        self._event_gate = EventIdGate()
        self._event_gate.prime(
            matching_events(
                coordinator.data,
                self._node_id_raw,
                MOTION_CAPABILITY_ID,
                self._channel,
            )
        )

    @property
    def available(self) -> bool:
        return bool(
            _motion_node_online(self.coordinator.data, self._node_id_raw)
            and _motion_classifier_enabled(
                self.coordinator.data, self._node_id_raw
            )
        )

    @property
    def is_on(self) -> bool:
        return self._motion

    def _arm_clear(self) -> None:
        if self.hass is None:
            return
        if self._clear_task is not None and not self._clear_task.done():
            self._clear_task.cancel()
        self._clear_task = self.hass.async_create_task(
            self._async_clear_motion(),
            f"nRFClaw NinaLink Motion clear {self._node_id_raw:08X}",
        )

    async def _async_clear_motion(self) -> None:
        try:
            await asyncio.sleep(MOTION_HOLD_SECONDS)
        except asyncio.CancelledError:
            return
        self._motion = False
        self.async_write_ha_state()

    def _handle_coordinator_update(self) -> None:
        data = self.coordinator.data

        if data and data.get("stream_reset"):
            self._event_gate.reset_stream()
            self._motion = False
            if self._clear_task is not None and not self._clear_task.done():
                self._clear_task.cancel()

        for event in matching_events(
            data,
            self._node_id_raw,
            MOTION_CAPABILITY_ID,
            self._channel,
        ):
            event_id = int(event["event_id"])
            if not self._event_gate.accept(event_id):
                continue
            raw = event.get("value", event.get("raw", 0))
            if not bool(raw):
                continue
            self._motion = True
            self._arm_clear()

        # Skip NinaLinkBinarySensor's retained-state view: Motion is an
        # occurrence-driven local hold in HA.
        CoordinatorEntity._handle_coordinator_update(self)

    async def async_will_remove_from_hass(self) -> None:
        if self._clear_task is not None and not self._clear_task.done():
            self._clear_task.cancel()
        await super().async_will_remove_from_hass()


class NinaLinkMotionEvent(NinaLinkEvent):
    """Motion occurrence EventEntity matching Direct mode presentation."""

    @property
    def available(self) -> bool:
        return bool(
            _motion_node_online(self.coordinator.data, self._node_id_raw)
            and _motion_classifier_enabled(
                self.coordinator.data, self._node_id_raw
            )
        )


def _motion_event_description(description: dict[str, Any]) -> dict[str, Any]:
    out = dict(description)
    out["unique_key"] = f"{description['unique_key']}:event"
    out["name"] = "Motion"
    out["platform"] = "event"
    out["event_type"] = "detected"
    out["event_types"] = ["detected"]
    out["event_device_class"] = "motion"
    out["available"] = True
    return out


VIBRATION_EVENT_CAPABILITY_ID = 0x020C
VIBRATION_MONITORING_CAPABILITY_ID = 0x0803

VIBRATION_EVENT_TYPES = {
    1: "vibration_warning",
    2: "vibration_alarm",
    3: "machine_on",
    4: "machine_off",
    5: "vibration_learn_complete",
}


def _vib_auto_enabled(data: dict[str, Any] | None, node_id: int) -> bool:
    if not data:
        return False

    cfg = _raw_capability(
        data,
        node_id,
        VIBRATION_MONITORING_CAPABILITY_ID,
        0,
    )
    if cfg is not None:
        raw = cfg.get("typed_value", cfg.get("value"))
        try:
            return bool(int(raw))
        except (TypeError, ValueError):
            return False

    for node in data.get("model", {}).get("nodes", []):
        if int(node.get("node_id_raw", -1)) != node_id:
            continue
        for inv in node.get("inventory", []):
            if (
                int(inv.get("capability_id_raw", -1))
                == VIBRATION_EVENT_CAPABILITY_ID
                and int(inv.get("channel", -1)) == 0
            ):
                return bool(int(inv.get("state_flags", 0)) & 0x04)

    return False


class NinaLinkVibrationEvent(NinaLinkEvent):
    """One VIB_AUTO semantic event stream using Direct-compatible event names."""

    def __init__(
        self,
        coordinator: Any,
        device: dict[str, Any],
        description: dict[str, Any],
        bridge_device_id: str,
    ) -> None:
        super().__init__(coordinator, device, description, bridge_device_id)
        self._attr_name = "Vibration events"
        self._attr_event_types = list(VIBRATION_EVENT_TYPES.values())

    @property
    def available(self) -> bool:
        return bool(
            _motion_node_online(self.coordinator.data, self._node_id_raw)
            and _vib_auto_enabled(
                self.coordinator.data, self._node_id_raw
            )
        )

    def _handle_coordinator_update(self) -> None:
        data = self.coordinator.data

        if data and data.get("stream_reset"):
            self._event_gate.reset_stream()

        for event in matching_events(
            data,
            self._node_id_raw,
            VIBRATION_EVENT_CAPABILITY_ID,
            self._channel,
        ):
            event_id = int(event["event_id"])
            if not self._event_gate.accept(event_id):
                continue

            raw = event.get("value", event.get("raw"))
            try:
                event_code = int(raw)
            except (TypeError, ValueError):
                continue

            event_type = VIBRATION_EVENT_TYPES.get(event_code)
            if event_type is None:
                continue

            self._trigger_event(
                event_type,
                {
                    "event_id": event_id,
                    "node_id": event["node_id"],
                    "capability_id": event["capability_id"],
                    "channel": event["channel"],
                    "value": event.get("value"),
                    "raw": event.get("raw"),
                    "event_code": event_code,
                    "sequence": event.get("sequence"),
                },
            )

        CoordinatorEntity._handle_coordinator_update(self)


def build_sensor_entities(
    coordinator: Any,
    bridge_device_id: str,
) -> list[SensorEntity]:
    entities: list[SensorEntity] = [
        NinaLinkBridgeNetworkIdSensor(coordinator)
    ]
    data = coordinator.data or {}

    for device in data.get("ha", {}).get("devices", []):
        entities.append(
            NinaLinkRadioDiagnosticSensor(
                coordinator,
                device,
                bridge_device_id,
                metric="rssi_dbm",
            )
        )
        entities.append(
            NinaLinkRadioDiagnosticSensor(
                coordinator,
                device,
                bridge_device_id,
                metric="snr_db",
            )
        )

    for device, desc in iter_platform_entities(data, "sensor"):
        cap_id = int(desc.get("capability_id_raw", -1))
        if cap_id in (0x0302, 0x0303):
            continue

        entities.append(
            NinaLinkSensor(
                coordinator,
                device,
                desc,
                bridge_device_id,
            )
        )

        if (
            cap_id == 0x0001
            and int(desc.get("channel", -1)) == 0
        ):
            entities.append(
                NinaLinkBatteryLevelSensor(
                    coordinator,
                    device,
                    desc,
                    bridge_device_id,
                )
            )

    for device in data.get("ha", {}).get("devices", []):
        node_id = int(device.get("node_id_raw", -1))
        has_hall = (
            _inventory_has(data, node_id, 0x0302, 0)
            or _inventory_has(data, node_id, 0x0303, 0)
        )
        if has_hall:
            entities.append(
                NinaLinkHallCounterSensor(
                    coordinator,
                    device,
                    bridge_device_id,
                )
            )

    return entities

def build_binary_sensor_entities(
    coordinator: Any,
    bridge_device_id: str,
) -> list[BinarySensorEntity]:
    entities: list[BinarySensorEntity] = []
    for device, desc in iter_platform_entities(
        coordinator.data, "binary_sensor"
    ):
        if (
            int(desc.get("capability_id_raw", -1)) == MOTION_CAPABILITY_ID
            and int(desc.get("channel", -1)) == 0
        ):
            entities.append(
                NinaLinkMotionBinarySensor(
                    coordinator, device, desc, bridge_device_id
                )
            )
        else:
            entities.append(
                NinaLinkBinarySensor(
                    coordinator, device, desc, bridge_device_id
                )
            )
    return entities



def build_event_entities(
    coordinator: Any,
    bridge_device_id: str,
) -> list[EventEntity]:
    entities: list[EventEntity] = []

    for device, desc in iter_platform_entities(
        coordinator.data, "event"
    ):
        if (
            int(desc.get("capability_id_raw", -1))
            == VIBRATION_EVENT_CAPABILITY_ID
            and int(desc.get("channel", -1)) == 0
        ):
            entities.append(
                NinaLinkVibrationEvent(
                    coordinator, device, desc, bridge_device_id
                )
            )
        else:
            entities.append(
                NinaLinkEvent(
                    coordinator, device, desc, bridge_device_id
                )
            )

    for device, desc in iter_platform_entities(
        coordinator.data, "binary_sensor"
    ):
        if (
            int(desc.get("capability_id_raw", -1))
            == MOTION_CAPABILITY_ID
            and int(desc.get("channel", -1)) == 0
        ):
            entities.append(
                NinaLinkMotionEvent(
                    coordinator,
                    device,
                    _motion_event_description(desc),
                    bridge_device_id,
                )
            )

    return entities
