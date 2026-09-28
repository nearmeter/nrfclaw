"""Direct EventEntity projection aligned with NinaLink Node semantics."""

from __future__ import annotations

from homeassistant.components.event import EventEntity
from homeassistant.core import callback
from homeassistant.helpers.entity import DeviceInfo

from .client import NrfClawData
from .const import (
    ACCEL_EVENT_FALL,
    ACCEL_EVENT_MOTION,
    ACCEL_EVENT_TAP,
    CAP_ACCEL,
    DOMAIN,
)
from .direct_adv import DirectAdvEvent
from .direct_semantic_profile import family_supported
from .device_identity import direct_device_name


def build_direct_event_entities(coordinator, entry) -> list[EventEntity]:
    data: NrfClawData = coordinator.data
    if not family_supported(data, CAP_ACCEL):
        return []
    return [
        DirectSemanticEvent(
            coordinator, entry, "Motion", "motion", ACCEL_EVENT_MOTION
        ),
        DirectSemanticEvent(
            coordinator, entry, "Tap", "tap", ACCEL_EVENT_TAP
        ),
        DirectSemanticEvent(
            coordinator, entry, "Fall", "fall", ACCEL_EVENT_FALL
        ),
    ]


class DirectSemanticEvent(EventEntity):
    _attr_has_entity_name = True
    _attr_event_types = ["detected"]

    def __init__(
        self,
        coordinator,
        entry,
        name: str,
        direct_event_type: str,
        required_accel_mode: int,
    ) -> None:
        self.coordinator = coordinator
        self._entry = entry
        self._direct_event_type = direct_event_type
        self._required_accel_mode = required_accel_mode
        self._attr_name = name
        self._attr_unique_id = f"{entry.unique_id}_event_{direct_event_type}"

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

    @property
    def available(self) -> bool:
        return bool(
            self.coordinator.last_update_success
            and self.coordinator.data.accel_event_mode == self._required_accel_mode
        )

    async def async_get_last_event_data(self):
        return None

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            self.coordinator.async_add_direct_event_listener(self._handle_direct_event)
        )

    @callback
    def _handle_direct_event(self, event: DirectAdvEvent) -> None:
        if event.event_type != self._direct_event_type:
            return
        self._trigger_event(
            "detected",
            {
                "event_id": event.event_id,
                "sequence": event.sequence,
                "capability_id": event.capability_id,
                "channel": event.channel,
                "arg0": event.arg0,
                "arg1": event.arg1,
            },
        )
        self.async_write_ha_state()
