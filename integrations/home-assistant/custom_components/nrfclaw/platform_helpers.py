"""Dynamic NinaLink platform entity registration."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any


def setup_dynamic_entities(
    entry: Any,
    async_add_entities: Callable[[list[Any]], None],
    factory: Callable[[Any, str], list[Any]],
) -> None:
    """Add entities initially and when new node/capability inventory appears."""
    runtime = entry.runtime_data
    coordinator = runtime.coordinator
    known: set[str] = set()

    def _sync() -> None:
        fresh = []
        for entity in factory(
            coordinator,
            runtime.bridge_device_id,
        ):
            unique_id = entity.unique_id
            if unique_id in known:
                continue
            known.add(unique_id)
            fresh.append(entity)
        if fresh:
            async_add_entities(fresh)

    entry.async_on_unload(coordinator.async_add_listener(_sync))
    _sync()
