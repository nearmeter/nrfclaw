# B8.1j — rtl_433_ESP Candidate Catalog + HA Vocabulary

Generated candidate exact-RF profiles: **32**
Selected module: **E220-900MM22S**
Highest assigned profile ID: **36**

Selection rule:

```text
rtl_433_ESP FSK
  -> PACKET_NATIVE_CANDIDATE
  -> exact RF variant
  -> B8.1d module status SUPPORTED
  -> nRFClaw CATALOG-only profile
```

These entries are intentionally **not** RFVM-ready, radio-ready,
capability-ready, runtime-ready, or hardware-validated.

Unknown PHY parameters remain zero. B8.1j does not invent deviation, RX
bandwidth, packet policy, CRC, whitening, or decoder behavior.

The generated source-field inventory is converted to capability **hints** for
future RFVM promotion gates. Hints do not synthesize sensor values.

B8.1j also expands RF profile identity from 4 to 8 bits and introduces a
backward-compatible persistence v2. Existing v1 persisted profiles remain
readable.

Home Assistant version: **0.10.27**.
