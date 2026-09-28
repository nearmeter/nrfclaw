"""Pure helpers for B7.4 Home Assistant entity adaptation."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Iterable, Iterator


def iter_platform_entities(
    data: Dict[str, Any],
    platform: str,
) -> Iterator[tuple[Dict[str, Any], Dict[str, Any]]]:
    """Yield (device, entity descriptor) pairs for one HA platform."""
    for device in data.get("ha", {}).get("devices", []):
        for entity in device.get("entities", []):
            if entity.get("platform") == platform:
                yield device, entity


def find_entity(
    data: Dict[str, Any] | None,
    unique_key: str,
) -> Dict[str, Any] | None:
    if not data:
        return None
    for _device, entity in iter_all_entities(data):
        if entity.get("unique_key") == unique_key:
            return entity
    return None


def iter_all_entities(
    data: Dict[str, Any],
) -> Iterator[tuple[Dict[str, Any], Dict[str, Any]]]:
    for device in data.get("ha", {}).get("devices", []):
        for entity in device.get("entities", []):
            yield device, entity


def matching_events(
    data: Dict[str, Any] | None,
    node_id_raw: int,
    capability_id_raw: int,
    channel: int,
) -> list[Dict[str, Any]]:
    if not data:
        return []
    out = []
    for event in data.get("events", []):
        if (
            int(event.get("node_id_raw", -1)) == node_id_raw
            and int(event.get("capability_id_raw", -1)) == capability_id_raw
            and int(event.get("channel", -1)) == channel
        ):
            out.append(event)
    return out


def u32_is_newer(value: int, previous: int) -> bool:
    """Serial-number comparison for uint32 event ids."""
    delta = (int(value) - int(previous)) & 0xFFFFFFFF
    return 0 < delta < 0x80000000


@dataclass
class EventIdGate:
    """Suppress replay of already published NinaLink events."""

    last_event_id: int | None = None

    def reset_stream(self) -> None:
        self.last_event_id = None

    def accept(self, event_id: int) -> bool:
        event_id &= 0xFFFFFFFF
        if self.last_event_id is None:
            self.last_event_id = event_id
            return True
        if not u32_is_newer(event_id, self.last_event_id):
            return False
        self.last_event_id = event_id
        return True

    def prime(self, events: Iterable[Dict[str, Any]]) -> None:
        """Prime without emitting, preventing replay when entity is added."""
        for event in events:
            event_id = int(event["event_id"])
            if self.last_event_id is None or u32_is_newer(
                event_id, self.last_event_id
            ):
                self.last_event_id = event_id
