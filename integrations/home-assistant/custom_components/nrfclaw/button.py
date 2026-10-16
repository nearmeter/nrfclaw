"""Home Assistant button platform for Direct and NinaLink behavior actions."""

from __future__ import annotations

from homeassistant.helpers import entity_registry as er

from .const import DOMAIN, MODE_BRIDGE, MODE_DIRECT
from .direct_sensor_controls import build_direct_sensor_control_buttons
from .ninalink_buttons import build_ninalink_control_buttons



def _cleanup_rf_virtual_command_buttons(hass, entry) -> None:
    # Remove stale command buttons belonging to RF virtual nodes.
    registry = er.async_get(hass)

    for reg_entry in er.async_entries_for_config_entry(
        registry,
        entry.entry_id,
    ):
        if reg_entry.platform != DOMAIN:
            continue
        if not reg_entry.entity_id.startswith("button."):
            continue

        unique_id = str(reg_entry.unique_id or "")
        parts = unique_id.split(":")
        if (
            len(parts) != 4
            or parts[0] != "ninalink"
            or parts[2] != "command"
        ):
            continue

        try:
            node_id = int(parts[1], 16)
        except ValueError:
            continue

        if (node_id & 0xFF000000) == 0xF8000000:
            registry.async_remove(reg_entry.entity_id)


async def async_setup_entry(hass, entry, async_add_entities) -> None:
    runtime = entry.runtime_data
    _cleanup_rf_virtual_command_buttons(hass, entry)
    coordinator = runtime.coordinator
    known: set[str] = set()

    if runtime.mode == MODE_DIRECT:
        def _sync_direct_buttons() -> None:
            fresh = []
            for entity in build_direct_sensor_control_buttons(coordinator, entry):
                if entity.unique_id in known:
                    continue
                known.add(entity.unique_id)
                fresh.append(entity)
            if fresh:
                async_add_entities(fresh)

        entry.async_on_unload(coordinator.async_add_listener(_sync_direct_buttons))
        _sync_direct_buttons()
        return

    if runtime.mode == MODE_BRIDGE:
        bridge_device_id = runtime.bridge_device_id
        if bridge_device_id is None:
            return

        def _sync_ninalink_buttons() -> None:
            fresh = []
            for entity in build_ninalink_control_buttons(
                coordinator,
                bridge_device_id,
            ):
                if entity.unique_id in known:
                    continue
                known.add(entity.unique_id)
                fresh.append(entity)
            if fresh:
                async_add_entities(fresh)

        entry.async_on_unload(
            coordinator.async_add_listener(_sync_ninalink_buttons)
        )
        _sync_ninalink_buttons()
