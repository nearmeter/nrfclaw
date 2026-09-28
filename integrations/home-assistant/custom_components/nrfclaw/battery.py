"""Battery presentation helpers.

Voltage is the authoritative physical measurement.  The percentage is only a
linear display estimate used by Home Assistant's battery device class.
"""

from __future__ import annotations

import math

BATTERY_EMPTY_V = 2.0
BATTERY_FULL_V = 3.6


def battery_percentage_from_voltage(voltage: float | int | None) -> int | None:
    if voltage is None:
        return None
    try:
        value = float(voltage)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(value):
        return None
    if value <= BATTERY_EMPTY_V:
        return 0
    if value >= BATTERY_FULL_V:
        return 100
    percent = (value - BATTERY_EMPTY_V) * 100.0 / (BATTERY_FULL_V - BATTERY_EMPTY_V)
    return int(percent + 0.5)
