# Vendored dependencies

`nrf5sdk/` is reserved for the curated Nordic nRF5 SDK subset required to build nRFClaw firmware and bootloader. The GitHub release is intended to include that subset so developers do not need the complete SDK.

Before release, populate `vendor/nrf5sdk/` with all referenced sources/headers plus the S132 SoftDevice files and create `vendor/nrf5sdk/.imported`. Do not commit SDK examples/documentation/build artifacts that nRFClaw does not use. Verify the redistribution terms of every vendored third-party component and preserve required license/copyright notices.
