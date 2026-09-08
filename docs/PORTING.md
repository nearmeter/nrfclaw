# Porting nRFClaw to another nRF52832 board

Start from the closest existing directory under `boards/`. Create a new board directory containing `board.mk`, `board_config.h` and a linker script. Keep hardware differences in the board layer instead of scattering board-specific `#ifdef`s through subsystem code.

Review at minimum: programming button/P0.21 equivalent, LEDs if any, battery ADC path, Hall/digital inputs, LIS2DH12 wiring, DS18B20 pin, LLCC68 SPI/control pins, serial pins, LFCLK source, DCDC policy, flash layout and antenna/radio constraints.

Build with:

```bash
make BOARD=myboard
```

Before declaring a port usable, validate: boot/current baseline, physical programming entry, NUS upload/run, reset into NDP, BOOT program persistence, BLE advertiser takeover, LoRa TX/sleep, any RX mode, battery reading, configured sensors and bootloader entry/DFU.
