# B8.1k6k3 — RF Virtual Node Control Cleanup

RF virtual nodes use the `0xF8ppssss` namespace. They represent external RF
devices observed by a NINASENSE bridge and are read-only from NinaLink's point
of view.

A W-MBus meter can legitimately publish semantic `VOLUME (0x0601)`. Previous
Home Assistant button logic interpreted any NinaLink device containing Volume
as a VM-backed retained accumulator and created `Reset accumulated total`
(COMMAND `0x0007`). That is invalid for RF virtual nodes.

B8.1k6k3:

- skips all NinaLink behavioral COMMAND buttons for `0xF8ppssss`
- removes stale already-registered RF COMMAND buttons from the HA entity
  registry on integration setup
- preserves Hall reset, VIB_AUTO relearn and accumulated-total reset for real
  NinaLink nodes
- does not change sensors, RF decoding, RFVM, firmware or NinaLink wire ABI

This is an HA-only presentation/control fix.
