# nRFClaw Semantic KB v1

Portable model-independent knowledge base extracted from the R3.8.20c2g1c semantic baseline.

## Goal
Feed the same contract to GPT-5 Mini, Claude, Gemini, or a local LLM and compare them using the same local validator/oracle/lowering. The target is semantic equivalence, not byte-for-byte identical IR.

## Contents
- `spec/language_contract.md`: normative language rules.
- `spec/primitive_catalog.json`: primitive ABI, fields, defaults and semantics.
- `spec/semantic_ir_root.schema.json`: provider-facing root JSON contract.
- `spec/semantics_and_cfg.md`: control-flow/state semantics.
- `prompts/provider_system_prompt.txt`: provider-neutral system prompt.
- `conformance/complex20.json[l]`: T01-T20 provider-neutral conformance cases.
- `reference/`: c2g1c source snapshots for audit/traceability; not required by the provider.
- `MANIFEST.sha256`: integrity manifest.

## Provider comparison protocol
1. Give each provider exactly the same KB files and system prompt.
2. Give only the natural-language `prompt` from each conformance case.
3. Parse provider JSON without semantic coercion. Transport wrapper removal is allowed; semantic repair is not.
4. Run the same strict Semantic IR validator.
5. Run the same semantic oracle and deterministic lowering.
6. Score IR-valid, first-pass-valid, COMPILE/safe rejection/hard error, lowering, semantic smoke, latency, tokens, and cost.
7. A provider passes when behavior passes the oracle; do not compare exact register allocation or exact IR text.

## Current intentional ambiguity
T11 VIB_AUTO asks for configuration but omits `learning_s` and `initial_s`; the correct safe result is AMBIGUOUS. This is not a model failure.
