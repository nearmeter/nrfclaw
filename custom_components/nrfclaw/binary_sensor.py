"""nRFClaw binary sensors for Direct native capabilities and NinaLink bridge nodes."""

from __future__ import annotations

from .const import MODE_DIRECT
from .direct_binary_entities import build_direct_binary_sensor_entities
from .ninalink_entities import build_binary_sensor_entities
from .platform_helpers import setup_dynamic_entities


async def async_setup_entry(hass, entry, async_add_entities) -> None:
    if entry.runtime_data.mode == MODE_DIRECT:
        async_add_entities(
            build_direct_binary_sensor_entities(entry.runtime_data.coordinator, entry)
        )
        return

    setup_dynamic_entities(entry, async_add_entities, build_binary_sensor_entities)
