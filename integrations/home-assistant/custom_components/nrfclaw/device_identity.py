"""Canonical Home Assistant presentation identity for nRFClaw devices."""

from __future__ import annotations

from typing import Any


def compact_suffix(value: str | None) -> str:
    compact = (value or "").replace(":", "").replace("-", "").upper()
    return compact[-6:] if len(compact) >= 6 else compact or "UNKNOWN"


def direct_device_name(board_name: str | None, address: str | None) -> str:
    model = board_name or "nRFClaw"
    return f"{model}-{compact_suffix(address)} Direct"


def bridge_device_name(address: str | None) -> str:
    return f"NINASENSE-{compact_suffix(address)} Bridge"


def ninalink_node_identity(node: dict[str, Any]) -> tuple[str, str | None, str]:
    """Return (name, manufacturer, model) for a projected NinaLink node.

    B7.6f2e's current external node model predates explicit board metadata, so
    the shipping NinaLink-node profile defaults to Nearmeter/NINASENSE.  Future
    model/manufacturer fields, when present, override that fallback without
    changing stable node identifiers.
    """
    node_id = int(node["node_id_raw"])
    model = str(node.get("model") or "NINASENSE")
    manufacturer = node.get("manufacturer")
    if manufacturer is None and model == "NINASENSE":
        manufacturer = "Nearmeter"
    name = f"{model}-{node_id & 0xFFFFFF:06X} Node"
    return name, manufacturer, model
