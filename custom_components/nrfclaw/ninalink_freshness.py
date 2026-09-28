"""NinaLink node freshness policy for Home Assistant."""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Any

UNKNOWN_CADENCE_OFFLINE_AFTER_S = 7200
MIN_OFFLINE_AFTER_S = 60
MAX_OFFLINE_AFTER_S = 14400
MAX_LEARNABLE_REPORT_INTERVAL_S = 3600
MISSED_REPORT_MULTIPLIER = 3.0


@dataclass
class _FreshnessState:
    last_contact_monotonic: float | None = None
    expected_interval_s: float | None = None


class NodeFreshnessTracker:
    """Learn report cadence and annotate external-model nodes with online state."""

    def __init__(self) -> None:
        self._state: dict[int, _FreshnessState] = {}

    @staticmethod
    def _age_s(node: dict[str, Any]) -> int:
        try:
            return max(0, int(node.get("age_s", 0)))
        except (TypeError, ValueError):
            return 0

    @staticmethod
    def _timeout_s(expected_interval_s: float | None) -> int:
        if expected_interval_s is None:
            return UNKNOWN_CADENCE_OFFLINE_AFTER_S
        timeout = round(expected_interval_s * MISSED_REPORT_MULTIPLIER)
        return max(MIN_OFFLINE_AFTER_S, min(MAX_OFFLINE_AFTER_S, timeout))

    def observe(
        self,
        model: dict[str, Any],
        *,
        now_monotonic: float | None = None,
    ) -> dict[str, Any]:
        now = time.monotonic() if now_monotonic is None else float(now_monotonic)
        present: set[int] = set()

        for node in model.get("nodes", []):
            node_id = int(node.get("node_id_raw", 0))
            if not node_id:
                continue
            present.add(node_id)

            age_s = self._age_s(node)
            contact = now - float(age_s)
            state = self._state.setdefault(node_id, _FreshnessState())

            previous_contact = state.last_contact_monotonic
            if previous_contact is None:
                state.last_contact_monotonic = contact
            elif contact > previous_contact + 1.0:
                interval = contact - previous_contact
                state.last_contact_monotonic = contact
                if 5.0 <= interval <= MAX_LEARNABLE_REPORT_INTERVAL_S:
                    if state.expected_interval_s is None:
                        state.expected_interval_s = interval
                    else:
                        state.expected_interval_s = (
                            state.expected_interval_s * 0.75 + interval * 0.25
                        )

            timeout_s = self._timeout_s(state.expected_interval_s)
            node["online"] = bool(
                node.get("valid", False)
                and node.get("ready", False)
                and age_s <= timeout_s
            )
            node["offline_after_s"] = timeout_s
            node["expected_report_interval_s"] = (
                None
                if state.expected_interval_s is None
                else int(round(state.expected_interval_s))
            )

        for node_id in tuple(self._state):
            if node_id not in present:
                del self._state[node_id]

        return model
