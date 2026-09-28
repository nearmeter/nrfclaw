"""Direct-NDP semantic sensor entities — B7.6f2m3."""

from __future__ import annotations

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorStateClass
from homeassistant.const import PERCENTAGE, UnitOfElectricPotential, UnitOfTemperature, UnitOfVolume
from homeassistant.helpers.entity import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .battery import battery_percentage_from_voltage
from .client import NrfClawData
from .const import CAP_BATTERY, CAP_DS18B20, CAP_HALL, CAP_TEMPERATURE, DOMAIN, SEMCAP_VOLUME, SEMCAP_VOLUME_US_GALLON
from .direct_coordinator import DirectNdpCoordinator
from .direct_semantic_profile import family_supported
from .device_identity import direct_device_name


def build_direct_sensor_entities(coordinator, entry) -> list[SensorEntity]:
    data: NrfClawData = coordinator.data
    entities: list[SensorEntity] = []

    if family_supported(data, CAP_BATTERY):
        entities.extend((BatteryLevelSensor(coordinator, entry),
                         BatteryVoltageSensor(coordinator, entry)))
    if data.has_capability(CAP_TEMPERATURE) or family_supported(data, CAP_DS18B20):
        entities.append(TemperatureSensor(coordinator, entry))
    # B7.6f2m3: Hall has one user-facing scalar. SINGLE counts upward;
    # QUADRATURE uses the same Counter entity with a signed value so reverse
    # movement decrements it. If a CLI/VM semantic volume is present, prefer
    # the engineering unit and do not create the raw Counter entity.
    has_volume_l = (SEMCAP_VOLUME, 0) in data.semantic_values
    has_volume_gal = (SEMCAP_VOLUME_US_GALLON, 0) in data.semantic_values
    if family_supported(data, CAP_HALL) and not (has_volume_l or has_volume_gal):
        entities.append(CounterSensor(coordinator, entry))
    if has_volume_l:
        entities.append(TotalVolumeSensor(coordinator, entry))
    if has_volume_gal:
        entities.append(TotalVolumeUsGallonSensor(coordinator, entry))
    return entities


class DirectNdpSensor(CoordinatorEntity[DirectNdpCoordinator], SensorEntity):
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

    @property
    def available(self) -> bool:
        return bool(super().available and self.native_value is not None)


class BatteryLevelSensor(DirectNdpSensor):
    _attr_name = "Battery level"
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_native_unit_of_measurement = PERCENTAGE
    _attr_state_class = SensorStateClass.MEASUREMENT
    def __init__(self, c, e): super().__init__(c, e, "battery_level")
    @property
    def native_value(self): return battery_percentage_from_voltage(self.coordinator.data.battery_v)


class BatteryVoltageSensor(DirectNdpSensor):
    _attr_name = "Battery voltage"
    _attr_device_class = SensorDeviceClass.VOLTAGE
    _attr_native_unit_of_measurement = UnitOfElectricPotential.VOLT
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_suggested_display_precision = 2
    def __init__(self, c, e): super().__init__(c, e, "battery_voltage")
    @property
    def native_value(self): return self.coordinator.data.battery_v


class TemperatureSensor(DirectNdpSensor):
    _attr_name = "Temperature"
    _attr_device_class = SensorDeviceClass.TEMPERATURE
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_suggested_display_precision = 3
    def __init__(self, c, e): super().__init__(c, e, "temperature")
    @property
    def native_value(self): return self.coordinator.data.temperature_c


class CounterSensor(DirectNdpSensor):
    _attr_name = "Counter"
    _attr_native_unit_of_measurement = "count"
    def __init__(self, c, e): super().__init__(c, e, "counter")
    @property
    def native_value(self):
        data = self.coordinator.data
        if (SEMCAP_VOLUME, 0) in data.semantic_values or (SEMCAP_VOLUME_US_GALLON, 0) in data.semantic_values:
            return None
        return data.hall_value if data.hall_mode in (1, 2) else None


class TotalVolumeSensor(DirectNdpSensor):
    _attr_name = "Volume"
    _attr_device_class = SensorDeviceClass.VOLUME
    _attr_native_unit_of_measurement = UnitOfVolume.LITERS
    _attr_state_class = SensorStateClass.TOTAL
    _attr_suggested_display_precision = 3
    def __init__(self, c, e): super().__init__(c, e, "semantic_volume_0")
    @property
    def native_value(self):
        state = self.coordinator.data.semantic_values.get((SEMCAP_VOLUME, 0))
        return None if state is None else state.value


class TotalVolumeUsGallonSensor(DirectNdpSensor):
    _attr_name = "Volume"
    _attr_device_class = SensorDeviceClass.VOLUME
    _attr_native_unit_of_measurement = "gal"
    _attr_state_class = SensorStateClass.TOTAL
    _attr_suggested_display_precision = 3
    def __init__(self, c, e): super().__init__(c, e, "semantic_volume_us_gallon_0")
    @property
    def native_value(self):
        state = self.coordinator.data.semantic_values.get((SEMCAP_VOLUME_US_GALLON, 0))
        return None if state is None else state.value
