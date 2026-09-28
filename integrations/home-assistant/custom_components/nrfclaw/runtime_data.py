"""Runtime objects owned by one nRFClaw config entry."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(slots=True)
class NrfClawRuntimeData:
    """Per-entry runtime for either Direct NDP or persistent NinaLink bridge."""

    mode: str
    coordinator: Any
    bridge_device_id: str | None = None
    session: Any | None = None
