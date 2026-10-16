# B8.1k8c — SHT Family Driver + Pin Ownership

Adds SHT2x, SHT3x, SHT4x and SHTC3 family detection, Sensirion CRC-8,
diagnostic temperature/humidity conversion, default D11/D14 pin selection and
explicit alternate connector GPIO selection.

Ownership is bidirectional: active SHT pins cannot be stolen by Hall, Serial or
generic GPIO; active Hall/Serial and onboard DS18B20/LIS2DH12 resources prevent
SHT from claiming conflicting pins.

The LIS2DH12 hardware TWI1 implementation is untouched.

This revision intentionally does not modify the logical temperature provider,
NinaLink, Home Assistant, NDP or CLI. B8.1k8c1 will expose the driver through a
physical diagnostic command after the ARM build gate passes.
