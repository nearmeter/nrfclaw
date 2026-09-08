# Protocol and compatibility notes

## BLE planes

Programming NUS UUIDs use the Nordic UART Service UUID family. Application NDP uses the nRFClaw Application GATT service (`4e52a800-0000-10a7-d24b-6a3244619a47`) with RX `...a801...` and TX `...a802...` characteristics.

Normal names distinguish the planes:

```text
nRFClaw(NDP)-XXXXXX   Application/NDP
nRFClaw(NUS)-XXXXXX   Physical programming
nRFClaw-DFU           Bootloader update mode
```

## NDP v1

NDP v1 is the stable application contract. Existing opcode meanings and binary layouts must not be changed in place. Add unused selectors/opcodes or introduce a future protocol revision.

The currently validated application transport is designed around the normal ATT MTU 23 constraint for complete small request/response frames; internal buffer sizes do not imply fragmentation support.

## VM schedule ABI

The host and firmware share MANUAL/BOOT schedule semantics. Host tooling should normalize and validate schedules rather than infer persistence from unrelated UI state.

## Programming security boundary

NUS is intentionally tied to physical-button access. Application NDP can have its own access/authentication policy. Do not merge the two trust models simply because both use BLE.
