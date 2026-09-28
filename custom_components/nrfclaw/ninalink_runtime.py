"""B7.3 — Home Assistant NinaLink coordinator runtime.

Pure-Python runtime shared by the Home Assistant coordinator and hardware gates.
It consumes an already connected Application/NDP session exposing:
- ninalink_change_subscribe(mask)
- ninalink_wait_change(timeout)
- ndp_command(opcode, payload)
"""

from __future__ import annotations

from typing import Any, Dict

try:
    from .ninalink_external import (
        NinaLinkExternalModelBuilder,
        NinaLinkExternalReconciler,
    )
    from .ninalink_semantics import project_model
    from .ninalink_freshness import NodeFreshnessTracker
except ImportError:  # CLI/hardware gate import
    from ninalink_external import (
        NinaLinkExternalModelBuilder,
        NinaLinkExternalReconciler,
    )
    from ninalink_semantics import project_model
    from ninalink_freshness import NodeFreshnessTracker


class NinaLinkCoordinatorRuntime:
    """Bridge B5.4/B5.5/B7.2 state into one HA-facing push model."""

    def __init__(
        self,
        app: Any,
        bridge_opcode: int,
        bridge_key: str,
    ) -> None:
        self.app = app
        self.bridge_opcode = bridge_opcode
        self.bridge_key = bridge_key
        self.builder = NinaLinkExternalModelBuilder(app, bridge_opcode)
        self.reconciler = NinaLinkExternalReconciler(app, bridge_opcode)
        self.data: Dict[str, Any] | None = None
        self.generation = 0
        self.freshness = NodeFreshnessTracker()

    async def _attach_inventory(self, model: Dict[str, Any]) -> None:
        for node in model["nodes"]:
            inv = await self.builder.read_inventory(node["node_id_raw"])
            node["inventory_valid"] = inv["valid"]
            node["inventory_complete"] = inv["complete"]
            node["inventory_overflow"] = inv["overflow"]
            node["inventory_registry_version"] = inv["registry_version"]
            node["inventory"] = inv["capabilities"]

    def _publish(
        self,
        *,
        model: Dict[str, Any],
        events: list[Dict[str, Any]],
        reason: str,
        state_revision: int,
        event_revision: int,
        change_revision: int,
        overrun: bool = False,
        stream_reset: bool = False,
    ) -> Dict[str, Any]:
        self.generation += 1
        self.freshness.observe(model)
        ha = project_model(model, self.bridge_key, events)

        self.data = {
            "type": "ha_coordinator",
            "generation": self.generation,
            "reason": reason,
            "bridge_key": self.bridge_key,
            "state_revision": state_revision,
            "event_revision": event_revision,
            "change_revision": change_revision,
            "event_cursor": self.reconciler.event_cursor,
            "overrun": overrun,
            "stream_reset": stream_reset,
            "events": events,
            "model": model,
            "ha": ha,
        }
        return self.data

    async def initialize(self) -> Dict[str, Any]:
        """Subscribe first, then establish the authoritative initial model."""
        baseline = await self.app.ninalink_change_subscribe(3)
        initial = await self.reconciler.initialize(baseline)
        model = initial["model"]
        await self._attach_inventory(model)

        return self._publish(
            model=model,
            events=[],
            reason="initial",
            state_revision=initial["state_revision"],
            event_revision=initial["event_revision"],
            change_revision=initial["change_revision"],
        )


    async def recover(self, saved_cursor: int) -> Dict[str, Any]:
        """Reconnect without losing events which arrived while BLE was down."""
        if not 0 <= saved_cursor <= 0xFFFFFFFF:
            raise ValueError("saved_cursor must be uint32")

        baseline = await self.app.ninalink_change_subscribe(3)
        model = await self.builder.read()
        await self._attach_inventory(model)

        recovered = await self.reconciler.read_events(saved_cursor)
        self.reconciler.model = model
        self.reconciler.event_cursor = recovered["next_cursor"]
        self.reconciler.state_revision = baseline["state_revision"]
        self.reconciler.event_revision = baseline["event_revision"]
        self.reconciler.change_revision = 0

        model["event_cursor"] = recovered["next_cursor"]
        model["event_window"] = recovered["window"]

        return self._publish(
            model=model,
            events=recovered["events"],
            reason="reconnect",
            state_revision=baseline["state_revision"],
            event_revision=baseline["event_revision"],
            change_revision=0,
            overrun=recovered["overrun"],
            stream_reset=recovered["stream_reset"],
        )

    async def refresh(self) -> Dict[str, Any]:
        """Manual authoritative snapshot refresh without advancing event cursor."""
        if self.data is None:
            return await self.initialize()

        saved_cursor = self.reconciler.event_cursor
        model = await self.builder.read()
        await self._attach_inventory(model)
        model["event_cursor"] = saved_cursor

        self.reconciler.model = model
        self.reconciler.event_cursor = saved_cursor

        return self._publish(
            model=model,
            events=[],
            reason="refresh",
            state_revision=self.reconciler.state_revision,
            event_revision=self.reconciler.event_revision,
            change_revision=self.reconciler.change_revision,
        )

    async def reconcile(self, change: Dict[str, Any]) -> Dict[str, Any]:
        """Apply one B5.5 change hint through authoritative B5.4 reads."""
        row = await self.reconciler.reconcile(change)
        model = row["model"]

        # B6 state rebuilds do not know B7.2 inventory, so re-attach it after
        # every change. This also handles a newly completed discovery epoch.
        await self._attach_inventory(model)

        return self._publish(
            model=model,
            events=row["events"],
            reason="change",
            state_revision=row["state_revision"],
            event_revision=row["event_revision"],
            change_revision=row["change_revision"],
            overrun=row["overrun"],
            stream_reset=row["stream_reset"],
        )

    async def wait_change(self, timeout: float = 45.0) -> Dict[str, Any]:
        return await self.app.ninalink_wait_change(timeout)

    async def wait_and_reconcile(self, timeout: float = 45.0) -> Dict[str, Any]:
        change = await self.wait_change(timeout)
        return await self.reconcile(change)
