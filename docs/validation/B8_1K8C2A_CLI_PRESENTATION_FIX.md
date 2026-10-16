# B8.1k8c2a — CLI Presentation Fix

Host-only corrective after B8.1k8c2.

Changes:

- `python3 nrfclaw_cli.py ha scan` no longer requires `--device`.
- Supplying `--device` to `ha scan` remains optional and only marks a target.
- `ha device-info` knows native CAP15 as `HUMIDITY`.
- `ha sensors` labels CAP14 as `Temperature`, not `DS18B20`.
- `ha sensors` shows the logical temperature source: DS18B20, NRF52_INTERNAL, or SHT.
- `ha sensors` reads selector 7 only when CAP15/HUMIDITY is supported and prints `%RH`.
- With no SHT, `Humidity: unavailable` is expected.

This revision changes only the host CLI, gate, and documentation. It does not
change firmware, NDP ABI, Home Assistant integration files, radio behavior, or
low-power firmware behavior. Firmware rebuild/flash is not required.
