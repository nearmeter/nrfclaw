# B2 — Semantic Capability Runtime Layer

Status: candidate implementation.

## Goal

Implement a read-only semantic adapter layer between existing nRFClaw drivers
and the B1.1 capability registry.

B2 does **not**:

- send LoRa packets;
- add bridge mode;
- change BLE/NDP;
- change VM opcodes;
- change existing sensor-driver behavior;
- start sensors merely because the capability layer exists.

## Files

New:

```text
include/nrfclaw_capability.h
src/nrfclaw_capability.c
```

Modified:

```text
Makefile
```

The Makefile only adds `src/nrfclaw_capability.c` to `CORE_SRC`.

## Runtime model

B2 exposes:

```text
registry descriptor lookup
registry enumeration
runtime state query
current/cached value read
```

Current drivers remain the source of truth.

Examples:

```text
battery_last()              -> battery_voltage
ds18b20_last_mC()           -> temperature
lis2dh12_read_xyz()         -> acceleration_x/y/z
vibration_metrics()         -> vibration metrics
hall_count()                -> counter
hall_position()             -> quadrature_position
tracking_active()           -> tracking_active
```

No B0 primitive is redirected through this API yet.

## Runtime state

B2 separates:

```text
SUPPORTED
PRESENT
ENABLED
FAULT
```

For optional peripherals this is deliberately more precise than the legacy
native capability mask.

Example:

```text
NINASENSE supports LIS2DH12
but component is not populated:

SUPPORTED=1
PRESENT=0
ENABLED=0
```

## Current-value reads

Reads are best-effort and do not initiate asynchronous acquisitions.

Therefore:

- battery read succeeds only after `nrfclaw_battery_last()` has a sample;
- temperature succeeds only after DS18B20 has a cached sample;
- event-only capabilities such as `tap` and `fall` are not readable;
- unsupported/future capabilities return false.

This keeps B2 free of new timers, power behavior and state machines.

## B2 gate

1. Apply B1.2 documentation commit.
2. Apply B2 code.
3. `make clean && make -j$(nproc)`.
4. `make size`.
5. Compare with B0:
   - text 170672
   - data 604
   - bss 8184
6. Run existing B0 smoke (`ha test`, LoRa diagnostic smoke if desired).
7. No existing BLE/NDP/VM behavior may change.

The B2 implementation is not wired into `main.c`; therefore normal runtime
behavior remains unchanged until B3 consumes the API.

Suggested commit:

```text
B2: add semantic capability runtime layer
```
