from __future__ import annotations

from datetime import timedelta

from homeassistant.components import bluetooth
from homeassistant.components.sensor import SensorEntity, SensorDeviceClass
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import UnitOfElectricPotential, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import (
    DataUpdateCoordinator, CoordinatorEntity, UpdateFailed,
)

from .client import NrfClawClient
from .const import DOMAIN, CAP_BATTERY, CAP_DS18B20, CAP_ACCEL, CAP_HALL


class NrfClawCoordinator(DataUpdateCoordinator):
    def __init__(self, hass: HomeAssistant, address: str, access_key: str = ""):
        super().__init__(
            hass,
            logger=__import__("logging").getLogger(__name__),
            name=f"nRFClaw {address}",
            update_interval=timedelta(seconds=30),
        )
        self.address = address
        self.access_key = access_key

    async def _async_update_data(self):
        device = bluetooth.async_ble_device_from_address(
            self.hass, self.address, connectable=True
        )
        if device is None:
            raise UpdateFailed("nRFClaw is not reachable by a connectable scanner")
        try:
            return await NrfClawClient(device, self.access_key).read()
        except Exception as err:
            raise UpdateFailed(str(err)) from err


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry, async_add_entities):
    coordinator = NrfClawCoordinator(
        hass, entry.data["address"], entry.data.get("access_key", "")
    )
    await coordinator.async_config_entry_first_refresh()

    entities = []
    d = coordinator.data
    if d.has_capability(CAP_BATTERY):
        entities.append(BatterySensor(coordinator, entry))
    if d.has_capability(CAP_HALL):
        entities.append(HallSensor(coordinator, entry))
    if d.has_capability(CAP_DS18B20):
        entities.append(TemperatureSensor(coordinator, entry))
    if d.has_capability(CAP_ACCEL):
        entities.extend([
            VibrationRmsSensor(coordinator, entry),
            VibrationPeakSensor(coordinator, entry),
        ])

    async_add_entities(entities)


class BaseSensor(CoordinatorEntity, SensorEntity):
    _attr_has_entity_name = True

    def __init__(self, coordinator, entry, key):
        super().__init__(coordinator)
        self._entry = entry
        self._attr_unique_id = f"{entry.unique_id}_{key}"

    @property
    def device_info(self):
        d = self.coordinator.data
        sw = d.firmware
        if sw is not None and d.firmware_build is not None:
            sw = f"{sw} build {d.firmware_build}"
        return {
            "identifiers": {(DOMAIN, self._entry.unique_id)},
            "name": self._entry.title,
            "manufacturer": "nRFClaw",
            "model": d.board_name or "nRFClaw",
            "hw_version": d.hw_rev,
            "sw_version": sw,
        }


class BatterySensor(BaseSensor):
    _attr_name = "Battery voltage"
    _attr_device_class = SensorDeviceClass.VOLTAGE
    _attr_native_unit_of_measurement = UnitOfElectricPotential.VOLT

    def __init__(self, coordinator, entry):
        super().__init__(coordinator, entry, "battery_voltage")

    @property
    def native_value(self):
        return self.coordinator.data.battery_v


class HallSensor(BaseSensor):
    _attr_name = "Hall counter"

    def __init__(self, coordinator, entry):
        super().__init__(coordinator, entry, "hall")

    @property
    def available(self):
        return super().available and bool(self.coordinator.data.hall_mode)

    @property
    def native_value(self):
        return self.coordinator.data.hall_value


class TemperatureSensor(BaseSensor):
    _attr_name = "DS18B20 temperature"
    _attr_device_class = SensorDeviceClass.TEMPERATURE
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS

    def __init__(self, coordinator, entry):
        super().__init__(coordinator, entry, "ds18b20_temperature")

    @property
    def native_value(self):
        return self.coordinator.data.temperature_c


class VibrationRmsSensor(BaseSensor):
    _attr_name = "Vibration RMS"
    _attr_native_unit_of_measurement = "mg"

    def __init__(self, coordinator, entry):
        super().__init__(coordinator, entry, "vibration_rms")

    @property
    def native_value(self):
        return self.coordinator.data.vibration_rms_mg


class VibrationPeakSensor(BaseSensor):
    _attr_name = "Vibration peak"
    _attr_native_unit_of_measurement = "mg"

    def __init__(self, coordinator, entry):
        super().__init__(coordinator, entry, "vibration_peak")

    @property
    def native_value(self):
        return self.coordinator.data.vibration_peak_mg
