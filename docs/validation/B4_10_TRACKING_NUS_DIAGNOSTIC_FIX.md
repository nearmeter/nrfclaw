# B4.10 tracking / NUS diagnostic correction

## Why the previous Gate C looked inconsistent

`nrfclaw_tracking_start()` is asynchronous for autonomous identity mode.

A successful return means that the start request was accepted. The tracking
state machine then finishes durable identity/journal work and only later sets
the runtime `active` flag in `nrfclaw_tracking_process()`.

Separately, pressing P0.21 intentionally stops TRACKING before NUS becomes
available. Therefore a normal `tracking-info` or the old
`ninalink-node-app-status` implementation, when read through NUS, observed the
post-stop state and printed `Active: no`.

The tracking subsystem already preserves a pre-stop diagnostic snapshot through
`nrfclaw_tracking_debug()`. B4.10 now uses that latch for
`ninalink-node-app-status`.

No NinaLink wire format changes are introduced by this correction.

## What changed

`nrfclaw_ninalink_link_get_app_status()` now reports the tracking state from
`nrfclaw_tracking_debug()` instead of calling `nrfclaw_tracking_active()`
directly.

CLI output becomes:

    Tracking active (pre-NUS if latched): yes|no

## Important semantic rule

Do NOT change `apply_cap_set()` to require `nrfclaw_tracking_active()==true`
immediately after `nrfclaw_tracking_start()`.

That would incorrectly reject a valid asynchronous TRACKING start while its
Flash/journal/apply state machine is still progressing.

`NRFCLAW_NINALINK_APP_OK` means the TRACKING start request was accepted by the
native tracking subsystem. The eventual runtime-active state is observed
separately.

## Hardware re-gate

Flash the corrected firmware to the node.

1. Ensure bridge RX is active.
2. Queue `tracking_active=ON`.
3. Open one reliable node contact and verify the CAP_SET result is `OK`.
4. Leave the node autonomous for a few seconds so the TRACKING state machine can
   finish.
5. Press P0.21 once and read `ninalink-node-app-status`.

Expected:

    Result:          OK
    Tracking active (pre-NUS if latched): yes

For `OFF`, queue `tracking_active=OFF`, deliver it, then repeat the same check.
Expected latched state is `no`.

## Capability discovery note

Pressing P0.21 stops TRACKING before opening NUS. Therefore a CAPS discovery
performed only after P0.21 is pressed will correctly see TRACKING as not
ENABLED.

A future autonomous-contact gate can prove the live `ENABLED` bit entirely over
NinaLink without opening NUS between the write and the discovery. The B4.10
writable-metadata gate itself can use the pre-NUS latched status described
above.
