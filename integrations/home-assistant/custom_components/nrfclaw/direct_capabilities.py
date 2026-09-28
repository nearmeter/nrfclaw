"""Native Direct-NDP capability presentation metadata."""

from .const import (
    CAP_ACCEL, CAP_BATTERY, CAP_BLE_APP, CAP_DS18B20, CAP_GPIO, CAP_HALL,
    CAP_LORA, CAP_RTC, CAP_SERIAL, CAP_STATE, CAP_TRACKING, CAP_VIB_AUTO,
    CAP_VIB_HEALTH, CAP_TEMPERATURE,
)

DIRECT_CAPABILITY_NAMES = {
    CAP_GPIO: "GPIO available",
    CAP_BATTERY: "Battery measurement available",
    CAP_DS18B20: "DS18B20 present",
    CAP_ACCEL: "Accelerometer present",
    CAP_LORA: "LoRa available",
    CAP_BLE_APP: "BLE Application available",
    CAP_RTC: "RTC available",
    CAP_HALL: "Hall active",
    CAP_STATE: "Persistent state available",
    CAP_SERIAL: "Serial active",
    CAP_TRACKING: "Tracking active",
    CAP_VIB_HEALTH: "Vibration health available",
    CAP_VIB_AUTO: "Vibration auto active",
    CAP_TEMPERATURE: "Temperature available",
}
