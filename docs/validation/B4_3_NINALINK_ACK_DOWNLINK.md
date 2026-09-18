# B4.3 — NinaLink ACK / Downlink Turnaround

## Goal

Prove the first bidirectional low-power NinaLink exchange:

```text
Node                         Bridge
----                         ------
CAP_REPORT + ACK_REQ  -----> continuous RX
                             validate B3.1/B3.2
                             queue report
                             stop RX
                             wait 60 ms
short timed RX         <----- ACK(seq,status=OK)
validate ACK
sleep
                             resume continuous RX
```

The ACK is the first real NinaLink downlink. Generic application commands
(`CAP_SET`, `COMMAND`) remain a later gate; B4.3 establishes and validates the
radio turnaround and downlink transport needed by them.

## Node receive window

B4.3 adds a real LLCC68 timed RX API. The default gate uses 600 ms. The timed
API programs `SetRx(timeout_ms * 64)` because SX126x/LLCC68 timeout units are
15.625 us.

## Bridge ACK timing

The bridge waits 60 ms after receiving an ACK-requesting uplink before sending
the ACK. This gives the node time to complete TX_DONE, deferred cleanup and RX
turnaround. This is a conservative lab value, not the final optimized timing.

## CLI

```bash
python3 nrfclaw_cli.py --device C8BA09 ninalink-bridge-start
python3 nrfclaw_cli.py --device 1BF1D0 \
    ninalink-ack-test --window 600 --wait 3
```

Expected node result is `ACKED`, a 28-byte uplink and an 18-byte ACK. Bridge
status must increment both `valid` and `ACK sent` and return to continuous RX.

## Negative gate

Stop the bridge and repeat. The node must finish with `TIMEOUT` after the finite
RX window; it must not remain in continuous receive.

## Pass criteria

1. B3.1/B3.2 host tests pass.
2. ARM build passes.
3. B4.1/B4.2 behavior remains intact.
4. Bridge validates an ACK_REQ CAP_REPORT.
5. Bridge sends an 18-byte ACK after 60 ms.
6. Node validates matching ACK sequence/status.
7. Node reports ACK RSSI/SNR.
8. Bridge resumes continuous RX after ACK TX.
9. Three consecutive exchanges succeed.
10. Bridge-off negative test ends in TIMEOUT.
11. No VM/NDP/Application BLE regression.
