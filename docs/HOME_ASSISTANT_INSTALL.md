# Installing nRFClaw on Home Assistant

The nRFClaw Home Assistant integration does not require HACS. The release
artifact is a normal ZIP containing the complete `custom_components/nrfclaw`
tree.

## Download

Use:

```text
https://github.com/nearmeter/nrfclaw/releases/latest/download/nrfclaw-home-assistant.zip
```

or download `nrfclaw-home-assistant.zip` from the latest nRFClaw GitHub
Release.

## Copy

Extract the archive and copy:

```text
custom_components/nrfclaw/
```

to:

```text
/config/custom_components/nrfclaw/
```

Replace the complete directory when updating from an older release so stale
modules cannot remain behind.

## Home Assistant OS: easiest file-copy method

The official Home Assistant **Samba share** app is available directly in the
built-in App store:

```text
Settings → Apps → Install app → Samba share
```

Configure a username/password and make sure the `config` share is enabled.
Then connect from the computer used to manage Home Assistant:

```text
Windows:       \\<HOME_ASSISTANT_IP>\config
macOS / Linux: smb://<HOME_ASSISTANT_IP>/config
```

Copy the extracted `custom_components` directory into that share.

Official Samba share source/documentation:

```text
https://github.com/home-assistant/addons/tree/master/samba
```

The official **File editor** app can be used to inspect the installed files:

```text
Settings → Apps → Install app → File editor
https://github.com/home-assistant/addons/tree/master/configurator
```

Neither method requires HACS or a custom App repository.

## Restart and discovery

Restart Home Assistant:

```text
Settings → System → Restart Home Assistant
```

Then power or reset a board configured as nRFClaw **Direct** or **NinaLink
Bridge**. Home Assistant Bluetooth discovery should offer the device under:

```text
Settings → Devices & services
```

If Direct NDP protection is enabled, paste the device's 256-bit NDP access key
into the setup flow.

NinaLink low-power Nodes do not connect directly to Home Assistant. They are
discovered and exposed through the Bridge.

## Package maintainers

The package is built only from:

```text
integrations/home-assistant/custom_components/nrfclaw/
```

Build and verify locally:

```bash
python3 tools/build_home_assistant_package.py
```

Default outputs:

```text
dist/nrfclaw-home-assistant.zip
dist/nrfclaw-home-assistant.zip.sha256
```

The builder verifies that every archived file matches the canonical source
byte-for-byte and validates the expected package layout before reporting PASS.
