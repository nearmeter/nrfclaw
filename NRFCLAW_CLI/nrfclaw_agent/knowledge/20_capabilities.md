# Current compiler capabilities

TRACKING: can start autonomous BLE tracking. Default interval is 1000 ms and current deterministic text compiler supports whole-second intervals from 1000..10000 ms. Current exposed TX power is +4 dBm. TRACKING_START takes BLE advertising ownership and disables adaptive NDP advertising.

MOTION: low-power LIS2DH12 high-pass interrupt wake. The current text compiler exposes threshold 250 mg and duration 100 ms. It can wait for a motion event, then continue the program. Do not interpret a request for continuous motion duration (for example "moving for more than 30 seconds") as a single motion event; that semantic is unsupported.

TEMPERATURE: DS18B20 read can be one-shot or periodic. Conditions supported are lower-than and greater-than thresholds in degrees Celsius. A phrase such as "read temperature" without "every N seconds" is one-shot. Never steal a tracking cadence and apply it to temperature.

BEACON: non-connectable broadcaster with local name, interval, optional boot wait and TX power. Name maximum is 24 UTF-8 bytes.

NDP: can be explicitly disabled before the requested behavior. Do not infer NDP OFF merely because tracking eventually starts; use ndp_off only when explicitly requested.
