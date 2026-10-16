# B8.1g-r4 — Profile-Authoritative External-RF Persistence

The B8.1g WH51 command initially returned:

```text
NDP 0x63: BUSY
```

Selector 21 maps any `false` from
`nrfclaw_bridge_radio_mode_set_external_persist()` to `NDP_BUSY`.

Two B8.0 persistence assumptions were still VEVOR-specific:

1. `config_valid()` accepted only RX BW codes `0x0B` and `0x1A`.
2. the packed state allocated only four bits to the persisted BW code.

WH51 uses `0x13`, so it was rejected. Even if accepted, raw `0x13` would be
truncated to `0x03` in the four-bit field.

B8.1g-r4 makes the RF profile registry authoritative for PHY bandwidth.

Persistent selection becomes:

```text
profile_id + exact center frequency
        ↓
profile registry
        ↓
bitrate / deviation / RX BW / preamble / sync / payload
```

The old four-bit BW field remains only as a compatibility token. On boot the
complete `rx_bw_code` is restored from the selected profile entry.

No state erase is required.
