# B8.1k7c2a — NetAtmo False-Sync Hardening / Anchor Corrective

B8.1k7c observed completed fixed 52-byte packets with LEN=0x55 and
non-repeatable IDs before the controlled CC1101 NetAtmo telegram arrived.

Valid NetAtmo LEN values used by this gate are 0x06, 0x19 and 0x31.
The diagnostic receiver now uses AA E7 12 instead of E7 12, using the final
AA preamble byte plus the protocol sync to reduce false locks.

Mandatory order: arm RX, transmit exactly one controlled frame, then poll.
Do not poll before transmitting.

Profile 27 remains catalog-only and HARDWARE_VALIDATED remains false.
