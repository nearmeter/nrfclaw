# B4.10 — Local Capability Discovery Diagnostic

Purpose: validate B4.10 capability metadata on boards that do not populate the
LLCC68/LoRa radio.

The diagnostic reuses exactly:

    nrfclaw_ninalink_capability_discovery_build_page()

the same page builder used by the NinaLink CAPS_RESPONSE path.

Existing physical-NUS opcode:
    NDP_NINALINK_LINK = 91 / 0x5B

Subactions:
    10,page
      -> registry_version,page,count,more

    11,page,index
      -> 9-byte capability descriptor

Descriptor ABI:
    capability_id:u16 LE
    channel:u8
    kind:u8
    value_type:u8
    scale10:s8
    unit:u8
    behavior_flags:u8
    runtime_state_flags:u8

No NinaLink RF wire-format change is made.

CLI:
    python3 nrfclaw_cli.py --device F07138 ninalink-capability-local --page 0

Repeat pages until More=no.

Expected hardware comparison:

Without LIS2DH:
    LIS2DH-derived capabilities => SUPPORTED

With detected LIS2DH:
    LIS2DH-derived capabilities => SUPPORTED|PRESENT

ENABLED may additionally appear while the corresponding runtime mode owns the
sensor.

F07138 already validated:
- I2C 0x19
- WHO_AM_I 0x33
- coherent XYZ orientation
- 31-sample FIFO capture at 200 Hz
- preserved vibration metrics after automatic sensor power-down
