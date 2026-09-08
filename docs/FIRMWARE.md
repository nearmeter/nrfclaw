# Firmware knowledge base

## Core modules

- `nrfclaw_vm.*` — VM interpreter and bytecode execution.
- `nrfclaw_schedule.*`, `nrfclaw_scheduler.*`, `nrfclaw_rtc.*` — MANUAL/BOOT scheduling and timed wakeups.
- `nrfclaw_ble.*` — physical-button NUS programming plane.
- `nrfclaw_ble_app.*` — Application/NDP and VM-controlled advertising roles.
- `nrfclaw_ndp*` — NDP v1 application protocol, access and key storage.
- `nrfclaw_lora.*`, `nrfclaw_llcc68_rl.*`, `nrfclaw_lora_profile*` — LLCC68 radio and persisted RF profile.
- `nrfclaw_serial.*` — UART primitives/routing.
- `nrfclaw_ds18b20.*`, `nrfclaw_battery.*`, `nrfclaw_inputs.*`, `nrfclaw_lis2dh12.*` — sensors/inputs.
- `nrfclaw_vib_auto*`, `nrfclaw_vib_health.*` — learned vibration behavior and events.
- `nrfclaw_tracking*` — autonomous OpenHaystack-compatible tracking.
- `nrfclaw_system_power.*` — minimum-power policy.
- `nrfclaw_flash.*`, `nrfclaw_state.*`, `nrfclaw_factory.*` — persistent state and factory data.
- `nrfclaw_board_api.*` plus `boards/*` — board abstraction.

## BLE programming behavior

Programming is physically opened with P0.21. The programming plane uses Nordic UART Service UUIDs and a board-specific `nRFClaw(NUS)-XXXXXX` identity. The current CoreBluetooth-compatible configuration uses conservative legacy 1M behavior and explicit handling for security parameters, PHY updates and missing system attributes.

## Application BLE

NDP advertises separately as `nRFClaw(NDP)-XXXXXX`. A VM program can select Advertiser role, configure interval/name, start advertising and replace advertising data with the VM buffer. Dynamic advertising data and the GAP device name are separate concepts.

## Low power

Do not add permanently running timers/poll loops where an RTC/event wakeup works. VIB_AUTO learning-window persistence is intentional. Tracking is advertising-based and should not be classified as a continuous high-consumption receiver. LoRa RX and serial RX deserve special treatment because listening requires active receiver hardware.

## LoRa

The LLCC68 profile is persisted. Unless a program explicitly changes RF parameters, host/compiler logic should preserve that profile. TX-only paths should return the radio to sleep after TX. Do not add arbitrary current-limit programming to the radio initialization without a hardware requirement.

## VM ABI compatibility

`include/nrfclaw_opcodes.h` is the embedded ABI authority. Host compiler changes must use existing opcodes unless firmware and CLI are versioned together. New language support should prefer new deterministic lowerers over changing opcode meaning.
