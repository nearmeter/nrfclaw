"""nRFClaw sensor platform for Direct NDP and NinaLink nodes."""

from __future__ import annotations

from homeassistant.helpers import entity_registry as er

from .const import DOMAIN, MODE_DIRECT, SEMCAP_VOLUME, SEMCAP_VOLUME_US_GALLON
from .direct_entities import build_direct_sensor_entities
from .ninalink_entities import build_sensor_entities
from .platform_helpers import setup_dynamic_entities


_DIRECT_DEPRECATED_SENSOR_SUFFIXES = (
    "acceleration_x",
    "acceleration_y",
    "acceleration_z",
    "position",
    "vibration_frequency",
    "vibration_peak_to_peak",
    "vibration_peak",
    "vibration_rms",
)
_NINALINK_HIDDEN_SENSOR_CAPS = {0x0203, 0x0204, 0x0205, 0x0206, 0x0207, 0x0208, 0x0209, 0x0800, 0x0801, 0x0802, 0x0803, 0x0804}


def _cleanup_deprecated_sensor_registry(hass, entry, runtime) -> None:
    """Remove sensor entities retired by B7.6f2m3.

    Direct unique IDs are deterministic suffixes. NinaLink vibration IDs are
    capability IDs 0x0206..0x0209. NinaLink 0x0303 remains supported, but an
    old registry entry named Position is removed once so it can be recreated
    as Counter with the same wire semantic.
    """
    registry = er.async_get(hass)
    mode = runtime.mode

    if mode == MODE_DIRECT:
        if not entry.unique_id:
            return
        suffixes = list(_DIRECT_DEPRECATED_SENSOR_SUFFIXES)
        data = getattr(runtime.coordinator, "data", None)
        semantic_values = getattr(data, "semantic_values", {}) if data is not None else {}
        if (SEMCAP_VOLUME, 0) in semantic_values or (SEMCAP_VOLUME_US_GALLON, 0) in semantic_values:
            suffixes.append("counter")
        for suffix in suffixes:
            unique_id = f"{entry.unique_id}_{suffix}"
            entity_id = registry.async_get_entity_id("sensor", DOMAIN, unique_id)
            if entity_id is not None:
                registry.async_remove(entity_id)
        return

    volume_nodes: set[int] = set()
    data = getattr(runtime.coordinator, "data", None) or {}
    for device in data.get("ha", {}).get("devices", []):
        if any(int(entity.get("capability_id_raw", -1)) in (0x0601, 0x0606) for entity in device.get("entities", [])):
            volume_nodes.add(int(device.get("node_id_raw", -1)))

    for reg_entry in list(registry.entities.values()):
        if reg_entry.platform != DOMAIN or not reg_entry.entity_id.startswith("sensor."):
            continue
        unique_id = str(reg_entry.unique_id or "")
        if not unique_id.startswith("ninalink:"):
            continue
        parts = unique_id.split(":")
        if len(parts) != 4:
            continue
        try:
            cap_id = int(parts[2], 16)
        except ValueError:
            continue
        if cap_id in _NINALINK_HIDDEN_SENSOR_CAPS:
            registry.async_remove(reg_entry.entity_id)
            continue
        # B7.6f2m6c1b: raw Hall rows are replaced by one stable synthetic Counter.
        if cap_id in (0x0302, 0x0303):
            registry.async_remove(reg_entry.entity_id)
            continue
        try:
            node_id = int(parts[1], 16)
        except ValueError:
            node_id = -1
        if node_id in volume_nodes and cap_id in (0x0302, 0x0303):
            registry.async_remove(reg_entry.entity_id)
            continue
        if cap_id == 0x0303 and getattr(reg_entry, "original_name", None) == "Position":
            registry.async_remove(reg_entry.entity_id)


async def async_setup_entry(hass, entry, async_add_entities) -> None:
    runtime = entry.runtime_data
    _cleanup_deprecated_sensor_registry(hass, entry, runtime)
    if runtime.mode == MODE_DIRECT:
        coordinator = runtime.coordinator
        known: set[str] = set()

        def _sync_direct() -> None:
            fresh = []
            for entity in build_direct_sensor_entities(coordinator, entry):
                if entity.unique_id in known:
                    continue
                known.add(entity.unique_id)
                fresh.append(entity)
            if fresh:
                async_add_entities(fresh)

        entry.async_on_unload(coordinator.async_add_listener(_sync_direct))
        _sync_direct()
        return

    setup_dynamic_entities(
        entry,
        async_add_entities,
        build_sensor_entities,
    )
