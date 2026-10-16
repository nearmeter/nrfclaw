# B8.1g-r3 — WH51 structural registry corrective

The r2 failure with only `14U` proved it matched the earlier `rfvm_wh51`
descriptor instead of the WH51 entry in `profiles[]`.

r3 first enters `static const nrfclaw_rf_profile_entry_t profiles[]`, then
parses the WH51 top-level initializer fields and changes only:

- frequency: 915000000 Hz
- bitrate: 17241 bps
- deviation: 30000 Hz
- RX bandwidth: 93800 Hz
- LLCC68 BW code: 0x13
- preamble: 24 bits

Sync `2D D4`, expected length 14, flags and RFVM pointer are preserved.

No revert is required before applying r3.
