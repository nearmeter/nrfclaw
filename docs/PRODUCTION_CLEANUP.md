# B7.6f2n1 — Production Repository / Instrumentation Cleanup

This cleanup removes historical development artifacts and firmware surfaces
that existed only to support completed hardware gates. It deliberately keeps
core product semantics, NinaLink protocol engines, Home Assistant behavior,
VM functionality, and low-power runtime logic.

## Removed from repository

- `.pre_b*` patch backups
- Python bytecode / `__pycache__`
- bootloader build products
- historical `docs/validation/` gate archive
- one-off `b412c_*gate*.py` and `b76*_scan.py` scripts
- CLI commands that targeted retired lab/gate/trace endpoints
- B5.5 hardware-gate runtime module
- NinaLink autonomous lab runtime module

## Removed from linked firmware surface

- SEGGER RTT runtime logging and RTT/log SDK sources
- Beacon/Broadcaster test NDP endpoint
- direct LoRa diagnostic NDP endpoints
- NinaLink lab NDP endpoint
- NinaLink node/link hardware-gate NDP endpoint
- obsolete Bridge gate/status selectors 3..7, 18..29, 38..39, 43..51, 53
- VIB_AUTO diagnostic status pages 2+ (production pages 0/1 retained)
- BLE beacon test configuration API
- RTT-only VM debug text formatting
- physical-NUS EVENT_INJECT and TRACKING_DEBUG test commands

The default application optimization changes from `-O3` to `-Os`.
`--gc-sections` and function/data sections remain enabled.

## Intentionally retained for a measured second pass

Some diagnostic counters remain embedded inside VIB_AUTO, LIS2DH12, tracking,
LoRa and NinaLink reliability internals. They are no longer exposed by the
production NDP surface, but removing them is a more invasive source change.
After this n1 tree builds on the real SDK/toolchain, use `make size-detail` to
measure the remaining linked contributors before n2 removes those counters.

## CLI compatibility

The generic `ninalink-cap-set` command was migrated from historical selectors
6/7/53 to the production HA selectors 54/55. Generic COMMAND and discovery
commands remain available. Lab-only node traces, loss injection and synthetic
gate commands were removed from the public CLI together with their firmware
endpoints.
