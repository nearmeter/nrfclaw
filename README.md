<p align="center">
  <img src="https://nrfclaw.cloud/assets/nrfclaw-icon.png" alt="nRFClaw" width="128">
</p>

# nRFClaw

### AI-programmable devices that sleep almost all the time.

🌐 Project: https://nrfclaw.cloud/

> In *Apollo 13*, the simulator team has to fit an essential power-up sequence inside a brutally small electrical budget. nRFClaw applies the same instinct to autonomous electronics: **wake only when useful work exists, do it quickly, then go back to sleep.**

**Describe the behavior. Compile it into deterministic bytecode. Send it over Bluetooth. Let a tiny battery-powered device run autonomously.**

nRFClaw is an open embedded platform for building AI-programmable, ultra-low-power autonomous devices around the Nordic **nRF52832**. Natural language is translated on the host into a validated program for a compact VM running on the MCU. The AI is not running on the battery-powered device and is not in the runtime control loop.

## Why nRFClaw?

- Natural language → Semantic IR → validated VM bytecode.
- Persistent `BOOT` programs or manually started programs.
- BLE programming through a physical-button protected NUS plane.
- BLE Beacon/Broadcaster and dynamic advertising payloads.
- Home Assistant Direct BLE/NDP integration plus NinaLink LoRa Bridge/Node transport.
- NinaLink low-power LoRa nodes with persistent Network ID isolation, automatic discovery and remote control.
- Optional 256-bit Application/NDP owner key for Home Assistant access.
- LoRa TX/RX with LLCC68.
- DS18B20 temperature and battery telemetry.
- LIS2DH12 motion, TAP, FALL, vibration and VIB_AUTO.
- Hall/reed/digital inputs and counting.
- Serial TX/RX, including a low-power Serial RX policy.
- RTC scheduling and persistent state.
- OpenHaystack-compatible tracking.
- BLE firmware upgrade through a compact DFU bootloader.
- Deterministic standalone compiler plus optional Gemini/OpenRouter/Ollama semantic agents.
- Designed around event-driven operation and microamp-class sleep currents.

## How It Works

A sentence such as:

```text
Read the temperature every 30 seconds and send it over LoRa.
```

is converted on the computer into deterministic pseudo-code:

```text
PROGRAM BOOT
LABEL LOOP
DS18 READ -> R0
FORMAT BUFFER "T=" + R0 AS CELSIUS
LORA SEND BUFFER
WAIT 30s
JMP LOOP
END
```

and then into compact VM bytecode. The device does not keep interpreting English and does not need an LLM, Internet connection or operating system.

```text
Natural language
      │
      ▼
Standalone semantic compiler ── or ── optional AI agent
      │
      ▼
Semantic IR / semantic plan
      │
      ▼
Schema + capability + semantic validation
      │
      ▼
Deterministic local lowering
      │
      ▼
VM bytecode
      │
      │ BLE/NUS programming
      ▼
┌──────────────────────── nRF52832 ───────────────────────┐
│                                                         │
│  VM wakes → reads sensor → updates BLE / sends LoRa     │
│     ▲                                      │            │
│     │                                      ▼            │
│ RTC / IRQ / event                      S L E E P        │
│     │                                      │            │
│     └──────────────────────────────────────┘            │
└─────────────────────────────────────────────────────────┘
```

`WAIT` is not a busy loop. The firmware returns to its low-power event loop whenever possible. The nRF52832 sleeps, the LLCC68 sleeps between transmissions, and event-capable sensors can wake the system by interrupt. This is why a device can periodically use LoRa or BLE without keeping those radios continuously active.

The important exception is **continuous reception**: LoRa RX and ordinary Serial RX must keep a receiver available and therefore belong to a higher-power class. Tracking/OpenHaystack, periodic LoRa TX, BLE advertising and interrupt-driven sensing are not inherently high-consumption modes.

## Hardware and Capabilities

The primary reference board is **NINASENSE**. Other nRF52832 boards can be supported through the board abstraction.

| Hardware | Capability |
|---|---|
| Nordic nRF52832 | MCU, BLE, VM, RTC, flash/state, internal die temperature fallback |
| LLCC68 | LoRa TX/RX |
| LIS2DH12 | Motion, TAP, FALL, vibration, VIB_AUTO |
| DS18B20 | Preferred external temperature source |
| Battery measurement circuit | Battery telemetry |
| Hall/reed inputs | Event, digital input and counting |
| 32.768 kHz crystal | Accurate low-power timing |
| UART pins | Serial TX/RX |
| P0.21 button | Physical programming/recovery access |

A board does not need every peripheral. Full capability requires the corresponding hardware to be fitted. For example, external ambient-grade temperature uses DS18B20, while the nRF52832 internal die sensor remains available as a fallback; vibration/FALL requires LIS2DH12, and LoRa requires LLCC68.

Current firmware capabilities include GPIO, battery, temperature, accelerometer, LoRa, BLE Application/NDP, RTC, Hall, persistent state, Serial, tracking and VIB_AUTO.

## Power Model

nRFClaw is optimized for short active windows:

```text
sleep → event → measure → transmit/update → sleep
```

On the NINASENSE reference design, suitable event-driven configurations have measured in the **single-digit to low-tens-of-microamps range** at rest. Actual battery life depends on sensor population, battery chemistry, radio power, intervals, environment and application behavior.

Typical qualitative classes:

| Behavior | Power character |
|---|---|
| Minimum-power test | Lowest baseline |
| Event-driven motion/Hall/VIB_AUTO | Excellent |
| Home Assistant Direct BLE/NDP | Excellent/Good; passive advertisements/events between short NDP sessions |
| NinaLink LoRa Node | Excellent/Good; Application/NDP is off and LLCC68 sleeps between contacts |
| Tracking/OpenHaystack | Excellent |
| Periodic LoRa TX | Excellent/Good depending on cadence and RF settings |
| BLE Beacon | Good |
| Low-power Serial RX policy | Good, but UART RX must remain active |
| NinaLink Bridge / continuous LoRa RX | Higher consumption; intended as the always-listening gateway |
| Continuous Serial RX | Higher consumption |
| Continuous LoRa RX | Higher consumption |

## Home Assistant installation

nRFClaw is distributed as a small Home Assistant integration package. **HACS is
not required.** No GitHub authentication or third-party repository is required.

### 1. Download the integration

<p align="left">
  <a href="https://github.com/nearmeter/nrfclaw/releases/latest/download/nrfclaw-home-assistant.zip">
    <img src="https://img.shields.io/badge/Download-nRFClaw_for_Home_Assistant-41BDF5?style=for-the-badge&logo=homeassistant&logoColor=white"
         alt="Download nRFClaw for Home Assistant">
  </a>
</p>

Direct URL:

```text
https://github.com/nearmeter/nrfclaw/releases/latest/download/nrfclaw-home-assistant.zip
```

The ZIP contains:

```text
custom_components/
└── nrfclaw/
    ├── __init__.py
    ├── manifest.json
    ├── config_flow.py
    ├── sensor.py
    └── ... remaining nRFClaw integration files
```

### 2. Copy the files into Home Assistant

Extract the ZIP and copy:

```text
custom_components/nrfclaw/
```

into the Home Assistant configuration directory so the final path is:

```text
/config/custom_components/nrfclaw/
```

If an older nRFClaw integration is already installed, replace the complete
`nrfclaw` directory instead of copying only changed files. This avoids
leaving obsolete modules from an older release.

For **Home Assistant OS**, the simplest file-copy method is the official
**Samba share** app included in the Home Assistant App store:

```text
Settings → Apps → Install app → Samba share
```

Configure a Samba username/password, enable the `config` share, and connect
from your computer:

```text
Windows:       \\<HOME_ASSISTANT_IP>\config
macOS / Linux: smb://<HOME_ASSISTANT_IP>/config
```

Copy the extracted `custom_components` directory into that `config` share.
No custom App repository and no HACS installation are needed.

Official Samba share reference:

```text
https://github.com/home-assistant/addons/tree/master/samba
```

The official **File editor** app is useful for inspecting the resulting files
from the Home Assistant UI:

```text
Settings → Apps → Install app → File editor
https://github.com/home-assistant/addons/tree/master/configurator
```

Developers who already have filesystem or SSH/SCP access may copy the same
directory by any normal file-transfer method.

### 3. Restart Home Assistant

```text
Settings → System → Restart Home Assistant
```

### 4. Discover the nRFClaw device

Power or reset an nRFClaw device configured as **Direct** or **NinaLink
Bridge**, then open:

```text
Settings → Devices & services
```

Home Assistant Bluetooth discovery should offer the device automatically. If
Direct NDP authentication is enabled, enter the 256-bit **NDP access key** in
the discovered-device setup form.

A low-power **NinaLink Node** does not connect directly to Home Assistant. The
Bridge receives the Node over LoRa and exposes the Node and its entities to
Home Assistant.

For maintainers, the release package is generated directly from the single
canonical source tree:

```text
integrations/home-assistant/custom_components/nrfclaw/
```

Build it locally with:

```bash
python3 tools/build_home_assistant_package.py
```

## Quick Start

### 1. Clone

```bash
git clone https://github.com/nearmeter/nrfclaw.git
cd nrfclaw
```

### 2. Install build tools and the validated ARM toolchain

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install make python3 python3-pip python3-venv bluez
```

nRFClaw is currently built with the **GNU Arm Embedded Toolchain
5-2016-q3-update** (`gcc-arm-none-eabi-5_4-2016q3`). Download the appropriate
package for your operating system from the official release page:

https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q3-update

For Linux, the release provides:

```text
gcc-arm-none-eabi-5_4-2016q3-20160926-linux.tar.bz2
```

Extract the toolchain to a directory of your choice. For example:

```bash
sudo tar -xjf gcc-arm-none-eabi-5_4-2016q3-20160926-linux.tar.bz2 -C /usr/local
```

This example results in a compiler path similar to:

```text
/usr/local/gcc-arm-none-eabi-5_4-2016q3/bin/arm-none-eabi-gcc
```

Verify the downloaded toolchain directly:

```bash
/usr/local/gcc-arm-none-eabi-5_4-2016q3/bin/arm-none-eabi-gcc --version
```

Install Nordic/J-Link command-line tools so this also works:

```bash
nrfjprog --version
```

### 3. Nordic SDK subset

The public repository is designed to contain only the nRF5 SDK17 files actually required by nRFClaw under:

```text
vendor/nrf5sdk/
```

This avoids requiring a full SDK download. Nordic files keep their original licenses/notices; they are not relicensed as Apache-2.0.

### 4. Configure and build the firmware

Configure the project with the NINASENSE board, the vendored nRF5 SDK subset
and the downloaded ARM GNU Toolchain:

```bash
make configure \
    BOARD=ninasense \
    NRF5SDK=vendor/nrf5sdk \
    TOOLCHAIN_PATH=/usr/local/gcc-arm-none-eabi-5_4-2016q3/bin
```

`TOOLCHAIN_PATH` must point to the toolchain's `bin` directory — the directory
that contains `arm-none-eabi-gcc`. If you extracted the toolchain somewhere
else, replace the path above accordingly.

The configuration is stored locally, so subsequent builds can use:

```bash
make clean
make -j$(nproc)
```

Build products are written below `build/ninasense/`.

### 5. Build bootloader

```bash
make bootloader BOARD=ninasense
```

### 6. First flash over SWD/J-Link

```bash
make erase BOARD=ninasense
make flash_softdevice BOARD=ninasense
make install_bootloader BOARD=ninasense
make flash BOARD=ninasense
make reset
```

After the bootloader is installed, normal firmware updates can be performed over BLE.

## Resource Usage

The firmware and bootloader are deliberately small compared with an OS-based edge stack. The release README should always report values produced by the exact public toolchain/vendor tree rather than historical development numbers.

```text
Application
  Flash: run `make size` on the release build
  RAM:   run `make size` on the release build

Bootloader
  Flash region: 0x0007A000 .. 0x0007EFFF (20 KiB reserved)
  Metadata:     0x0007F000
  Flash used:   run arm-none-eabi-size on the release bootloader ELF
  RAM used:     run arm-none-eabi-size on the release bootloader ELF
```

The bootloader contains only the BLE/DFU functionality needed to validate and install an application image. It does not carry the VM, LoRa application runtime, sensor stack or semantic compiler, keeping both flash and RAM requirements small.

## Install the CLI

```bash
cd NRFCLAW_CLI
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
python3 nrfclaw_cli.py --help
```

### Find devices

```bash
python3 nrfclaw_cli.py ha scan
```

Normal Application/NDP devices appear as, for example:

```text
nRFClaw(NDP)-1BF1D0
```

### Enter programming mode

Press **P0.21**. The board exposes the physical programming NUS plane:

```text
nRFClaw(NUS)-1BF1D0
```

Then:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 status
```

If NUS is no longer visible, press P0.21 again and retry.

## Program With Natural Language

Preview locally, without uploading:

```bash
python3 nrfclaw_cli.py prompt --standalone \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

To persist it directly as a BOOT program, press P0.21 first and run:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --standalone --boot --upload \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

Validated lowering:

```text
PROGRAM BOOT
LABEL LOOP
DS18 READ -> R0
BAT READ -> R1
FORMAT BUFFER "T=" + R0 AS CELSIUS + ",B=" + R1 AS VOLTS
LORA SEND BUFFER
WAIT 600s
JMP LOOP
END
```

## Optional AI Agent

The agent helps interpret less rigid natural language, but it does **not** get permission to invent trusted bytecode. Its output still crosses the local Semantic IR/schema/capability validation boundary before deterministic lowering.

Configure OpenRouter:

```bash
export OPENROUTER_API_KEY="..."
python3 nrfclaw_cli.py agent-set openrouter \
  --model google/gemini-2.5-flash-lite
python3 nrfclaw_cli.py agent-status
python3 nrfclaw_cli.py agent-test
```

Or Gemini:

```bash
export GEMINI_API_KEY="..."
python3 nrfclaw_cli.py agent-set gemini --model gemini-2.0-flash-lite
```

Or a local Ollama model:

```bash
python3 nrfclaw_cli.py agent-set ollama --model qwen2.5:3b
```

Use the configured agent:

```bash
python3 nrfclaw_cli.py prompt --agent \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

Compile with the agent and write the validated program directly to a board as persistent BOOT bytecode:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --agent --boot --upload \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

For a MANUAL program, `--run` starts it immediately after upload:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --agent --upload --run \
  "Read the temperature every 30 seconds and send it over LoRa."
```

Disable agent use with:

```bash
python3 nrfclaw_cli.py agent-off
```

## Minimum-Power Board Test

The following sentence is a certified standalone minimum-power program:

```text
At boot, disable everything to test minimum power consumption.
```

Install it:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --standalone --boot --upload \
  "At boot, disable everything to test minimum power consumption."
```

It lowers to:

```text
PROGRAM BOOT
SYSTEM MINIMUM POWER
END
```

The firmware turns off/relinquishes unrelated runtime peripherals while preserving the P0.21 recovery/programming path. Reset the board and **disconnect BLE/NUS before measuring**.

### Measuring current on NINASENSE

NINASENSE provides two current-test pins beside the current-test switch/header area (next to U7/SW1). The ammeter is inserted **in series** across these two pins; do not place an ammeter directly across the battery as if measuring voltage.

1. Power the board normally and install the minimum-power BOOT program.
2. Reset and allow the board to enter the minimum-power state.
3. Open/remove the current-test link so current must pass through the two measurement pins.
4. Put a multimeter in **current** mode and bridge the two current-test pins with the meter leads, so the meter becomes the supply path.
5. Start on a mA range if the meter is not autoranging. Once startup capacitors are charged, move to µA range if appropriate.
6. If a µA-only range shows `OL` during initial capacitor charging and remains latched, wait a few seconds, briefly remove the probes, then reconnect them while the capacitors are still charged.

Do not confuse a current measurement with a voltage measurement: the current test points are intended for a series ammeter.

## Tracking / OpenHaystack

Tracking can be programmed in one sentence. This exact example is validated by the standalone compiler:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --standalone --boot --upload \
  "At boot, enable tracking every 5 seconds."
```

It produces a 5000 ms tracking advertising interval and persists as a BOOT program.

### Provisioning sequence

1. Press P0.21 and upload the BOOT tracking program above.
2. **Reset the device.** Let the persisted BOOT program initialize tracking.
3. Only then press **P0.21 again** to enter the physical NUS programming plane.
4. Generate/display the provisioning QR:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 tracking-provision \
  --name "My nRFClaw"
```

The CLI displays a terminal QR by default. It can also create a PNG:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 tracking-provision \
  --name "My nRFClaw" --qr-png nrfclaw-tracker.png
```

Inspect the identity:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 tracking-identity
python3 nrfclaw_cli.py --device 1BF1D0 tracking-info
```

The explicit reset-before-provisioning sequence matters: the BOOT program first establishes the tracking runtime/identity state; P0.21 then deliberately takes BLE ownership back for secure physical provisioning.

## VIB_AUTO: Let the Device Learn the Machine

VIB_AUTO is intended for machines whose normal vibration is not a single fixed number. During discovery, the LIS2DH12 captures sparse vibration windows and nRFClaw clusters recurring operating patterns into local profiles.

A washing machine is a useful mental model. It may repeatedly exhibit several normal vibration regimes — gentle washing, agitation, draining-related vibration and high-speed spin. nRFClaw does **not** assign human labels such as "wash" or "spin" automatically; instead it stores up to six learned vibration profiles and later asks: **does the current window resemble one of the normal learned profiles?**

Each profile stores statistics including:

```text
RMS acceleration mean + tolerance
Peak acceleration mean + tolerance
Peak-to-peak mean + tolerance
Zero-crossing frequency mean + tolerance
Observation count
Confidence
```

Profiles are persisted, so the device does not have to relearn them after every reset. Very normal samples can slowly age/adapt a profile, while anomalous windows are kept out of learning so they do not teach the system that a fault is normal.

### Two-hour learning example with one-minute installation delay

This exact prompt is validated by the standalone compiler:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --standalone --boot --upload \
  "At boot, activate VIB_AUTO on the machine with learning 2 hours and initial delay 1 minute; if vibration becomes abnormal, notify Home Assistant."
```

Validated behavior:

```text
PROGRAM BOOT
BLE NDP ON
VIB_AUTO CONFIG learning=7200s initial=60s
VIB_AUTO START
LABEL vib_alarm_loop
WAIT_EVENT VIB_AUTO.ALARM
HA EVENT cap=13 op=4 value=R7
JMP vib_alarm_loop
END
```

The one-minute initial/arming delay lets you install/fix the sensor to the machine without those handling vibrations entering the learning set. The following two hours are the discovery window. After learning, normal profiles are monitored locally; repeated abnormal behavior can raise `VIB_AUTO.ALARM`, which this program forwards to Home Assistant.

Inspect the learner:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 vib-auto-status
python3 nrfclaw_cli.py --device 1BF1D0 vib-auto-baseline
python3 nrfclaw_cli.py --device 1BF1D0 vib-auto-diagnostics
```

Configure it directly without natural language when desired:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 vib-auto-config \
  --learning-time 2h --arming-delay 60
python3 nrfclaw_cli.py --device 1BF1D0 vib-auto-start --relearn
```

`vib-auto-baseline` prints every learned profile with confidence, observations, RMS, peak, peak-to-peak and zero-crossing values/tolerances.

## Home Assistant: Direct BLE and NinaLink

nRFClaw can reach Home Assistant in two low-power ways: directly over BLE/NDP, or through **NinaLink**, where remote battery-powered nodes use LoRa and a bridge exposes them to Home Assistant over BLE/NDP.

```text
Direct BLE
──────────
NINASENSE ── BLE advertisements / short NDP sessions ──► Home Assistant

NinaLink
────────
Battery Node ── duty-cycled LoRa ──► Bridge ── BLE/NDP ──► Home Assistant
                  sleeps between        │
                  contact windows       └─ continuous LoRa RX
```

The roles are persistent and mutually exclusive:

| Role | Application/NDP on that board | LoRa behavior | Power intent |
|---|---|---|---|
| **Direct BLE** | Enabled | LoRa not required for HA | Low-power BLE advertising/events; NDP opens only when needed |
| **NinaLink Node** | **Off** | Periodic/event-driven TX plus bounded receive windows | Battery node; LLCC68 sleeps between contacts |
| **NinaLink Bridge** | Enabled | Continuous NinaLink RX | Gateway role; higher consumption than a node |

The default NinaLink node contact/report period is **20 seconds**. It can be changed in the natural-language role prompt. Shorter intervals improve control/report latency but increase radio duty cycle.

### Direct Home Assistant

Enable the Direct BLE role as a persistent BOOT configuration:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  prompt --standalone --upload --boot \
  "At boot, enable Home Assistant."
```

The device advertises its Direct role and Home Assistant can create the corresponding NDP device/entities. Passive telemetry and events are advertisement-driven where possible, so the board does not need to maintain a continuous BLE connection.

### Create a NinaLink bridge and network

Press **P0.21** on the board that will become the bridge, then run:

```bash
python3 nrfclaw_cli.py --device C8BA09 \
  prompt --standalone --upload --boot \
  "Enable Home Assistant in bridge mode."
```

If the bridge has no NinaLink network yet, the CLI creates and persists a random non-zero 16-bit Network ID. If one already exists, it is reused. The upload prints it, for example:

```text
=== PROMPT NINALINK PROVISION B7.6f2l2 ===
Role:             BRIDGE
Network ID:       0x7A31
Action:           created
```

Copy that Network ID. It can be checked again while P0.21/NUS is open:

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-network-status
```

`0x0000` is the legacy/unprovisioned value. Prompt-based provisioning uses `0x0001..0xFFFF`.

### Add a low-power NinaLink node to the same network

Press **P0.21** on the node and use the Network ID copied from the bridge:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  prompt --standalone --upload --boot \
  "Enable Home Assistant in LoRa mode on network 0x7A31."
```

This first persists `0x7A31`, then stores the `NINALINK_NODE` BOOT program. After reset the node keeps Application/NDP BLE off, wakes for NinaLink contact/report windows, exchanges ACK/downlink traffic with the bridge, then returns the LLCC68 and MCU to the low-power path.

Once a node already has the correct Network ID, the shorter form reuses it:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  prompt --standalone --upload --boot \
  "Enable Home Assistant in LoRa mode."
```

For an unprovisioned node this shorter form deliberately fails instead of silently creating a different network.

Network provisioning is a **host-side action of `prompt --upload`**. A raw VM `.bin` contains the role bytecode but does not contain the Network ID provisioning transaction.

### Protect Home Assistant NDP with an owner key

NDP protection is optional. To enable it, press **P0.21** so the physical NUS owner plane is available, then generate a 256-bit key:

```bash
python3 nrfclaw_cli.py --device C8BA09 ndp-key-generate
```

Example output:

```text
NDP protection:   enabled
NDP key:          hex:0123456789abcdef...64 hexadecimal digits total...
Store this key securely; Application/HA now requires it.
```

Copy the complete `hex:<64-hex-digits>` value into **NDP access key** when adding the Direct device or NinaLink Bridge in Home Assistant. The key can be read again only through the physical P0.21/NUS owner path:

```bash
python3 nrfclaw_cli.py --device C8BA09 ndp-key-get
```

The **NDP key and NinaLink Network ID are different things**: the NDP key authenticates Home Assistant/Application access to the Direct device or bridge, while the Network ID isolates which LoRa nodes belong to a NinaLink network.

### Natural-language Home Assistant behaviors

These English forms are canonical examples used by the CLI/agent knowledge base:

```text
Every HALL1 pulse represents 1 liter. Accumulate the total consumption and display it in Home Assistant.

At boot, enable Home Assistant and notify via Telegram through HA when the electric mixer starts running.

At boot, enable Home Assistant and notify when the washing machine stops running.

At boot, enable Home Assistant and notify when the pump vibration becomes unusual.

At boot, enable Home Assistant and notify via Telegram through HA when the motor vibration becomes abnormal.
```

The vibration wording maps to semantic VIB_AUTO events:

| Natural wording | Device event |
|---|---|
| machine starts running | `VIB_AUTO.MACHINE_ON` |
| machine stops running | `VIB_AUTO.MACHINE_OFF` |
| vibration becomes unusual/strange | `VIB_AUTO.WARNING` |
| vibration becomes abnormal/anomalous | `VIB_AUTO.ALARM` |

Telegram is an **HA-side action**. The nRF52832 does not run Telegram, Wi-Fi or TLS; it publishes the semantic event to Home Assistant, and a Home Assistant automation performs the notification.

Discover visible Direct/Bridge devices with:

```bash
python3 nrfclaw_cli.py ha scan
```

Inspect one:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 ha device-info
python3 nrfclaw_cli.py --device 1BF1D0 ha sensors
```

## BLE Beacon: Name vs Dynamic Payload

A named beacon:

```text
BLE NAME "VIB-ALARM"
```

is different from a dynamic payload:

```text
FORMAT BUFFER "T=" + R0 AS CELSIUS
BLE ADV BUFFER
```

`BLE ADV BUFFER` updates Advertising Data; the scanner-visible device name does not necessarily become the sensor value.

## LoRa

Inspect the persisted LLCC68 profile:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 lora-get
```

Periodic LoRa TX wakes the radio for transmission and returns it to sleep after TX completion. Continuous LoRa RX is a fundamentally different, higher-power mode because the receiver must remain listening.

## Write, Edit, Compile and Validate Pseudo-code

Natural language is convenient, but nRFClaw also exposes its deterministic pseudo-code directly. This is useful when developing, debugging or reviewing an exact VM program without an AI agent.

Show the complete grammar supported by the installed CLI:

```bash
python3 nrfclaw_cli.py pseudo-help
```

Create a source file, for example `temperature_lora.nrf`:

```text
PROGRAM BOOT
LABEL LOOP
TEMP READ -> R0
BAT READ -> R1
FORMAT BUFFER "T=" + R0 AS CELSIUS + " B=" + R1 AS VOLTS
LORA SEND BUFFER
WAIT 600s
JMP LOOP
END
```

Edit it with any text editor:

```bash
nano temperature_lora.nrf
```

Compile and validate the pseudo-code locally:

```bash
python3 nrfclaw_cli.py compile --file temperature_lora.nrf
```

The compiler parses every statement, validates the program and prints the deterministic representation, generated bytecode and schedule. Invalid syntax or unsupported operations are rejected instead of being silently ignored.

To force BOOT scheduling while compiling:

```bash
python3 nrfclaw_cli.py compile --boot --file temperature_lora.nrf
```

To compile and save the raw VM bytecode as a `.bin` file:

```bash
python3 nrfclaw_cli.py compile --boot \
  --file temperature_lora.nrf \
  --output temperature_lora.bin
```

A successful compilation prints the generated bytecode and schedule. The `.bin` contains VM bytecode, not an nRF52832 firmware image; it is the small program executed by the nRFClaw VM.

You can therefore use a simple edit/validate cycle:

```text
temperature_lora.nrf
        │
        │ edit
        ▼
 nrfclaw_cli.py compile
        │
        ├── syntax/semantic error ──► edit again
        │
        ▼
  validated bytecode
        │
        ▼
temperature_lora.bin
```

### Upload a compiled `.bin` VM program

First press **P0.21** so the device enters the `nRFClaw(NUS)-XXXXXX` programming plane.

Upload a MANUAL program:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  upload temperature_lora.bin
```

Upload it and execute it immediately:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  upload-run temperature_lora.bin
```

For an autonomous program that must start after reset, persist the `.bin` with a BOOT schedule:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  upload temperature_lora.bin --boot
```

Then reset the device. The firmware scheduler loads the persisted bytecode and starts the VM automatically.

> **Important:** `--boot` on `upload` controls the schedule stored with the raw `.bin`. Use it when the program must run autonomously after reset. `upload-run` is intended for immediate execution of a loaded program.

### Generate `.bin` directly from a natural-language prompt

The same output can be produced without manually writing pseudo-code:

```bash
python3 nrfclaw_cli.py prompt --standalone --boot \
  --output temperature_lora.bin \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

Or compile, persist and install directly on the board without keeping an intermediate `.bin`:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 prompt --agent --boot --upload \
  "Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously."
```

This gives developers three equivalent workflows: natural language → board, natural language → `.bin` → board, or hand-written pseudo-code → `.bin` → board.

## BLE Firmware Upgrade

After the bootloader has been installed once by SWD:

```bash
python3 nrfclaw_cli.py --device 1BF1D0 \
  firmware-upgrade ../build/ninasense/nrfclaw_ninasense.bin
```

The application requests DFU, the compact bootloader advertises the DFU service, receives the image using the conservative BLE transfer protocol, validates it and returns to the application.

## Useful CLI Commands

```bash
python3 nrfclaw_cli.py examples
python3 nrfclaw_cli.py caps-help
python3 nrfclaw_cli.py pseudo-help
python3 nrfclaw_cli.py semantic-status
python3 nrfclaw_cli.py semantic-ir-schema
python3 nrfclaw_cli.py ask "How does VIB_AUTO work?"
```

## AI Knowledge Base

`docs/NRFCLAW_AI_KNOWLEDGE_BASE.md` is a self-contained technical context file intended for ChatGPT, Claude, Gemini, local RAG systems and other assistants. Give that file to the model and tell it to treat the KB as the authoritative nRFClaw project reference.

Example:

```text
Use NRFCLAW_AI_KNOWLEDGE_BASE.md as the authoritative technical reference.
How do I build a node that wakes on motion, reads a DS18B20 and sends LoRa?
```

The AI can explain and propose behavior; executable programs must still pass nRFClaw's local semantic/compiler validation.

## Documentation

```text
docs/ARCHITECTURE.md
docs/FIRMWARE.md
docs/BOOTLOADER.md
docs/CLI.md
docs/PROTOCOLS.md
docs/PORTING.md
docs/NRFCLAW_AI_KNOWLEDGE_BASE.md
```

## License

Original nRFClaw source code is released under the **Apache License 2.0**. Apache-2.0 is a strong fit for an embedded/open-hardware ecosystem because it is permissive while also providing an explicit patent grant from contributors.

Third-party files — particularly the curated Nordic nRF5 SDK subset in `vendor/` — retain their own copyright and license terms. They are **not** converted to Apache-2.0 by being present in this repository. Preserve all Nordic headers/notices and see `THIRD_PARTY_NOTICES.md`.

---


🏢 Published by NearMeter: https://github.com/nearmeter
✉️ Contact: nrfclaw@nearmeter.com


<p align="center"><strong>nRFClaw — Write behavior, not firmware.</strong></p>
<p align="center">https://nrfclaw.cloud/ · nrfclaw@nearmeter.com</p>
