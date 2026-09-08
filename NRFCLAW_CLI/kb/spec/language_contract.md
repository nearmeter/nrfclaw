# nRFClaw Semantic Language Specification v1

## Purpose
This package is the portable, provider-independent semantic contract for translating natural-language nRFClaw requests into Semantic IR v1. It is intended to be supplied to GPT, Claude, Gemini, local models, or future providers without fine-tuning.

## Pipeline
Natural language -> provider LLM -> Semantic IR v1 -> strict local validator -> semantic oracle -> deterministic lowering -> VM bytecode.

The provider is NOT trusted to emit bytecode. It emits only Semantic IR. Semantically equivalent IR is acceptable; exact register allocation or action layout need not match another provider.

## Hard rules
1. Output JSON only, conforming to the root contract.
2. Numeric fields are JSON numbers, never quoted strings.
3. program.mode is BOOT only for explicit boot/startup/reset intent; otherwise MANUAL.
4. register.compare supports LT, GT, EQ only and writes 1=true, 0=false. jump_if_zero jumps only on 0.
5. register.add/sub operands a and b are register IDs, not immediate values. Load constants with register.set.
6. Omitted LoRa RF parameters mean persisted/current profile; do not fabricate lora.config or ambiguity.
7. Omitted BLE advertising interval uses runtime/default cadence; do not fabricate ambiguity.
8. CR4/5=>cr=1, CR4/6=>2, CR4/7=>3, CR4/8=>4.
9. accel.config requires mode only. Defaults: odr=10 Hz, full-scale=2 g, threshold=120 mg, duration=100 ms, low_power=true. Explicit values override defaults.
10. vib_auto.config is different: learning_s and initial_s MUST be explicitly supplied by the user; otherwise AMBIGUOUS.
11. Literal BLE payload: buffer.set_literal + ble.advertise_buffer. Dynamic values: buffer.format_register / buffer.format_2reg.
12. Continuous relays/monitoring must contain a real CFG cycle. loop has an implicit back-edge. Labels/jumps may also express cycles.
13. Preserve user literals and numeric values exactly.
14. Return MISSING_CAPABILITY only when the requested behavior cannot be expressed with the supplied primitive catalog and compositions.
15. Never expose or depend on a Gold IR. Validation is behavioral/semantic.
