# nRFClaw CLI

The CLI is the host-side interface for nRFClaw. It discovers the physical-button NUS programming plane, uploads/runs VM programs, inspects the Application/NDP plane, performs RF/sensor diagnostics, upgrades application firmware through the nRFClaw BLE bootloader, and compiles natural-language or pseudo-code into deterministic VM bytecode.

## Requirements

- Python 3.10+
- `bleak>=0.21`
- A working Bluetooth LE adapter

Install:

```bash
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 nrfclaw_cli.py --help
```

Linux normally also requires BlueZ. On Debian/Ubuntu: `sudo apt install bluez`.

## Connecting to a board

Normal application mode advertises as `nRFClaw(NDP)-XXXXXX`. Programming is intentionally physical: press the board programming button (P0.21 on the current reference design) to open the NUS programming window. It advertises as `nRFClaw(NUS)-XXXXXX`.

```bash
python3 nrfclaw_cli.py --device 1BF1D0 status
```

If the CLI reports that NUS was not found, press P0.21 and retry. `--device` is the six-hex-digit suffix shown in the BLE name.

## Compile without connecting

```bash
python3 nrfclaw_cli.py prompt --standalone \
  "Read the temperature every 30 seconds and send it over LoRa."

python3 nrfclaw_cli.py pseudo-help
python3 nrfclaw_cli.py examples
python3 nrfclaw_cli.py caps-help
```

## Upload and run

Compile a program to a binary file, then use the program commands exposed by `--help`. For an immediate test, `upload-run` uploads and starts a VM image. BOOT-scheduled programs are persisted and run again after reset.

```bash
python3 nrfclaw_cli.py --device 1BF1D0 upload-run program.bin
python3 nrfclaw_cli.py --device 1BF1D0 status
```

## Firmware upgrade over BLE

After the bootloader has been installed once with a hardware debugger, later application images can be upgraded over BLE:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 firmware-upgrade ../build/ninasense/nrfclaw_ninasense.bin
```

The CLI requests DFU through the physical NUS session, reconnects to `nRFClaw-DFU`, transfers the image with CRC checking, and the bootloader resets into the validated application.

See the repository root README and `docs/CLI.md` for architecture, security boundaries, schedules, BLE planes, examples, and diagnostics.
