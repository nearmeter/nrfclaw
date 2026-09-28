"""B7.5 persistent session supervisor and reconnect recovery."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable
from typing import Any

from .ninalink_runtime import NinaLinkCoordinatorRuntime

_LOGGER = logging.getLogger(__name__)


class NinaLinkSessionManager:
    """Own the live BLE session after initial config-entry setup."""

    def __init__(
        self,
        *,
        client: Any,
        coordinator: Any,
        client_factory: Callable[[], Awaitable[Any]],
        bridge_opcode: int,
        bridge_key: str,
        task_factory: Callable[[Awaitable[Any]], asyncio.Task[Any]] | None = None,
    ) -> None:
        self.client = client
        self.coordinator = coordinator
        self.client_factory = client_factory
        self.bridge_opcode = bridge_opcode
        self.bridge_key = bridge_key
        self.task_factory = task_factory or asyncio.create_task
        self._task: asyncio.Task[Any] | None = None
        self._stopping = False

    def start(self) -> None:
        if self._task is None:
            self._task = self.task_factory(self._run())

    async def async_stop(self) -> None:
        self._stopping = True
        task = self._task
        self._task = None
        if task is not None:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        await self.client.close()

    async def _recover_connection(self, saved_cursor: int) -> None:
        old_generation = (
            self.coordinator.data.get("generation", 0)
            if self.coordinator.data
            else 0
        )

        delay = 1.0
        while not self._stopping:
            client = None
            try:
                client = await self.client_factory()
                await client.connect()

                runtime = NinaLinkCoordinatorRuntime(
                    client,
                    self.bridge_opcode,
                    self.bridge_key,
                )
                runtime.generation = old_generation
                data = await runtime.recover(saved_cursor)

                self.client = client
                self.coordinator.runtime = runtime
                self.coordinator.async_set_updated_data(data)
                return
            except asyncio.CancelledError:
                if client is not None:
                    await client.close()
                raise
            except Exception as exc:
                if client is not None:
                    await client.close()
                _LOGGER.warning(
                    "NinaLink reconnect failed; retrying in %.1fs: %s",
                    delay,
                    exc,
                )
                await asyncio.sleep(delay)
                delay = min(delay * 2.0, 30.0)

    async def _run(self) -> None:
        while not self._stopping:
            try:
                try:
                    await self.coordinator.async_wait_and_process(60.0)
                except TimeoutError:
                    data = await self.coordinator.runtime.refresh()
                    self.coordinator.async_set_updated_data(data)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                if self._stopping:
                    return
                runtime = self.coordinator.runtime
                saved_cursor = runtime.reconciler.event_cursor
                _LOGGER.info(
                    "NinaLink live session lost at event cursor %u: %s",
                    saved_cursor,
                    exc,
                )
                await self.client.close()
                await self._recover_connection(saved_cursor)
