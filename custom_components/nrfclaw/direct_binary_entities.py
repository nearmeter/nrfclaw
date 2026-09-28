"""Direct-NDP semantic binary sensors aligned with NinaLink Node."""

from __future__ import annotations

import asyncio

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.core import callback
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .client import NrfClawData
from .const import (
    ACCEL_EVENT_MOTION,
    CAP_ACCEL,
    CAP_HALL,
    CAP_TRACKING,
    DOMAIN,
)
from .direct_adv import DirectAdvEvent
from .direct_coordinator import DirectNdpCoordinator
from .direct_semantic_profile import family_supported
from .device_identity import direct_device_name


def build_direct_binary_sensor_entities(coordinator, entry) -> list[BinarySensorEntity]:
    data: NrfClawData = coordinator.data
    entities: list[BinarySensorEntity] = []
    if family_supported(data, CAP_HALL):
        entities.append(DirectHallStateBinarySensor(coordinator, entry))
    if family_supported(data, CAP_ACCEL):
        entities.append(DirectMotionBinarySensor(coordinator, entry))
    if family_supported(data, CAP_TRACKING):
        entities.append(DirectTrackingBinarySensor(coordinator, entry))
    return entities


class DirectSemanticBinarySensor(CoordinatorEntity[DirectNdpCoordinator], BinarySensorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry, key: str) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        data: NrfClawData = self.coordinator.data
        sw_version = data.firmware
        if sw_version is not None and data.firmware_build is not None:
            sw_version = f"{sw_version} build {data.firmware_build}"
        return DeviceInfo(
            identifiers={(DOMAIN, self._entry.unique_id or self.coordinator.address)},
            name=direct_device_name(data.board_name, self.coordinator.address),
            manufacturer="Nearmeter",
            model=data.board_name or "nRFClaw",
            hw_version=data.hw_rev,
            sw_version=sw_version,
        )


class DirectHallStateBinarySensor(DirectSemanticBinarySensor):
    _attr_name = "Hall state"
    def __init__(self, c, e): super().__init__(c, e, "hall_state")
    @property
    def is_on(self): return getattr(self.coordinator.data, "hall_state", None)
    @property
    def available(self): return bool(super().available and self.is_on is not None)


class DirectTrackingBinarySensor(DirectSemanticBinarySensor):
    _attr_name = "Tracking"
    def __init__(self, c, e): super().__init__(c, e, "tracking")
    @property
    def is_on(self): return self.coordinator.data.capability_active(CAP_TRACKING)
    @property
    def available(self): return bool(super().available and self.is_on is not None)


class DirectMotionBinarySensor(DirectSemanticBinarySensor):
    """Five-second HA latch driven by the existing Direct semantic motion event."""

    _attr_name = "Motion"
    _attr_device_class = BinarySensorDeviceClass.MOTION
    _MOTION_HOLD_SECONDS = 5.0

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator, entry, "motion")
        self._motion: bool = False
        self._clear_task: asyncio.Task | None = None

    @property
    def is_on(self): return self._motion

    @property
    def available(self):
        return bool(
            super().available
            and self.coordinator.data.accel_event_mode == ACCEL_EVENT_MOTION
        )

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            self.coordinator.async_add_direct_event_listener(self._handle_direct_event)
        )
        self.async_on_remove(self._cancel_clear_task)

    @callback
    def _handle_direct_event(self, event: DirectAdvEvent) -> None:
        if event.event_type != "motion":
            return
        self._motion = True
        self.async_write_ha_state()
        self._cancel_clear_task()
        self._clear_task = self.hass.async_create_task(
            self._async_clear_motion(), "nRFClaw Direct motion clear"
        )

    async def _async_clear_motion(self) -> None:
        try:
            await asyncio.sleep(self._MOTION_HOLD_SECONDS)
        except asyncio.CancelledError:
            return
        self._motion = False
        self._clear_task = None
        self.async_write_ha_state()

    @callback
    def _cancel_clear_task(self) -> None:
        if self._clear_task is not None:
            self._clear_task.cancel()
            self._clear_task = None
