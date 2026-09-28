"""nRFClaw Home Assistant integration.

Two intentionally different BLE lifecycles share the same Application/NDP
service:
- Direct NDP sensor: connect -> snapshot -> disconnect for low power.
- NinaLink bridge: persistent authenticated connection for B5.5 push/events.
"""

from __future__ import annotations

import asyncio

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er

from .const import (
    CONF_MODE,
    DOMAIN,
    LEGACY_DEFAULT_MODE,
    MODE_BRIDGE,
    MODE_DIRECT,
    NDP_NINALINK_BRIDGE,
)
from .coordinator import NinaLinkCoordinator
from .direct_coordinator import DirectNdpCoordinator
from .device_identity import bridge_device_name
from .ninalink_runtime import NinaLinkCoordinatorRuntime
from .ninalink_session import NinaLinkSessionManager
from .runtime_data import NrfClawRuntimeData
from .transport import NinaLinkApplicationClient

PLATFORMS = [
    Platform.SENSOR,
    Platform.BINARY_SENSOR,
    Platform.EVENT,
    Platform.SELECT,
    Platform.BUTTON,
]

type NrfClawConfigEntry = ConfigEntry[NrfClawRuntimeData]


def _remove_legacy_lora_config_entities(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> None:
    """Remove B7.6f2j1/j2 LoRa CONFIG entities from the entity registry.

    Radio tuning remains available through firmware/NDP, CLI, Studio and prompt
    workflows.  It is intentionally no longer exposed as ordinary HA device
    configuration.
    """
    registry = er.async_get(hass)
    for entity in er.async_entries_for_config_entry(registry, entry.entry_id):
        if "_config_lora_" in entity.unique_id:
            registry.async_remove(entity.entity_id)


def _remove_legacy_direct_walk_entity(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> None:
    """Remove the obsolete Direct Walk EventEntity from the registry.

    WALK remains a reserved/experimental firmware event for protocol
    compatibility, but it is intentionally not presented as a supported
    Direct Home Assistant control or event because the current classifier can
    produce motion-like false positives.
    """
    registry = er.async_get(hass)
    for entity in er.async_entries_for_config_entry(registry, entry.entry_id):
        if entity.unique_id and entity.unique_id.endswith("_event_walk"):
            registry.async_remove(entity.entity_id)


def _bridge_key(address: str) -> str:
    compact = address.replace(":", "").replace("-", "").upper()
    return f"bridge:{compact[-6:]}"


def _direct_ble_lock(hass: HomeAssistant) -> asyncio.Lock:
    """Serialize short Direct-NDP sessions across local battery devices."""
    domain_data = hass.data.setdefault(DOMAIN, {})
    lock = domain_data.get("direct_ble_lock")
    if lock is None:
        lock = asyncio.Lock()
        domain_data["direct_ble_lock"] = lock
    return lock


async def _async_setup_direct(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> bool:
    coordinator = DirectNdpCoordinator(
        hass,
        entry,
        _direct_ble_lock(hass),
    )
    await coordinator.async_config_entry_first_refresh()

    entry.runtime_data = NrfClawRuntimeData(
        mode=MODE_DIRECT,
        coordinator=coordinator,
    )

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    entry.async_on_unload(coordinator.async_start_advertisements())
    return True


async def _async_setup_bridge(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> bool:
    address = entry.data.get("address") or entry.unique_id
    if not address:
        raise ConfigEntryNotReady("nRFClaw Bluetooth address is missing")

    access_key = entry.data.get("access_key", "")
    bridge_key = _bridge_key(address)

    async def _new_client() -> NinaLinkApplicationClient:
        return NinaLinkApplicationClient(
            hass,
            address,
            access_key,
        )

    client = await _new_client()
    try:
        await client.connect()

        runtime = NinaLinkCoordinatorRuntime(
            client,
            NDP_NINALINK_BRIDGE,
            bridge_key,
        )
        coordinator = NinaLinkCoordinator(hass, entry, runtime)
        await coordinator.async_config_entry_first_refresh()
    except Exception as exc:
        await client.close()
        raise ConfigEntryNotReady(
            f"Unable to establish nRFClaw Application/NDP bridge session: {exc}"
        ) from exc

    registry = dr.async_get(hass)
    bridge = registry.async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, bridge_key)},
        name=bridge_device_name(address),
        manufacturer="Nearmeter",
        model="NINASENSE",
    )

    session = NinaLinkSessionManager(
        client=client,
        coordinator=coordinator,
        client_factory=_new_client,
        bridge_opcode=NDP_NINALINK_BRIDGE,
        bridge_key=bridge_key,
        task_factory=hass.async_create_task,
    )

    entry.runtime_data = NrfClawRuntimeData(
        mode=MODE_BRIDGE,
        coordinator=coordinator,
        bridge_device_id=bridge.id,
        session=session,
    )

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    session.start()
    return True


async def async_setup_entry(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> bool:
    """Set up Direct NDP or persistent NinaLink bridge by explicit entry mode."""
    _remove_legacy_lora_config_entities(hass, entry)
    mode = entry.data.get(CONF_MODE, LEGACY_DEFAULT_MODE)
    if mode == MODE_DIRECT:
        _remove_legacy_direct_walk_entity(hass, entry)
        return await _async_setup_direct(hass, entry)
    if mode == MODE_BRIDGE:
        return await _async_setup_bridge(hass, entry)
    raise ConfigEntryNotReady(f"Unsupported nRFClaw mode: {mode}")


async def async_unload_entry(
    hass: HomeAssistant,
    entry: NrfClawConfigEntry,
) -> bool:
    runtime = entry.runtime_data

    if runtime.mode == MODE_BRIDGE and runtime.session is not None:
        await runtime.session.async_stop()

    unload_ok = await hass.config_entries.async_unload_platforms(
        entry,
        PLATFORMS,
    )

    if unload_ok and runtime.mode == MODE_DIRECT:
        await runtime.coordinator.async_shutdown()

    return unload_ok
