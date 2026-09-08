# Third-Party Notices

nRFClaw original source code is licensed under Apache License 2.0; see `LICENSE`.

This repository may redistribute a curated subset of Nordic Semiconductor's nRF5 SDK under `vendor/nrf5sdk/`. Those files remain subject to their original Nordic Semiconductor copyright notices, license headers, redistribution conditions and other applicable terms. Inclusion in the nRFClaw repository does not relicense Nordic code under Apache-2.0.

When populating `vendor/nrf5sdk/`:

- copy only files required to build the supported targets;
- preserve original copyright and license headers verbatim;
- preserve SDK/component license and notice files applicable to the copied files;
- do not replace Nordic headers with the nRFClaw Apache header;
- review any additional third-party components contained inside the SDK subset and preserve their notices as required.

Before each public release, regenerate this notice if the contents of `vendor/` change.
