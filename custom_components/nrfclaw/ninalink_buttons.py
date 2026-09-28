"""NinaLink remote behavioral action buttons matching the Direct UI."""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.components.button import ButtonEntity
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .ninalink_controls import (
    ENUM8,
    HALL_MODE_CAPABILITY,
    VIBRATION_MONITORING_CAPABILITY,
    _current_typed_value,
    _device_info,
    _inventory_entry,
    _node_online,
    _remote_control_supported,
)

_LOGGER = logging.getLogger(__name__)

RESET_HALL_COUNTER_COMMAND = 0x0005
VIB_AUTO_RELEARN_COMMAND = 0x0006
RESET_SEMANTIC_TOTAL_COMMAND = 0x0007

COUNTER_CAPABILITY = 0x0302
QUADRATURE_POSITION_CAPABILITY = 0x0303
VOLUME_CAPABILITY = 0x0601
VOLUME_US_GALLON_CAPABILITY = 0x0606


def _inventory_supported(
    data: dict[str, Any] | None,
    node_id: int,
    capability_id: int,
    channel: int = 0,
) -> bool:
    desc = _inventory_entry(data, node_id, capability_id, channel)
    return bool(
        desc is not None
        and (int(desc.get("state_flags", 0)) & 0x01)
    )


def _has_remote_volume(data: dict[str, Any] | None, node_id: int) -> bool:
    return bool(
        _inventory_supported(data, node_id, VOLUME_CAPABILITY)
        or _inventory_supported(data, node_id, VOLUME_US_GALLON_CAPABILITY)
    )


class _NinaLinkRemoteCommandButton(CoordinatorEntity, ButtonEntity):
    """Base class for one zero-argument NinaLink COMMAND button."""

    _command_id: int

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
            f"ninalink:{self._node_id:08X}:command:"
            f"0x{self._command_id:04X}"
        )
        self._attr_device_info = _device_info(device, bridge_device_id)

    def _semantic_available(self) -> bool:
        return True

    def _readback_endpoints(self) -> list[tuple[int, int]]:
        return []

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and _node_online(self.coordinator.data, self._node_id)
            and self._semantic_available()
        )

    async def async_press(self) -> None:
        await self.coordinator.async_execute_remote_command(
            self._node_id,
            self._command_id,
        )

        endpoints = self._readback_endpoints()
        if not endpoints:
            return

        # The COMMAND result is authoritative for the operation itself.
        # The follow-up QUERY_CAPABILITIES refreshes the affected state on the
        # node's next normal contact; no optimistic cache mutation is made.
        try:
            await self.coordinator.runtime.builder.remote_query_capabilities(
                self._node_id,
                endpoints,
            )
        except Exception as exc:
            _LOGGER.warning(
                "NinaLink command 0x%04X succeeded for node %08X, "
                "but follow-up readback could not be queued: %s",
                self._command_id,
                self._node_id,
                exc,
            )


class NinaLinkHallCounterResetButton(_NinaLinkRemoteCommandButton):
    _attr_name = "Reset Hall counter"
    _command_id = RESET_HALL_COUNTER_COMMAND

    def _semantic_available(self) -> bool:
        if not _remote_control_supported(
            self.coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            ENUM8,
        ):
            return False
        mode = _current_typed_value(
            self.coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
        )
        return mode in (1, 2, 3)

    def _readback_endpoints(self) -> list[tuple[int, int]]:
        mode = _current_typed_value(
            self.coordinator.data,
            self._node_id,
            HALL_MODE_CAPABILITY,
            0,
        )
        if mode in (1, 2):
            return [
                (HALL_MODE_CAPABILITY, 0),
                (COUNTER_CAPABILITY, 0),
            ]
        if mode == 3:
            return [
                (HALL_MODE_CAPABILITY, 0),
                (QUADRATURE_POSITION_CAPABILITY, 0),
            ]
        return [(HALL_MODE_CAPABILITY, 0)]


class NinaLinkVibrationBaselineRelearnButton(_NinaLinkRemoteCommandButton):
    _attr_name = "Relearn vibration baseline"
    _command_id = VIB_AUTO_RELEARN_COMMAND

    def _semantic_available(self) -> bool:
        return bool(
            _inventory_supported(
                self.coordinator.data,
                self._node_id,
                VIBRATION_MONITORING_CAPABILITY,
            )
            and _current_typed_value(
                self.coordinator.data,
                self._node_id,
                VIBRATION_MONITORING_CAPABILITY,
                0,
            )
            == 1
        )


class NinaLinkAccumulatedTotalResetButton(_NinaLinkRemoteCommandButton):
    _attr_name = "Reset accumulated total"
    _command_id = RESET_SEMANTIC_TOTAL_COMMAND

    def _semantic_available(self) -> bool:
        if not _has_remote_volume(self.coordinator.data, self._node_id):
            return False
        return bool(
            _current_typed_value(
                self.coordinator.data,
                self._node_id,
                VOLUME_CAPABILITY,
                0,
            )
            is not None
            or _current_typed_value(
                self.coordinator.data,
                self._node_id,
                VOLUME_US_GALLON_CAPABILITY,
                0,
            )
            is not None
        )

    def _readback_endpoints(self) -> list[tuple[int, int]]:
        endpoints: list[tuple[int, int]] = []
        for capability_id in (
            VOLUME_CAPABILITY,
            VOLUME_US_GALLON_CAPABILITY,
        ):
            if _inventory_supported(
                self.coordinator.data,
                self._node_id,
                capability_id,
            ):
                endpoints.append((capability_id, 0))
        return endpoints[:2]


def build_ninalink_control_buttons(
    coordinator: Any,
    bridge_device_id: str,
) -> list[ButtonEntity]:
    """Build remote behavior buttons only for relevant discovered semantics."""
    data = coordinator.data or {}
    buttons: list[ButtonEntity] = []

    for device in data.get("ha", {}).get("devices", []):
        node_id = int(device.get("node_id_raw", -1))

        # Hall button mirrors Direct: entity exists when Hall control exists,
        # but becomes unavailable while Hall mode is Off.
        if _remote_control_supported(
            data,
            node_id,
            HALL_MODE_CAPABILITY,
            ENUM8,
        ):
            buttons.append(
                NinaLinkHallCounterResetButton(
                    coordinator,
                    device,
                    bridge_device_id,
                )
            )

        # Relearn mirrors Direct: entity exists with VIB_AUTO support and is
        # available only while monitoring is Auto.
        if _inventory_supported(
            data,
            node_id,
            VIBRATION_MONITORING_CAPABILITY,
        ):
            buttons.append(
                NinaLinkVibrationBaselineRelearnButton(
                    coordinator,
                    device,
                    bridge_device_id,
                )
            )

        # Retained semantic total is optional, exactly as in Direct mode.
        if _has_remote_volume(data, node_id):
            buttons.append(
                NinaLinkAccumulatedTotalResetButton(
                    coordinator,
                    device,
                    bridge_device_id,
                )
            )

    return buttons
