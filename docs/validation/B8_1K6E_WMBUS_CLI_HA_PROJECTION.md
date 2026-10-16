# B8.1k6e — Wireless M-Bus CLI/HA Projection

Scope: CLI/HA presentation only. No firmware/radio ABI change.

The B8.1k6d hardware gate proved one synthetic transmission maps 1:1 to:
DIO1 -> packet -> RFVM decode -> NinaLink publish.

B8.1k6e adds profile 37 to the CLI-local `gfsk-capabilities` schema:

- name: Wireless M-Bus Mode C Format B
- expected capability set: `{0x0900}`

The Home Assistant catalog already owns profile 37 presentation and capability
hint metadata from B8.1k6c.

The old `ninalink-nodes` CLI command is not restored. Current verification uses
Application-plane commands:

- `ninalink-external-model --json`
- `ninalink-ha-preview --json`

Expected synthetic virtual node:

- meter ID: 12345678 = `0x00BC614E`
- profile ID: 37 = `0x25`
- virtual node: `0xF825614E`

`HARDWARE_VALIDATED` remains false until a real Wireless M-Bus meter is tested.
