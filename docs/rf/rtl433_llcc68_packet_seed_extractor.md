# B8.1c1 — Band-Agnostic Exact-Frequency Packet Profile Seeds

B8.1c1 replaces the earlier B8.1c design that filtered protocol metadata
through the currently populated 900 MHz module.

The logical protocol and its RF variants are now separate.

Example:

```text
WH52
  protocol seed
    bitrate/framing/decoder metadata
    RF variants:
      433.920 MHz
      868.000 MHz
      915.000 MHz
```

The three frequencies are not concurrent runtime frequencies. They are three
possible exact RF variants of the same logical protocol.

At runtime nRFClaw selects exactly one variant:

```text
protocol
  ↓
RF variant
  ↓
exact center frequency
  ↓
LLCC68 remains there until reconfigured
```

There is no automatic frequency scan and no radio timesharing.

B8.1c1 extracts conservatively:

- logical protocol identity;
- one or more exact frequency variants already known by B8.1b2;
- nominal PCM bitrate hint;
- byte-array `bitbuffer_search()` anchors;
- leading AA/55 preamble prefix;
- sync-word candidate;
- byte-extraction length hints.

It intentionally does not invent:

- FSK deviation;
- RX bandwidth;
- shaping;
- packet mode;
- payload length policy;
- radio CRC/whitening mode.

Run:

```bash
python3 tools/extract_rtl433_llcc68_packet_seeds.py \
    --catalog build/b81a/rtl433_fsk_catalog.json \
    --compat build/b81b2/rtl433_fsk_compat.json \
    --source /tmp/rtl_433_ESP \
    --out-dir build/b81c1 \
    --show
```

Outputs:

- `rtl433_llcc68_packet_seeds.json`
- `rtl433_llcc68_packet_seeds.csv`
- `rtl433_llcc68_rf_variants.csv`
- `rtl433_llcc68_packet_seeds.txt`

By default only `PACKET_NATIVE_CANDIDATE` entries are emitted.
Use `--include-review` to include `PACKET_NATIVE_REVIEW`.

Hardware/module compatibility remains deferred to the later module-capability
gate. A 433 MHz protocol is not removed because the currently populated board
uses a 900 MHz radio, and vice versa.
