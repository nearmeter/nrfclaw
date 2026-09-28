"""Home Assistant event entities for Direct and NinaLink bridge modes."""

from __future__ import annotations

from .const import MODE_DIRECT
from .direct_event_entities import build_direct_event_entities
from .ninalink_entities import build_event_entities
from .platform_helpers import setup_dynamic_entities


async def async_setup_entry(hass, entry, async_add_entities) -> None:
    if entry.runtime_data.mode == MODE_DIRECT:
        async_add_entities(
            build_direct_event_entities(
                entry.runtime_data.coordinator,
                entry,
            )
        )
        return

    setup_dynamic_entities(
        entry,
        async_add_entities,
        build_event_entities,
    )
