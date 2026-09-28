"""Shared Direct semantic inventory for Home Assistant parity with NinaLink Node."""

from __future__ import annotations

NINASENSE_SENSOR_ENTITIES = (
    "Battery level", "Battery voltage", "Temperature",
    "Acceleration X", "Acceleration Y", "Acceleration Z",
    "Counter",
)
NINASENSE_BINARY_ENTITIES = ("Hall state", "Motion", "Tracking")
NINASENSE_EVENT_ENTITIES = ("Motion", "Tap", "Fall")
NINASENSE_SEMANTIC_ENTITY_COUNT = (
    len(NINASENSE_SENSOR_ENTITIES)
    + len(NINASENSE_BINARY_ENTITIES)
    + len(NINASENSE_EVENT_ENTITIES)
)


def is_ninasense(data) -> bool:
    return str(getattr(data, "board_name", "") or "").upper() == "NINASENSE"


def family_supported(data, capability: int) -> bool:
    """Full vocabulary for NINASENSE; capability-family driven for generic boards."""
    return is_ninasense(data) or bool(data.has_capability(capability))
