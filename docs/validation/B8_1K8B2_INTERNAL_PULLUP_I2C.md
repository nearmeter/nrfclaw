# B8.1k8b1 — External Low-Power I2C with Internal Pull-Ups

Default NINASENSE SHT pins:

- SHT_SCL = D11 / P0.11
- SHT_SDA = D14 / P0.14

The user may explicitly select another supported GPIO pair.

There are no external I2C pull-up resistors in the intended wiring. The SHT is
wired directly to the NINASENSE GPIO header, so the nRF52 internal GPIO pull-ups
provide the released/HIGH state.

The external bus is software/open-drain I2C:

- LOW: GPIO output LOW
- HIGH/released: GPIO input + internal PULLUP
- never actively drive HIGH

While an SHT bus is claimed, SCL/SDA remain input+pull-up between transactions,
so the bus never floats. On deinit/release the pins return to true high-Z:
INPUT_DISCONNECT + NOPULL + NOSENSE.

A conservative software clock is used to give the internal pull-ups extra rise
time margin on short wires.

The LIS2DH12 source and hardware TWI1 path are unchanged.

B8.1k8c will add SHT-family detection/conversion and pin ownership:
default D11/D14 conflicts with Hall, while explicit alternate pins are validated
against other runtime owners.
