"""Capability-gated Direct behavior controls.

B7.6f2k3 keeps the Home Assistant UI at the product-behavior level:
- exclusive Motion/Tap/Fall classifier,
- Low/Normal/High sensitivity presets,
- Hall Off/Counter HALL1/Counter HALL2/Quadrature,
- Hall counter reset,
- autonomous vibration Off/Auto + Low/Normal/High sensitivity + baseline relearn,
- reset of a VM-backed retained accumulated total.

Raw thresholds, pull-ups and other engineering knobs stay in CLI/Studio/prompt.
"""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.components.select import SelectEntity
from homeassistant.helpers.entity import DeviceInfo, EntityCategory
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .client import NrfClawData
from .const import (
    ACCEL_EVENT_FALL,
    ACCEL_EVENT_MOTION,
    ACCEL_EVENT_OFF,
    ACCEL_EVENT_TAP,
    CAP_ACCEL,
    CAP_HALL,
    CAP_VIB_AUTO,
    DOMAIN,
    EVENT_SENSITIVITY_HIGH,
    EVENT_SENSITIVITY_LOW,
    EVENT_SENSITIVITY_NORMAL,
    HALL_MODE_OFF,
    HALL_MODE_QUADRATURE,
    HALL_MODE_SINGLE,
    SEMCAP_VOLUME,
    SEMCAP_VOLUME_US_GALLON,
)
from .direct_coordinator import DirectNdpCoordinator
from .direct_semantic_profile import family_supported
from .device_identity import direct_device_name

_EVENT_OPTIONS = ["Off", "Motion", "Tap", "Fall"]
_EVENT_OPTION_TO_MODE = {
    "Off": ACCEL_EVENT_OFF,
    "Motion": ACCEL_EVENT_MOTION,
    "Tap": ACCEL_EVENT_TAP,
    "Fall": ACCEL_EVENT_FALL,
}
_EVENT_MODE_TO_OPTION = {value: key for key, value in _EVENT_OPTION_TO_MODE.items()}

_SENSITIVITY_OPTIONS = ["Low", "Normal", "High"]
_SENSITIVITY_OPTION_TO_VALUE = {
    "Low": EVENT_SENSITIVITY_LOW,
    "Normal": EVENT_SENSITIVITY_NORMAL,
    "High": EVENT_SENSITIVITY_HIGH,
}
_SENSITIVITY_VALUE_TO_OPTION = {
    value: key for key, value in _SENSITIVITY_OPTION_TO_VALUE.items()
}

_HALL_OPTIONS = ["Off", "Counter HALL1", "Counter HALL2", "Quadrature"]
_HALL_OPTION_TO_PROFILE = {
    "Off": (HALL_MODE_OFF, 1),
    "Counter HALL1": (HALL_MODE_SINGLE, 1),
    "Counter HALL2": (HALL_MODE_SINGLE, 2),
    "Quadrature": (HALL_MODE_QUADRATURE, 1),
}

_VIBRATION_OPTIONS = ["Off", "Auto"]

_VIBRATION_SENSITIVITY_OPTIONS = ["Low", "Normal", "High"]
_VIBRATION_SENSITIVITY_OPTION_TO_VALUE = {"Low": 0, "Normal": 1, "High": 2}
_VIBRATION_SENSITIVITY_VALUE_TO_OPTION = {value: key for key, value in _VIBRATION_SENSITIVITY_OPTION_TO_VALUE.items()}


def _has_resettable_total(data: NrfClawData) -> bool:
    return bool(
        data.semantic_total_reset_writable
        and (
            (SEMCAP_VOLUME, 0) in data.semantic_values
            or (SEMCAP_VOLUME_US_GALLON, 0) in data.semantic_values
        )
    )


def _device_info(coordinator: DirectNdpCoordinator, entry) -> DeviceInfo:
    data: NrfClawData = coordinator.data
    sw_version = data.firmware
    if sw_version is not None and data.firmware_build is not None:
        sw_version = f"{sw_version} build {data.firmware_build}"
    return DeviceInfo(
        identifiers={(DOMAIN, entry.unique_id or coordinator.address)},
        name=direct_device_name(data.board_name, coordinator.address),
        manufacturer="Nearmeter",
        model=data.board_name or "nRFClaw",
        hw_version=data.hw_rev,
        sw_version=sw_version,
    )


def build_direct_sensor_control_entities(
    coordinator: DirectNdpCoordinator,
    entry,
) -> list[SelectEntity]:
    data: NrfClawData = coordinator.data
    entities: list[SelectEntity] = []

    if family_supported(data, CAP_ACCEL) and data.accel_event_control_writable:
        entities.append(DirectAccelEventModeSelect(coordinator, entry))
    if family_supported(data, CAP_ACCEL) and data.accel_event_sensitivity_writable:
        entities.append(DirectAccelEventSensitivitySelect(coordinator, entry))
    if family_supported(data, CAP_HALL) and data.hall_control_writable:
        entities.append(DirectHallModeSelect(coordinator, entry))
    if data.has_capability(CAP_VIB_AUTO) and data.vib_auto_control_writable:
        entities.append(DirectVibrationMonitoringSelect(coordinator, entry))
    if data.has_capability(CAP_VIB_AUTO) and data.vib_auto_sensitivity_writable:
        entities.append(DirectVibrationSensitivitySelect(coordinator, entry))

    return entities


def build_direct_sensor_control_buttons(
    coordinator: DirectNdpCoordinator,
    entry,
) -> list[ButtonEntity]:
    data: NrfClawData = coordinator.data
    buttons: list[ButtonEntity] = []
    if family_supported(data, CAP_HALL) and data.hall_control_writable:
        buttons.append(DirectHallCounterResetButton(coordinator, entry))
    if data.has_capability(CAP_VIB_AUTO) and data.vib_auto_control_writable:
        buttons.append(DirectVibrationBaselineRelearnButton(coordinator, entry))
    if _has_resettable_total(data):
        buttons.append(DirectAccumulatedTotalResetButton(coordinator, entry))
    return buttons


class DirectAccelEventModeSelect(
    CoordinatorEntity[DirectNdpCoordinator], SelectEntity
):
    _attr_has_entity_name = True
    _attr_name = "Event detection"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _EVENT_OPTIONS

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_accel_event_mode"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def current_option(self) -> str | None:
        return _EVENT_MODE_TO_OPTION.get(self.coordinator.data.accel_event_mode)

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.accel_event_control_writable
            and self.coordinator.data.vib_auto_enabled is not True
            and self.coordinator.data.accel_event_mode is not None
        )

    async def async_select_option(self, option: str) -> None:
        mode = _EVENT_OPTION_TO_MODE.get(option)
        if mode is None:
            raise ValueError(f"unsupported accelerometer event option: {option}")
        await self.coordinator.async_write_accel_event_mode(mode)


class DirectAccelEventSensitivitySelect(
    CoordinatorEntity[DirectNdpCoordinator], SelectEntity
):
    _attr_has_entity_name = True
    _attr_name = "Event sensitivity"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _SENSITIVITY_OPTIONS

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_accel_event_sensitivity"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def current_option(self) -> str | None:
        return _SENSITIVITY_VALUE_TO_OPTION.get(
            self.coordinator.data.accel_event_sensitivity
        )

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.accel_event_sensitivity_writable
            and self.coordinator.data.vib_auto_enabled is not True
            and self.coordinator.data.accel_event_mode
            in (ACCEL_EVENT_MOTION, ACCEL_EVENT_TAP, ACCEL_EVENT_FALL)
            and self.coordinator.data.accel_event_sensitivity is not None
        )

    async def async_select_option(self, option: str) -> None:
        sensitivity = _SENSITIVITY_OPTION_TO_VALUE.get(option)
        if sensitivity is None:
            raise ValueError(f"unsupported event sensitivity option: {option}")
        await self.coordinator.async_write_accel_event_sensitivity(sensitivity)


class DirectVibrationMonitoringSelect(
    CoordinatorEntity[DirectNdpCoordinator], SelectEntity
):
    _attr_has_entity_name = True
    _attr_name = "Vibration monitoring"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _VIBRATION_OPTIONS

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_vibration_monitoring"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def current_option(self) -> str | None:
        enabled = self.coordinator.data.vib_auto_enabled
        if enabled is None:
            return None
        return "Auto" if enabled else "Off"

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.vib_auto_control_writable
            and self.coordinator.data.vib_auto_enabled is not None
        )

    async def async_select_option(self, option: str) -> None:
        if option not in _VIBRATION_OPTIONS:
            raise ValueError(f"unsupported vibration-monitoring option: {option}")
        await self.coordinator.async_write_vibration_monitoring(option == "Auto")


class DirectVibrationSensitivitySelect(
    CoordinatorEntity[DirectNdpCoordinator], SelectEntity
):
    _attr_has_entity_name = True
    _attr_name = "Vibration sensitivity"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _VIBRATION_SENSITIVITY_OPTIONS

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_vibration_sensitivity"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def current_option(self) -> str | None:
        return _VIBRATION_SENSITIVITY_VALUE_TO_OPTION.get(
            self.coordinator.data.vib_auto_sensitivity
        )

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.vib_auto_sensitivity_writable
            and self.coordinator.data.vib_auto_sensitivity is not None
        )

    async def async_select_option(self, option: str) -> None:
        sensitivity = _VIBRATION_SENSITIVITY_OPTION_TO_VALUE.get(option)
        if sensitivity is None:
            raise ValueError(f"unsupported vibration sensitivity option: {option}")
        await self.coordinator.async_write_vibration_sensitivity(sensitivity)


class DirectHallModeSelect(CoordinatorEntity[DirectNdpCoordinator], SelectEntity):
    _attr_has_entity_name = True
    _attr_name = "Hall mode"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = _HALL_OPTIONS

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_hall_mode"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def current_option(self) -> str | None:
        mode = self.coordinator.data.hall_mode
        channel = self.coordinator.data.hall_channel
        if mode == HALL_MODE_OFF:
            return "Off"
        if mode == HALL_MODE_SINGLE and channel == 1:
            return "Counter HALL1"
        if mode == HALL_MODE_SINGLE and channel == 2:
            return "Counter HALL2"
        if mode == HALL_MODE_QUADRATURE:
            return "Quadrature"
        return None

    @property
    def available(self) -> bool:
        return bool(super().available and self.coordinator.data.hall_control_writable)

    async def async_select_option(self, option: str) -> None:
        profile = _HALL_OPTION_TO_PROFILE.get(option)
        if profile is None:
            raise ValueError(f"unsupported Hall option: {option}")
        await self.coordinator.async_write_hall_mode(*profile)


class DirectHallCounterResetButton(
    CoordinatorEntity[DirectNdpCoordinator], ButtonEntity
):
    _attr_has_entity_name = True
    _attr_name = "Reset Hall counter"
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_hall_counter_reset"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.hall_control_writable
            and self.coordinator.data.hall_mode in (HALL_MODE_SINGLE, HALL_MODE_QUADRATURE)
        )

    async def async_press(self) -> None:
        await self.coordinator.async_reset_hall_counter()


class DirectVibrationBaselineRelearnButton(
    CoordinatorEntity[DirectNdpCoordinator], ButtonEntity
):
    _attr_has_entity_name = True
    _attr_name = "Relearn vibration baseline"
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_vibration_relearn"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def available(self) -> bool:
        return bool(
            super().available
            and self.coordinator.data.vib_auto_control_writable
            and self.coordinator.data.vib_auto_enabled is True
        )

    async def async_press(self) -> None:
        await self.coordinator.async_relearn_vibration_baseline()


class DirectAccumulatedTotalResetButton(
    CoordinatorEntity[DirectNdpCoordinator], ButtonEntity
):
    _attr_has_entity_name = True
    _attr_name = "Reset accumulated total"
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator, entry) -> None:
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_config_accumulated_total_reset"

    @property
    def device_info(self) -> DeviceInfo:
        return _device_info(self.coordinator, self._entry)

    @property
    def available(self) -> bool:
        return bool(super().available and _has_resettable_total(self.coordinator.data))

    async def async_press(self) -> None:
        await self.coordinator.async_reset_accumulated_total()
