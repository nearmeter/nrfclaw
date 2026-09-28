# nRFClaw Home Assistant installation with HACS

B7.6f2p1 makes the nRFClaw repository directly installable as a custom HACS
integration repository. No SSH shell, Terminal add-on, or manual copy into
`/config/custom_components` is required.

## Install

Use the My Home Assistant link:

https://my.home-assistant.io/redirect/hacs_repository/?owner=nearmeter&repository=nrfclaw&category=integration

Then:

1. Download nRFClaw in HACS.
2. Restart Home Assistant.
3. Power/reset a device configured as Home Assistant Direct or NinaLink Bridge.
4. Accept the Bluetooth discovery card.
5. For protected Direct NDP, enter the 256-bit access key generated through the
   physical P0.21/NUS management plane.

## Transport roles

- **Direct**: Home Assistant connects to the nRFClaw device over BLE/NDP.
- **NinaLink Bridge**: Home Assistant connects to the Bridge over BLE/NDP; the
  Bridge continuously receives NinaLink LoRa traffic.
- **NinaLink Node**: the low-power node keeps Application/NDP off and reports to
  the Bridge over LoRa. Nodes appear in Home Assistant through the Bridge.

## Repository layout

HACS requires the integration to be present under:

```text
custom_components/nrfclaw/
```

The historical/canonical source tree remains:

```text
integrations/home-assistant/custom_components/nrfclaw/
```

B7.6f2p1 keeps a distribution mirror at the HACS-required root location.
Run:

```bash
python3 tools/check_hacs_mirror.py --sync
python3 tools/check_hacs_mirror.py
```

after changing the canonical Home Assistant integration. CI rejects any drift.

## Validation

The GitHub workflow runs:

- mirror parity check;
- JSON validation;
- Python byte-compilation;
- HACS validation;
- Home Assistant hassfest validation.

For the first custom-repository phase, the HACS action ignores only the GitHub
`topics` check. Configure repository topics such as `home-assistant`,
`hacs`, `bluetooth`, `lora`, and `nrf52` before submitting nRFClaw to
the HACS default store, then remove that ignore.
