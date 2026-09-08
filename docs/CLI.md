# CLI and compiler knowledge base

`NRFCLAW_CLI/nrfclaw_cli.py` is the reference host interface. It combines BLE device management with the deterministic semantic compiler.

## Runtime dependency

```bash
python3 -m pip install -r NRFCLAW_CLI/requirements.txt
```

The BLE dependency is `bleak`. Semantic compilation itself is local and can be previewed without a device.

## Device selection

The same board address can expose different logical BLE planes. Do not select a device from MAC suffix alone: programming discovery expects the NUS identity/service. Press P0.21, then use `--device XXXXXX`.

## Useful commands

```bash
python3 nrfclaw_cli.py --help
python3 nrfclaw_cli.py --device XXXXXX status
python3 nrfclaw_cli.py --device XXXXXX board
python3 nrfclaw_cli.py --device XXXXXX lora-get
python3 nrfclaw_cli.py --device XXXXXX hall-status
python3 nrfclaw_cli.py prompt --standalone "..."
python3 nrfclaw_cli.py compile program.nrfclaw
python3 nrfclaw_cli.py semantic-ir-schema
python3 nrfclaw_cli.py examples
python3 nrfclaw_cli.py caps-help
```

Run the command-specific `--help` before destructive or provisioning operations.

## Compiler pipeline

The current host supports canonical English-first semantics while accepting supported normalized input forms. The important boundary is:

```text
text -> normalization/action parsing -> Semantic IR/capability checks
     -> deterministic lowerer -> pseudo/IR -> VM bytecode
```

Agent-backed interpretation is optional. The standalone path is the reproducible/certifiable path. An agent must never be treated as an authority that can directly invent VM opcodes.

## Semantic certification rule

A successful compile is not sufficient for a composite example. Validation must check that the generated IR contains every required primitive. For example, a FALL -> temperature+battery -> LoRa+BLE recipe is only valid if the IR actually contains FALL wait, both sensor reads, LoRa buffer TX and BLE buffer advertising.

## BOOT semantics

When persistence is requested, confirm that the result is BOOT. Uploading a MANUAL program and enabling a UI persistence option elsewhere must not be assumed to change its schedule unless the compiler/upload protocol actually sets BOOT.

## BLE dynamic payloads

`BLE ADV BUFFER` updates advertising data from the VM buffer. It does not rename the BLE device. A scanner may continue showing a stable device identity while the manufacturer/service advertising payload changes.

## RF and power

LoRa TX uses the persisted RF profile unless explicitly overridden. Continuous LoRa RX is higher consumption. Serial low-power mode shuts down unrelated services but cannot turn off the UART receiver while it is expected to receive bytes.
