"""Home Assistant DataUpdateCoordinator for NinaLink."""

from __future__ import annotations

import asyncio

import logging
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import DOMAIN
from .ninalink_runtime import NinaLinkCoordinatorRuntime

_LOGGER = logging.getLogger(__name__)


class NinaLinkCoordinator(DataUpdateCoordinator[dict[str, Any]]):
    """Push coordinator backed by B5.5 notifications and B5.4 reconciliation."""

    def __init__(
        self,
        hass: HomeAssistant,
        config_entry: ConfigEntry,
        runtime: NinaLinkCoordinatorRuntime,
    ) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name=f"{DOMAIN} NinaLink",
            config_entry=config_entry,
            update_interval=None,
            always_update=False,
        )
        self.runtime = runtime
        self._bootstrap: dict[str, Any] | None = None

    async def _async_setup(self) -> None:
        """Subscribe and establish one authoritative initial snapshot."""
        try:
            self._bootstrap = await self.runtime.initialize()
        except Exception as exc:
            raise UpdateFailed(f"NinaLink initial setup failed: {exc}") from exc

    async def _async_update_data(self) -> dict[str, Any]:
        """Return bootstrap data or an explicit authoritative refresh."""
        try:
            if self._bootstrap is not None:
                data = self._bootstrap
                self._bootstrap = None
                return data
            return await self.runtime.refresh()
        except Exception as exc:
            raise UpdateFailed(f"NinaLink refresh failed: {exc}") from exc


    async def async_write_remote_capability(
        self,
        node_id: int,
        capability_id: int,
        channel: int,
        value_type: int,
        value: int,
    ) -> dict[str, Any]:
        """Queue a remote semantic write without optimistic HA state mutation."""
        try:
            return await self.runtime.builder.remote_cap_set(
                node_id,
                capability_id,
                channel,
                value_type,
                value,
            )
        except Exception as exc:
            raise HomeAssistantError(
                f"NinaLink remote capability write could not be queued: {exc}"
            ) from exc


    async def async_execute_remote_command(
        self,
        node_id: int,
        command_id: int,
        args: bytes = b"",
        *,
        timeout: float = 120.0,
    ) -> dict[str, Any]:
        """Queue and confirm one exact NinaLink COMMAND transaction."""
        try:
            ticket = await self.runtime.builder.remote_command(
                node_id,
                command_id,
                args,
            )
            command_seq = int(ticket["command_seq"])

            loop = asyncio.get_running_loop()
            deadline = loop.time() + float(timeout)
            delay = 1.0

            while True:
                status = await self.runtime.builder.remote_command_status()

                exact = bool(
                    int(status.get("node_id", -1)) == int(node_id)
                    and int(status.get("command_id", -1)) == int(command_id)
                    and int(status.get("command_seq", -1)) == command_seq
                )

                if exact and not bool(status.get("pending", False)):
                    result = int(status.get("result", 255))
                    if result == 0:
                        return status
                    raise HomeAssistantError(
                        "NinaLink remote command failed: "
                        f"node=0x{int(node_id):08X} "
                        f"command=0x{int(command_id):04X} "
                        f"seq={command_seq} result={result}"
                    )

                if loop.time() >= deadline:
                    raise HomeAssistantError(
                        "NinaLink remote command timed out: "
                        f"node=0x{int(node_id):08X} "
                        f"command=0x{int(command_id):04X} "
                        f"seq={command_seq}"
                    )

                await asyncio.sleep(delay)
                delay = min(delay * 1.5, 8.0)

        except HomeAssistantError:
            raise
        except Exception as exc:
            raise HomeAssistantError(
                "NinaLink remote command could not be completed: "
                f"{exc}"
            ) from exc

    async def async_process_change(
        self,
        change: dict[str, Any],
    ) -> dict[str, Any]:
        """Reconcile one B5.5 hint and push the new model to HA entities."""
        try:
            data = await self.runtime.reconcile(change)
        except Exception as exc:
            raise UpdateFailed(f"NinaLink reconciliation failed: {exc}") from exc

        self.async_set_updated_data(data)
        return data

    async def async_wait_and_process(
        self,
        timeout: float = 45.0,
    ) -> dict[str, Any]:
        """Wait for one B5.5 notification and publish its reconciliation."""
        change = await self.runtime.wait_change(timeout)
        return await self.async_process_change(change)
