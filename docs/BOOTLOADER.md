# BLE bootloader

The nRFClaw bootloader provides application DFU after one factory installation over SWD/J-Link.

## Memory and boot

The current bootloader linker location is `0x0007A000`; the factory target writes this to UICR `BOOTLOADERADDR` (`0x10001014`). Application validity is checked before jumping to the application. The bootloader restores the expected SoftDevice vector-table behavior before application start.

## Entry

The application can request DFU and reset. The bootloader remains active when the DFU GPREGRET marker is present; otherwise a valid application is started. During DFU it advertises a NUS-compatible `nRFClaw-DFU` service.

## Transport

Commands include INFO, BEGIN, DATA, END, ABORT and BOOT. BEGIN supplies image size and expected CRC. DATA is stop-and-wait and offset checked. END validates CRC, commits the protected first page/metadata and resets. The implementation deliberately splits flash writes into bounded word operations.

The current transport uses conservative ATT/NUS behavior rather than assuming a large negotiated MTU. This was retained for robust macOS/CoreBluetooth operation.

## Factory installation

```bash
make bootloader BOARD=ninasense
make install_bootloader BOARD=ninasense
```

This is a one-time/debugger operation. Subsequent application updates can use:

```bash
cd NRFCLAW_CLI
python3 nrfclaw_cli.py --device XXXXXX firmware-upgrade ../build/ninasense/nrfclaw_ninasense.bin
```

Do not move the bootloader address or application/metadata boundaries without reviewing all linker scripts, DFU constants and UICR programming together.
