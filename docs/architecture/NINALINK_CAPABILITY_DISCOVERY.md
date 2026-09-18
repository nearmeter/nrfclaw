# NinaLink Capability Discovery + Writable Metadata

B4.10 activates the capability discovery ABI already frozen in B1.2/B3.2:
`CAPS_REQUEST=0x02` and `CAPS_RESPONSE=0x03`.

For the low-power ACK window B4.10 adds a linked CAPS_REQUEST form with a
3-byte payload: `reply_to_seq:u16 + page_index:u8`. The original one-byte
B3.2 CAPS_REQUEST remains unchanged for legacy/non-linked use.

CAPS_RESPONSE remains exactly the B3.2 wire format: a four-byte page header
plus up to five 9-byte descriptors. Five descriptors produce the maximum
64-byte NinaLink frame.

Only B2 capabilities whose runtime state contains `SUPPORTED` are advertised.
`PRESENT`, `ENABLED` and `FAULT` are preserved separately in
`runtime_state_flags`.

B4.10 also makes writable metadata authoritative. `TRACKING_ACTIVE (0x0401)`
is changed to `READABLE|REPORTABLE|WRITABLE|RETAINED`, and CAP_SET checks the
registry WRITABLE bit before applying a value.
