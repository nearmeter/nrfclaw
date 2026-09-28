"""nRFClaw select platform for Direct and NinaLink Node controls."""
from __future__ import annotations

from .const import MODE_BRIDGE, MODE_DIRECT
from .direct_sensor_controls import build_direct_sensor_control_entities
from .ninalink_controls import build_ninalink_control_entities


async def async_setup_entry(hass, entry, async_add_entities) -> None:
    runtime = entry.runtime_data
    coordinator = runtime.coordinator
    known: set[str] = set()

    if runtime.mode == MODE_DIRECT:
        def _sync_direct_selects() -> None:
            fresh = []
            for entity in build_direct_sensor_control_entities(coordinator, entry):
                if entity.unique_id in known:
                    continue
                known.add(entity.unique_id)
                fresh.append(entity)
            if fresh:
                async_add_entities(fresh)

        entry.async_on_unload(coordinator.async_add_listener(_sync_direct_selects))
        _sync_direct_selects()
        return

    if runtime.mode == MODE_BRIDGE:
        bridge_device_id = runtime.bridge_device_id
        if bridge_device_id is None:
            return

        def _sync_ninalink_selects() -> None:
            fresh = []
            for entity in build_ninalink_control_entities(coordinator, bridge_device_id):
                if entity.unique_id in known:
                    continue
                known.add(entity.unique_id)
                fresh.append(entity)
            if fresh:
                async_add_entities(fresh)

        entry.async_on_unload(coordinator.async_add_listener(_sync_ninalink_selects))
        _sync_ninalink_selects()
