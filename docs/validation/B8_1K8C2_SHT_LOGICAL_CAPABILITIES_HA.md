# B8.1k8c2 — SHT Logical Capabilities + Home Assistant

Temperature priority is **DS18B20 -> SHT -> NRF52_INTERNAL**. Existing source IDs 1/2 stay unchanged and SHT is appended as source 3.

The existing semantic `HUMIDITY=0x0101` is activated when an SHT is present. Native CAP15 and NDP SENSOR_READ selector 7 are appended without renumbering CAP14 / selector 6.

On no-SHT boards the default D11/D14 software-I2C probe releases the bus immediately after NOT_FOUND, preserving connector ownership and low-power behavior.

Direct HA remains bootstrap-once plus passive advertising: pages 0..3 remain assigned and page 4 carries humidity in centi-percent on the normal sample cycle. NinaLink needs no wire-format change because 0x0101 already exists in the semantic registry.

Physical SHT validation remains pending until hardware is available.
