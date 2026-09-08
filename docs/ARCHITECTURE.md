# nRFClaw architecture

nRFClaw separates intent, deterministic compilation and embedded execution. The host CLI accepts natural language, pseudo-code or Semantic IR. Supported semantics are normalized and validated, lowered to the frozen VM ABI, and uploaded as bytecode. The nRF52832 does not run an LLM.

## Runtime blocks

`src/main.c` initializes the board and core services. The major subsystems are deliberately separated: VM (`nrfclaw_vm`), scheduler/schedules, event dispatch, system power, BLE programming/application planes, LoRa/LLCC68, serial, sensors, VIB_AUTO, tracking, persistent flash/state, authentication/NDP access and board abstraction.

## Event-driven model

Battery life depends on avoiding polling. Motion, FALL, Hall and VIB_AUTO are represented as events. Periodic work should use the scheduler/RTC. LoRa TX should wake the radio only around a transmission. Continuous LoRa RX and continuous UART RX are intrinsically higher-power because a receiver remains active.

## BLE ownership

There is one application advertising resource, so ownership is explicit. NDP Peripheral is the normal application plane. VM Advertiser/Beacon and Tracking can take ownership. P0.21/NUS programming has recovery/programming priority. When changing away from NDP, the firmware stops the old advertising set and its adaptive timer before the new role configures it.

## Schedules

VM programs are MANUAL or BOOT. MANUAL is explicit execution. BOOT is persisted and eligible to execute autonomously after restart. Host software must not silently equate “uploaded” with “running after restart.”

## Trust boundary

An AI/agent may propose Semantic IR, but it is not trusted bytecode. Schema/capability checks and deterministic local lowering remain authoritative. Unsupported compositions should fail rather than silently dropping clauses.
