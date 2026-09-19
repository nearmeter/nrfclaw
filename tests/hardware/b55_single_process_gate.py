#!/usr/bin/env python3
import argparse
import asyncio
import json
import os
import sys
from pathlib import Path

# Resolve the repository root from this file so the gate works regardless of
# the caller's current working directory.
REPO_ROOT = Path(__file__).resolve().parents[2]
CLI_DIR = REPO_ROOT / "NRFCLAW_CLI"
if str(CLI_DIR) not in sys.path:
    sys.path.insert(0, str(CLI_DIR))

import nrfclaw_cli as cli


def compact(addr: str) -> str:
    return (addr or "").replace(":", "").replace("-", "").upper()


def pick_devices(rows, bridge_suffix: str, node_suffix: str):
    bridge_suffix = compact(bridge_suffix)
    node_suffix = compact(node_suffix)

    bridge = None
    node = None

    for dev, adv in rows:
        address = getattr(dev, "address", "") or ""
        address_c = compact(address)
        adv_name = getattr(adv, "local_name", None) if adv is not None else None
        name = (adv_name or getattr(dev, "name", None) or "").strip()
        uuids = {
            u.lower()
            for u in ((getattr(adv, "service_uuids", None) or []) if adv is not None else [])
        }

        if address_c.endswith(bridge_suffix):
            is_app = (
                name.upper() == f"NRFCLAW(NDP)-{bridge_suffix}".upper()
                or cli.APP_SERVICE.lower() in uuids
            )
            if is_app:
                bridge = dev

        if address_c.endswith(node_suffix):
            is_nus = (
                name.upper() == f"NRFCLAW(NUS)-{node_suffix}".upper()
                or cli.NUS_SERVICE.lower() in uuids
            )
            if is_nus:
                node = dev

    return bridge, node


async def run(args):
    print("=== B5.5 single-process BLE resolution ===", flush=True)
    rows = await cli.scan_bleak_devices(args.scan_timeout)
    bridge_dev, node_dev = pick_devices(rows, args.bridge, args.node)

    if bridge_dev is None or node_dev is None:
        print("Devices visible to Bleak:", flush=True)
        for dev, adv in rows:
            address = getattr(dev, "address", "") or "-"
            adv_name = getattr(adv, "local_name", None) if adv is not None else None
            name = (adv_name or getattr(dev, "name", None) or "-").strip()
            uuids = (
                list(getattr(adv, "service_uuids", None) or [])
                if adv is not None else []
            )
            print(f"  {address:17} {name}", flush=True)
            if uuids:
                print("    UUIDs: " + ", ".join(uuids), flush=True)

    if bridge_dev is None:
        raise RuntimeError(
            f"Application bridge {args.bridge} was not resolved in the shared scan"
        )
    if node_dev is None:
        raise RuntimeError(
            f"NUS node {args.node} was not resolved in the shared scan; "
            "press P0.21 on the node before running the gate"
        )

    print(
        f"Resolved bridge Application: "
        f"{getattr(bridge_dev, 'name', None) or 'nRFClaw(NDP)'} "
        f"({bridge_dev.address})",
        flush=True,
    )
    print(
        f"Resolved node NUS: "
        f"{getattr(node_dev, 'name', None) or 'nRFClaw(NUS)'} "
        f"({node_dev.address})",
        flush=True,
    )

    app_key = cli.ndp_access_key(os.environ.get("NRFCLAW_NDP_KEY"))
    nus_key = cli.stage5_key(os.environ.get("NRFCLAW_KEY"))

    print("=== B5.5 Application subscription ===", flush=True)
    async with cli.ApplicationNDPClient(bridge_dev, app_key) as app:
        baseline = await app.ninalink_change_subscribe(3)
        print(json.dumps({
            "type": "subscription",
            "schema": baseline["schema"],
            "mask": baseline["mask"],
            "state_revision": baseline["state_revision"],
            "event_revision": baseline["event_revision"],
            "newest_event_id": baseline["newest_event_id"],
        }, sort_keys=True), flush=True)

        print("=== B5.5 node NUS connection (pre-resolved, no new scan) ===", flush=True)
        async with cli.NRFClawClient(node_dev, nus_key) as node:
            print("=== B5.5 state change ===", flush=True)
            await node.ninalink_reliable_test(
                window_ms=600,
                attempts=3,
                backoff_ms=200,
                wait_s=6.0,
                expect_timeout=False,
            )
            state_change = await app.ninalink_wait_change(args.notify_timeout)
            print(json.dumps(
                {"type": "change", **state_change},
                sort_keys=True,
            ), flush=True)

            if not state_change["state_changed"]:
                raise RuntimeError(
                    f"first B5.5 notification was not STATE: {state_change}"
                )
            if state_change["state_revision"] <= baseline["state_revision"]:
                raise RuntimeError(
                    "STATE notification did not advance state_revision"
                )

            print("=== B5.5 event change ===", flush=True)
            await node.ninalink_event_test(
                event_name="tap",
                window_ms=600,
                attempts=3,
                backoff_ms=200,
                wait_s=6.0,
            )
            event_change = await app.ninalink_wait_change(args.notify_timeout)
            print(json.dumps(
                {"type": "change", **event_change},
                sort_keys=True,
            ), flush=True)

            if not event_change["event_changed"]:
                raise RuntimeError(
                    f"second B5.5 notification was not EVENT: {event_change}"
                )
            if event_change["event_revision"] <= baseline["event_revision"]:
                raise RuntimeError(
                    "EVENT notification did not advance event_revision"
                )
            if event_change["newest_event_id"] < 1:
                raise RuntimeError(
                    "EVENT notification did not expose newest_event_id >= 1"
                )

        stats = await app.ninalink_change_stats()
        print(json.dumps({"type": "stats", **stats}, sort_keys=True), flush=True)

        if stats["notifications_sent"] < 2:
            raise RuntimeError(
                f"expected >=2 notifications, got {stats['notifications_sent']}"
            )
        if stats["send_errors"] != 0:
            raise RuntimeError(
                f"B5.5 send_errors={stats['send_errors']}"
            )

        print("PASS Application/NDP subscription established", flush=True)
        print("PASS CAP_REPORT generated STATE change notification", flush=True)
        print("PASS CAP_EVENT generated EVENT change notification", flush=True)
        print("PASS revisions advanced monotonically", flush=True)
        print("PASS event notification carries newest_event_id", flush=True)
        print("PASS notifications sent without send errors", flush=True)
        print("B5.5 HARDWARE GATE: PASS", flush=True)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bridge", default="C8BA09")
    p.add_argument("--node", default="1BF1D0")
    p.add_argument("--scan-timeout", type=float, default=5.0)
    p.add_argument("--notify-timeout", type=float, default=12.0)
    args = p.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
