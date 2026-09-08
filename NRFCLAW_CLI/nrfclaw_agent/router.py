#!/usr/bin/env python3
"""R3.8.16b1 agent router.

Default UX is AUTO: prefer a configured/reachable external/local HTTP agent,
then fall back to the standalone semantic engine.  The agent never emits VM
bytecode; it emits nRFClaw pseudo-code or a structured answer.  The local
pseudo compiler remains the only bytecode generator.
"""
from __future__ import annotations
import hashlib, json, re, urllib.request, time
from pathlib import Path
from typing import Any

from .core import AgentConfig, AgentError, load_config, call_provider, provider_catalog, last_provider_meta
from nrfclaw_pseudo import GRAMMAR, compile_pseudo
from nrfclaw_semantic_ir import schema_contract, validate_ir, compile_ir, SemanticIRError, SemanticIRMissingCapability
from nrfclaw_schedule_semantics import detect_schedule_intent, validate_schedule_clause
from nrfclaw_semantic_grounding import grounding_bundle

TOOLS = Path(__file__).resolve().parents[1]
PRODUCT_KB = TOOLS / 'nrfclaw_knowledge.json'
CAPS_KB = TOOLS / 'nrfclaw_semantic_capabilities.json'
EXAMPLES_KB = TOOLS / 'nrfclaw_semantic_examples.json'
KNOWLEDGE_DIR = Path(__file__).resolve().parent / 'knowledge'


def _read(path: Path) -> str:
    try: return path.read_text(encoding='utf-8')
    except Exception: return ''


def knowledge_bundle() -> str:
    parts=[]
    for p in (PRODUCT_KB, CAPS_KB, EXAMPLES_KB):
        if p.exists(): parts.append(f'===== {p.name} =====\n{_read(p)}')
    for p in sorted(KNOWLEDGE_DIR.glob('*.md')):
        if p.name in ('00_contract.md','10_schema.md'):
            continue
        parts.append(f'===== {p.name} =====\n{_read(p)}')
    parts.append('===== PSEUDO_GRAMMAR =====\n'+GRAMMAR)
    return '\n\n'.join(parts)


def knowledge_sha256() -> str:
    return hashlib.sha256(knowledge_bundle().encode('utf-8')).hexdigest()


def _health(cfg: AgentConfig, timeout: float=0.65) -> bool:
    try:
        if cfg.provider == 'ollama':
            with urllib.request.urlopen(cfg.base_url.rstrip('/') + '/api/tags', timeout=timeout) as r:
                return 200 <= getattr(r,'status',200) < 300
        # For remote providers, configuration + key is a sufficient preflight;
        # the actual request returns a precise error if unreachable.
        import os
        if cfg.api_key_env and not os.getenv(cfg.api_key_env): return False
        return bool(cfg.base_url and cfg.model)
    except Exception:
        return False


def external_available(cfg: AgentConfig|None=None) -> bool:
    cfg=cfg or load_config()
    if not cfg.enabled:
        return False
    return _health(cfg)


def _extract_json(text: str, allow_pseudo: bool=False) -> dict[str, Any]:
    s=text.strip()
    if s.startswith('```'):
        s=re.sub(r'^```(?:json)?\s*','',s); s=re.sub(r'\s*```$','',s)
    try:
        obj=json.loads(s)
    except Exception:
        a,b=s.find('{'),s.rfind('}')
        if a>=0 and b>a:
            try: obj=json.loads(s[a:b+1])
            except Exception: obj=None
        else:
            obj=None
        # Compatibility path for routed/free models that ignore JSON mode but
        # correctly emit canonical nRFClaw pseudo-code. The pseudo compiler is
        # still the security boundary and rejects unknown statements.
        if obj is None and allow_pseudo:
            m=re.search(r'(?ims)^\s*PROGRAM\s+(?:BOOT|MANUAL)\b.*',s)
            if m:
                pseudo=m.group(0).strip()
                pseudo=re.sub(r'(?m)^```(?:text|pseudo|nrfclaw)?\s*$','',pseudo)
                pseudo=re.sub(r'(?m)^```\s*$','',pseudo).strip()
                return {
                    'schema_version':2,'status':'COMPILE','language':'other',
                    'pseudo_code':pseudo,'semantic_ast':{'source':'agent-pseudo-fallback'},
                    'unresolved_clauses':[],'missing_capabilities':[],
                    'notes':['Provider returned canonical pseudo-code instead of JSON; accepted after deterministic local validation.'],
                }
        if obj is None: raise AgentError('agent did not return JSON or canonical nRFClaw pseudo-code')
    if not isinstance(obj,dict): raise AgentError('agent JSON root must be object')
    return obj


def build_program_system_prompt() -> str:
    return f'''You are the nRFClaw multilingual semantic planner.
Understand Portuguese, English and Spanish naturally. You may use other languages when clear.
The knowledge base below is authoritative. It is reloaded automatically on every invocation,
so changes to board facts, capabilities and examples are learned without model fine-tuning.

SAFETY CONTRACT:
- Never invent hardware capabilities, pins, electrical limits, timings, opcodes or bytecode.
- Never output VM bytecode.
- Convert the request to nRFClaw pseudo-code using ONLY the grammar below.
- Preserve conditions, else branches, repeat counts, event scopes, cadences, hysteresis,
  variable dependencies and persistence. Do not silently drop clauses.
- If the current pseudo grammar/runtime cannot express a clause, return status MISSING_CAPABILITY
  with explicit missing_capabilities and unresolved_clauses.
- If the request is ambiguous, return status AMBIGUOUS and explain what is missing.
- Prefer event-driven/low-power primitives over polling when behavior is equivalent.
- Natural machine/vibration anomaly wording maps to VIB_AUTO.ALARM when no other anomaly detector is named.
- If the user asks only for a LoRa alert and supplies no payload, use the documented KB canonical alert payload; never invent a different payload.

Return exactly one JSON object:
{{
  "schema_version": 2,
  "status": "COMPILE|MISSING_CAPABILITY|AMBIGUOUS|RESOURCE_CONFLICT",
  "language": "pt|en|es|other",
  "pseudo_code": "PROGRAM ...\\n...",
  "semantic_ast": {{...}},
  "unresolved_clauses": [],
  "missing_capabilities": [],
  "notes": []
}}

{knowledge_bundle()}
'''


def build_qa_system_prompt() -> str:
    return f'''You are the nRFClaw/NINASENSE multilingual product assistant.
Answer in the user's language. Use ONLY the supplied knowledge base for product-specific facts.
Distinguish FACT, MEASURED and ENGINEERING ESTIMATE. Never turn an estimate into a guarantee.
For battery-life calculations explain assumptions. For voltages, pins and electrical limits be exact.
If the KB does not establish the answer, say that validation/measurement is needed.
Return exactly JSON: {{"answer":"...","confidence":"high|medium|low","basis":["FACT|MEASURED|ESTIMATE"],"kb_sha256":"..."}}.

{knowledge_bundle()}
'''


def plan_program(source: str, cfg: AgentConfig|None=None) -> dict[str, Any]:
    cfg=cfg or load_config()
    raw=call_provider(cfg,build_program_system_prompt(),source)
    obj=_extract_json(raw, allow_pseudo=True)
    if obj.get('schema_version') != 2: raise AgentError('agent semantic schema must be 2')
    status=obj.get('status')
    if status not in ('COMPILE','MISSING_CAPABILITY','AMBIGUOUS','RESOURCE_CONFLICT'):
        raise AgentError(f'invalid agent status: {status!r}')
    pseudo=obj.get('pseudo_code','')
    if status=='COMPILE':
        if not isinstance(pseudo,str) or not pseudo.strip(): raise AgentError('COMPILE response has no pseudo_code')
        # Critical local validation: agent output must pass deterministic compiler.
        result=compile_pseudo(pseudo)
        obj['_compile_result']=result
    obj['kb_sha256']=knowledge_sha256()
    return obj




def _requested_program_mode(source: str) -> str:
    """Resolve the authenticated Schedule ABI mode from explicit user intent."""
    detected=detect_schedule_intent(source)
    return str((detected or {'mode':'MANUAL'})['mode']).upper()


def _invalid_action_snapshot(obj: Any, max_chars: int=280) -> str:
    """Return a compact sanitized structural clue for validator failures."""
    try:
        actions=((obj or {}).get('program') or {}).get('actions') or []
        for i,a in enumerate(actions):
            if not isinstance(a,dict) or not isinstance(a.get('op'),str):
                text=json.dumps({'index':i,'action':a},ensure_ascii=False,separators=(',',':'))
                return text[:max_chars]
    except Exception:
        pass
    return ''

def build_ir_system_prompt(source: str='') -> str:
    contract=json.dumps(schema_contract(), ensure_ascii=False, indent=2)
    expected_mode=_requested_program_mode(source)
    return f"""You are the nRFClaw multilingual Semantic IR planner.
Understand the user's original language directly. Do not translate through an
English keyword parser. Portuguese, English and Spanish are common, but any
language may be used; report language='other' when it is not pt/en/es.

ARCHITECTURE CONTRACT:
- Never emit VM bytecode or opcodes.
- Never emit nRFClaw pseudo-code.
- Return only Semantic IR JSON conforming to the contract below.
- The JSON object itself IS the IR. NEVER wrap it in root/data/result and NEVER emit vm_abi or transport metadata.
- Preserve EVERY requested clause and literal exactly, including prefixes,
  suffixes, intervals, conditions, persistence and data-flow.
- If an operation is semantically understood but the current VM cannot express
  it, return MISSING_CAPABILITY; do not silently remove or approximate it.
- If ambiguous, return AMBIGUOUS rather than guessing.
- The local validator/compiler is authoritative and may reject your output.
- Use the supplied knowledge/capability data only for device-specific facts.
- Every program.actions element is an object whose "op" value is ALWAYS a JSON STRING such as {{"op":"lora.receive","target":"buffer"}}. NEVER make "op" an object, array, or nested operation description.
- For THIS request, program.mode MUST be {expected_mode}. Schedule is authenticated program metadata, not a VM action. For AT/EVERY/WEEKLY include program.schedule with the requested semantic fields; never invent schedule opcodes.

SEMANTIC IR CONTRACT:
{contract}

IMPORTANT COMPOSITION RULES:
- A BLE advertising cadence belongs in ble.advertiser.interval_ms (example: interval_ms=5000 for 5 seconds). Do NOT add a wait solely to implement that BLE cadence.
- A continuous LoRa-to-BLE relay must return control flow to lora.receive after updating the advertised buffer, so later LoRa packets can replace the beacon payload.
- buffer.prepend applies to the arbitrary received BUFFER.
- buffer.set_literal replaces the current BUFFER with exactly the requested literal; use it for literal BLE payloads before ble.advertise_buffer.
- Preserve explicit literals and numeric values exactly.
- If LoRa RF parameters are omitted, use the persisted/current LoRa profile. Do NOT invent a required lora.config and do NOT return AMBIGUOUS merely because frequency/power/SF/BW/CR were not restated.
- If BLE advertising is requested without an explicit interval, use ble.advertiser with its runtime/default cadence. Do NOT return AMBIGUOUS merely because the interval was omitted.
- Typed buffer formatting already supports dynamic register values: use buffer.format_register for one register and buffer.format_2reg for two values instead of claiming generic itoa/string concatenation is missing.
- Immediate comparisons are expressible: load the constant with register.set, compare registers using register.compare(LT|GT|EQ), then branch with jump_if_zero.
- Return MISSING_CAPABILITY only after checking the dependency-closed action forms supplied for this request.
- NUMERIC JSON TYPE RULE: every numeric Semantic IR field MUST be emitted as a JSON number/integer, never as a quoted string. In particular accel.config odr_hz, full_scale_g, threshold_mg, and duration_ms are numeric. Correct example: {{"op":"accel.config","mode":"FALL","odr_hz":100,"full_scale_g":8,"threshold_mg":1000,"duration_ms":100,"low_power":false}}. Incorrect: values such as "100", "8", or "1000" in quotes.
- accel.config defaults are authoritative and deterministic: mode is required; odr_hz defaults to 10, full_scale_g to 2, threshold_mg to 120, duration_ms to 100, and low_power to true when omitted. If the user says only "configure FALL" / "configure FALL em baixo consumo", emit accel.config with mode=FALL and omit unspecified numeric fields instead of returning AMBIGUOUS or MISSING_CAPABILITY or inventing alternate values. Explicitly stated parameters override these defaults and remain subject to normal validation.
- Capability grounding aliases are authoritative: configure/setup UART or serial -> serial.config; receive/read a serial line -> serial.receive with mode=LINE; read/get battery -> battery.read. Do not invent aliases such as serial.configure, serial.receive_line, or read.battery.
- These canonical Semantic IR actions ARE available even if a retrieved grounding snippet omits them: {{"op":"serial.config","tx_pin":INT,"rx_pin":INT,"baud":INT,"economy":BOOL}}; {{"op":"serial.receive","mode":"LINE"}}; {{"op":"battery.read","register":R}}. Never report these canonical actions as missing capabilities.
- LoRa coding-rate mapping is authoritative: CR4/5 -> lora.config.cr=1, CR4/6 -> cr=2, CR4/7 -> cr=3, CR4/8 -> cr=4. These forms are not ambiguous.
- vib_auto.config requires explicit user-provided learning_s and initial_s. If the request asks to configure VIB_AUTO but does not provide both values, return AMBIGUOUS. Never invent or assume typical/default values such as learning_s=300 or initial_s=0.
- Do not claim that literal BLE payload construction is missing when buffer.set_literal is supplied.

{grounding_bundle(source, k=2)}
"""


def _normalize_provider_ir(obj: dict[str, Any]) -> dict[str, Any]:
    """Normalize only known transport wrappers; never repair semantic content."""
    if not isinstance(obj, dict):
        return obj
    # Compatibility for providers that wrap an otherwise complete IR. The
    # model-facing contract no longer asks for this, but accepting it here
    # prevents provider formatting quirks from becoming semantic failures.
    for wrapper in ("root", "data", "result"):
        inner=obj.get(wrapper)
        if isinstance(inner, dict) and ("ir_version" in inner or "status" in inner):
            obj=dict(inner)
            break
    obj=dict(obj)
    for k in ("vm_abi","provider","model","kb_sha256","agent_ir_attempts","attempts","latency_ms"):
        obj.pop(k, None)
    return obj


def _validate_source_semantics(source: str, obj: dict[str, Any]) -> None:
    """Small deterministic invariants for high-value composed intents.

    R3.8.20c2d also rejects a narrow set of false safe-rejections when the
    requested behavior is directly expressible by already-exposed typed IR.
    This triggers the normal repair pass instead of accepting a fabricated
    MISSING_CAPABILITY/AMBIGUOUS result.
    """
    low=source.lower()
    status=obj.get('status')
    if status != 'COMPILE':
        missing=' '.join(str(x) for x in (obj.get('missing_capabilities') or [])).lower()
        unresolved=' '.join(str(x) for x in (obj.get('unresolved_clauses') or [])).lower()
        notes=' '.join(str(x) for x in (obj.get('notes') or [])).lower()
        detail=' '.join((missing,unresolved,notes))

        # Dynamic numeric formatting is available through typed formatters.
        format_intent=any(x in low for x in ('format','formate','formata','formar','forme','mensagem','message','mensaje','prefix','prefixo'))
        false_format=any(x in detail for x in ('itoa','integer_to_ascii','integer to ascii','conversão de valores numéricos','conversao de valores numericos','concatena','buffer.append','stringify'))
        if status=='MISSING_CAPABILITY' and format_intent and false_format:
            raise SemanticIRError('semantic invariant: typed buffer.format_register/buffer.format_fixed/buffer.format_2reg can represent dynamic numeric formatting; do not claim generic itoa/string concatenation is required')
        literal_ble_intent=(any(x in low for x in ('ble','bluetooth','beacon','anunc','advert')) and any(q in source for q in ("'", '"')))
        false_literal_ble=any(x in detail for x in ('ble.advertise_literal','buffer.set_literal','buffer literal','literal arbitrário','literal arbitrario','payload de publicidade'))
        if status=='MISSING_CAPABILITY' and literal_ble_intent and false_literal_ble:
            raise SemanticIRError('semantic invariant: buffer.set_literal + ble.advertise_buffer can represent a literal BLE payload')

        # GT/LT/EQ against an immediate is expressible by loading the immediate
        # into a register, register.compare, then jump_if_zero.
        compare_intent=any(x in low for x in ('maior','menor','greater','less','igual','equal','>','<','=='))
        false_compare=any(x in detail for x in ('comparação \'maior que\'','comparacao \'maior que\'','comparison \'greater','jump_if_greater','desvio condicional','conditional branch'))
        if status=='MISSING_CAPABILITY' and compare_intent and false_compare:
            raise SemanticIRError('semantic invariant: register.set + register.compare(LT|GT|EQ) + jump_if_zero supports this comparison/branch; do not report it missing')

        # accel.config has deterministic ABI/runtime defaults for omitted physical
        # parameters. A request such as "configure FALL" is therefore not
        # ambiguous merely because ODR/scale/threshold/duration were omitted.
        accel_intent = ('fall' in low and any(x in low for x in ('configure','configurar','configura')))
        accel_defaults_detail = any(x in detail for x in (
            'odr_hz','full_scale_g','threshold_mg','duration_ms',
            'parâmetros numéricos','parametros numericos',
            'parámetros numéricos'
        ))
        if status in ('AMBIGUOUS', 'MISSING_CAPABILITY') and accel_intent and accel_defaults_detail:
            raise SemanticIRError(
                'semantic invariant: accel.config FALL has deterministic defaults '
                'for omitted odr_hz/full_scale_g/threshold_mg/duration_ms/low_power; '
                'do not reject solely because those optional parameters were not restated'
            )

        # Persisted/current radio profile is the product default when RF params
        # were not explicitly requested. BLE cadence likewise has a runtime
        # default when the user did not state an interval.
        explicit_lora_cfg=any(x in low for x in ('configure lora','configurar lora','configura lora','frequency','frequência','frecuencia','mhz','dbm','sf7','sf8','sf9','sf10','sf11','sf12','bw125','bw250','bw500','cr4/'))
        omitted_rf=('lora' in low and not explicit_lora_cfg)
        omitted_ble_interval=(any(x in low for x in ('ble','bluetooth','beacon','anunc','advert')) and not any(x in low for x in ('intervalo','interval ','a cada','every ','cada ')))
        asks_for_defaults=any(x in detail for x in ('configuração lora','configuracao lora','lora config','frequency','frequência','frecuencia','power','sf','bw','coding rate','intervalo de anúncio','intervalo de anuncio','advertising interval','cadência de advertising','cadencia de advertising'))
        if status=='AMBIGUOUS' and asks_for_defaults and (omitted_rf or omitted_ble_interval):
            raise SemanticIRError('semantic invariant: omitted LoRa RF parameters use the persisted/current profile and omitted BLE interval uses the runtime default; do not return AMBIGUOUS solely for those defaults')

        # Capability grounding false-rejection guards. These do not add new
        # capabilities; they expose canonical names/mappings already present
        # in the typed Semantic IR ABI.
        explicit_cr = None
        for text_cr, cr_num in (
            ('cr4/5',1), ('cr4/6',2),
            ('cr4/7',3), ('cr4/8',4)
        ):
            if text_cr in low:
                explicit_cr = (text_cr, cr_num)
                break

        if status=='AMBIGUOUS' and explicit_cr and any(
            x in detail for x in (
                'coding rate','tasa de codificación',
                'tasa de codificacion','cr=1',
                'cr (1..4)','cr 1..4'
            )
        ):
            text_cr, cr_num = explicit_cr
            raise SemanticIRError(
                f'semantic invariant: LoRa {text_cr.upper()} maps '
                f'deterministically to lora.config.cr={cr_num}; '
                'do not return AMBIGUOUS for this coding-rate notation'
            )

        missing_items = [
            str(x).lower().strip()
            for x in (obj.get('missing_capabilities') or [])
        ]

        serial_intent = ('serial' in low or 'uart' in low)
        canonical_serial_missing = any(
            x.startswith('serial.config') or x.startswith('serial.receive')
            for x in missing_items
        )
        false_serial = canonical_serial_missing or any(
            x in detail for x in (
                'serial.configure','serial.receive_line',
                'configure uart','configurar uart',
                'recibir una línea','recibir una linea'
            )
        )
        if status=='MISSING_CAPABILITY' and serial_intent and false_serial:
            raise SemanticIRError(
                'semantic invariant: canonical serial.config and '
                'serial.receive(mode=LINE) are available Semantic IR actions; '
                'do not report them or aliases as missing'
            )

        battery_intent = any(
            x in low for x in ('battery','bateria','batería')
        )
        canonical_battery_missing = any(
            x.startswith('battery.read') for x in missing_items
        )
        false_battery = canonical_battery_missing or any(
            x in detail for x in (
                'read.battery','leer la batería','leer la bateria',
                'read battery','battery read'
            )
        )
        if status=='MISSING_CAPABILITY' and battery_intent and false_battery:
            raise SemanticIRError(
                'semantic invariant: canonical battery.read(register=R) '
                'is an available Semantic IR action; do not report it '
                'or read.battery as missing'
            )
        sched=detect_schedule_intent(source)
        if sched and status in ('MISSING_CAPABILITY','AMBIGUOUS') and any(x in detail for x in ('schedule','rtc','calendar','clock','wall-clock','at_time')):
            raise SemanticIRError('semantic invariant: firmware Schedule ABI supports '+str(sched.get('mode'))+' as authenticated program metadata; do not report schedule/RTC capability missing merely because it is not an action opcode')
        return
    acts=(obj.get('program') or {}).get('actions') or []

    def _walk(xs):
        for a in xs:
            if not isinstance(a,dict):
                continue
            yield a
            nested=a.get('actions')
            if isinstance(nested,list):
                yield from _walk(nested)

    flat_acts=list(_walk(acts))
    ops=[a.get('op') for a in flat_acts]
    # VIB_AUTO configuration parameters are semantic inputs,
    # not planner defaults.
    if 'vib_auto.config' in ops:
        has_learning = (
            'learning_s' in low or
            'learning s' in low or
            'learning=' in low or
            'learning ' in low or
            'aprendiz' in low
        )
        has_initial = (
            'initial_s' in low or
            'initial s' in low or
            'initial=' in low or
            'initial ' in low or
            'inicial' in low
        )
        if not (has_learning and has_initial):
            raise SemanticIRError(
                'semantic invariant: vib_auto.config requires user-provided '
                'learning_s and initial_s; do not invent defaults—return '
                'AMBIGUOUS when either is unspecified'
            )

    expected_mode=_requested_program_mode(source)
    schedule_errors=validate_schedule_clause(source, obj.get('program'))
    if schedule_errors:
        raise SemanticIRError('semantic schedule oracle: ' + '; '.join(schedule_errors))
    relay=('lora.receive' in ops and 'ble.advertise_buffer' in ops and
           ('lora' in low) and any(x in low for x in ('beacon','advert','anunc','ble','bluetooth')))
    if not relay: return
    def _scope_relay_cycle(xs):
        # A loop container has an implicit back-edge from the end of its
        # actions to the first action. Explicit label/jump cycles also work.
        if not isinstance(xs,list):
            return False

        rx=[
            i for i,a in enumerate(xs)
            if isinstance(a,dict) and a.get('op')=='lora.receive'
        ]
        adv=[
            i for i,a in enumerate(xs)
            if isinstance(a,dict) and a.get('op')=='ble.advertise_buffer'
        ]
        labels={
            a.get('name'):i
            for i,a in enumerate(xs)
            if isinstance(a,dict) and a.get('op')=='label'
        }
        jumps=[
            (i,labels.get(a.get('label')))
            for i,a in enumerate(xs)
            if isinstance(a,dict) and a.get('op')=='jump'
        ]

        if rx and adv:
            for r in rx:
                for d in adv:
                    if r < d and any(
                        t is not None and t <= r and j > d
                        for j,t in jumps
                    ):
                        return True

        for a in xs:
            if not isinstance(a,dict):
                continue
            nested=a.get('actions')

            if a.get('op')=='loop' and isinstance(nested,list):
                nrx=[
                    i for i,x in enumerate(nested)
                    if isinstance(x,dict) and x.get('op')=='lora.receive'
                ]
                nadv=[
                    i for i,x in enumerate(nested)
                    if isinstance(x,dict) and x.get('op')=='ble.advertise_buffer'
                ]
                if nrx and nadv and any(r < d for r in nrx for d in nadv):
                    return True

            if isinstance(nested,list) and _scope_relay_cycle(nested):
                return True

        return False

    if not _scope_relay_cycle(acts):
        raise SemanticIRError(
            'semantic invariant: continuous LoRa-to-BLE relay must have a '
            'control-flow path from ble.advertise_buffer back to lora.receive'
        )
    intervals=[a.get('interval_ms') for a in flat_acts if a.get('op')=='ble.advertiser' and isinstance(a.get('interval_ms'),int)]
    waits=[a.get('seconds') for a in flat_acts if a.get('op')=='wait' and isinstance(a.get('seconds'),int)]
    cadence=any(x in low for x in ('a cada','every ','cada ','interval','intervalo'))
    if cadence and any(ms == sec*1000 for ms in intervals for sec in waits):
        raise SemanticIRError('semantic invariant: BLE advertising cadence is already represented by interval_ms; do not add an equivalent wait that delays LoRa reception')

def plan_ir_with_metadata(source: str, cfg: AgentConfig|None=None) -> tuple[dict[str, Any], dict[str, Any]]:
    """Ask a provider for Semantic IR and return (pure_ir, transport_metadata).

    R3.8.20c2 records per-attempt latency/rejection diagnostics while preserving
    the c1 invariant that transport metadata never enters the validated IR.
    """
    cfg=cfg or load_config()
    system=build_ir_system_prompt(source)
    raw=None
    last_error=None
    attempt_log=[]
    request=source
    for attempt in range(3):
        t0=time.monotonic()
        try:
            raw=call_provider(cfg, system, request)
            provider_ms=round((time.monotonic()-t0)*1000,1)
        except Exception as exc:
            provider_ms=round((time.monotonic()-t0)*1000,1)
            attempt_log.append({'attempt':attempt+1,'elapsed_ms':provider_ms,'valid':False,'error':str(exc)})
            raise
        obj=None
        try:
            obj=_normalize_provider_ir(_extract_json(raw, allow_pseudo=False))
            obj=validate_ir(obj, require_compile=False)
            _validate_source_semantics(source,obj)
            pmeta=last_provider_meta()
            attempt_log.append({'attempt':attempt+1,'elapsed_ms':provider_ms,'valid':True,'error':'',**pmeta})
            meta={
                'provider': cfg.provider,
                'model': cfg.model,
                'kb_sha256': knowledge_sha256(),
                'attempts': attempt+1,
                'repairs': attempt,
                'total_provider_ms': round(sum(x['elapsed_ms'] for x in attempt_log),1),
                'attempt_log': attempt_log,
                'prompt_tokens': sum(int(x.get('prompt_tokens',0) or 0) for x in attempt_log),
                'completion_tokens': sum(int(x.get('completion_tokens',0) or 0) for x in attempt_log),
                'total_tokens': sum(int(x.get('total_tokens',0) or 0) for x in attempt_log),
                'cost': sum(float(x.get('cost',0) or 0) for x in attempt_log),
            }
            return obj, meta
        except (SemanticIRError, AgentError) as exc:
            last_error=exc
            pmeta=last_provider_meta()
            snapshot=_invalid_action_snapshot(obj)

            # Provider-neutral diagnostics only. Observe the rejected Semantic IR
            # exactly as returned; never coerce or repair ir_version here.
            observed_version = None
            observed_version_type = 'missing'
            observed_keys = []
            if isinstance(obj, dict):
                observed_keys = sorted(str(k) for k in obj.keys())[:32]
                if 'ir_version' in obj:
                    observed_version = obj.get('ir_version')
                    observed_version_type = type(observed_version).__name__

            entry={
                'attempt': attempt+1,
                'elapsed_ms': provider_ms,
                'valid': False,
                'error': str(exc),
                'ir_version_observed': observed_version,
                'ir_version_type': observed_version_type,
                'top_level_keys': observed_keys,
                'raw_preview': str(raw)[:1500],
                **pmeta,
            }
            if snapshot:
                entry['invalid_action']=snapshot
            attempt_log.append(entry)
            if attempt >= 2:
                break
            request = (
                "Your previous Semantic IR was rejected by the authoritative local validator.\n"
                f"VALIDATION ERROR: {exc}\n"
                "Return a corrected COMPLETE Semantic IR JSON object only. The JSON object itself is the IR: "
                "do not wrap it in root/data/result and do not emit vm_abi or metadata. Every action.op MUST be a JSON string. "
                f"program.mode MUST be {_requested_program_mode(source)} for this request. Do not explain. "
                "Do not remove user requirements merely to satisfy validation. If a requirement "
                "cannot be represented, return status MISSING_CAPABILITY.\n\n"
                "ORIGINAL USER REQUEST:\n"+source+"\n\n"
                "REJECTED OUTPUT:\n"+raw
            )
    detail='; '.join(
        f"#{x['attempt']} {x['elapsed_ms']:.0f}ms: {x['error']} "
        f"[ir_version={x.get('ir_version_observed')!r} "
        f"type={x.get('ir_version_type')} "
        f"keys={x.get('top_level_keys')} "
        f"raw={x.get('raw_preview','')[:500]!r}]"
        for x in attempt_log if not x['valid']
    )
    raise AgentError(f'invalid Semantic IR after 3 attempts: {last_error}; attempts: {detail}')


def plan_ir(source: str, cfg: AgentConfig|None=None) -> dict[str, Any]:
    """Compatibility API: return only validated, schema-pure Semantic IR."""
    ir,_meta=plan_ir_with_metadata(source,cfg)
    return ir


def compile_ir_plan(source: str, cfg: AgentConfig|None=None):
    """Provider -> pure Semantic IR -> local validator/lowering -> bytecode.

    Returns a wrapper object; compiler artifacts and metadata never enter the IR.
    """
    obj,meta=plan_ir_with_metadata(source, cfg)
    if obj.get('status') != 'COMPILE':
        details=obj.get('missing_capabilities') or obj.get('unresolved_clauses') or obj.get('notes') or []
        raise AgentError(f"Semantic IR status {obj.get('status')}: {'; '.join(details)}")
    try:
        result,pseudo=compile_ir(obj, source=source)
    except SemanticIRMissingCapability as exc:
        raise AgentError(f'MISSING_CAPABILITY {exc.capability}: {exc.detail}') from exc
    except SemanticIRError as exc:
        raise AgentError(f'Semantic IR compile rejected: {exc}') from exc
    return {'ir':obj, 'metadata':meta, 'compile_result':result, 'pseudo_code':pseudo}

def answer_external(question: str, cfg: AgentConfig|None=None) -> dict[str, Any]:
    cfg=cfg or load_config()
    raw=call_provider(cfg,build_qa_system_prompt(),question,
                      response_kind="json_object")
    obj=_extract_json(raw)
    if not isinstance(obj.get('answer'),str): raise AgentError('agent QA response has no answer')
    obj['kb_sha256']=knowledge_sha256()
    return obj

# ---------------------------------------------------------------------------
# R3.8.20c2g1 - Lean Agent Fallback + Strict Capability Contract
# ---------------------------------------------------------------------------

def _lean_action_forms(source: str) -> tuple[list[str], list[dict[str, Any]], list[str]]:
    """Return the dependency-closed operation set without examples/full KB."""
    from nrfclaw_semantic_grounding import select_operations
    ops, groups = select_operations(source)
    forms=[]
    try:
        ref=json.loads(_read(TOOLS / 'nrfclaw_semantic_ir_reference_r3820c.json'))
    except Exception:
        ref={}
    catalog=(ref or {}).get('operations') or {}
    for op in ops:
        spec=catalog.get(op)
        if spec is not None:
            row={'op':op}
            row.update(spec)
            forms.append(row)
    return list(ops), forms, list(groups)




def _lean_dependency_cards(source: str, available_ops: list[str], groups: list[str]) -> list[str]:
    """Compact, compositional semantics attached only when their dependencies are selected.

    These are not task examples and never mention benchmark family IDs.  They describe
    mandatory relationships between primitives so a small provider prompt still carries
    the structural semantics needed to compose a valid CFG.
    """
    low=source.lower(); ops=set(available_ops); gs=set(groups); cards=[]
    sched=detect_schedule_intent(source)
    if sched:
        cards.append('SCHEDULE ABI: authenticated program metadata, never a VM action/opcode. Use program.mode='+str(sched.get('mode'))+'. AT requires program.schedule time/date/timezone or epoch_utc; EVERY requires interval_s; WEEKLY requires days[] and time. Do not report RTC/schedule.at_time as a missing action.')
    continuous=any(x in low for x in (
        'continuamente','continuously','continue monitoring','continue monitorando',
        'continue waiting','continue esperando','volte a aguardar','vuelva a esperar',
        'wait for the next','próximo evento','proximo evento','next packet','next alarm',
        'next warning','forever','sem parar','sin parar','persistent behavior','comportamento persistente',
        'while keeping','while tracking remains active','mantendo','permanezca activo','repeat forever','repita'))
    periodic=any(x in low for x in (
        'every ','a cada ','cada ','periodic','periodicamente','periódicamente','periodically',
        'interval','cadence','cadência','cadencia'))
    eventish=bool(gs & {'hall','accel','vib'}) or any(x in low for x in ('event','evento','alarm','alarme','warning','fall','queda','motion','movimento'))

    if 'ble' in gs:
        cards.append('BLE ADVERTISING: ble.advertiser configures the advertiser; ble.adv_start starts it. To advertise mutable BUFFER content use ble.advertise_buffer. For a literal payload, buffer.set_literal(value) then ble.advertise_buffer. ble.name is a device name, not a payload substitute. Advertising data does not implicitly start the advertiser.')
    if 'serial' in gs:
        cards.append('SERIAL RX: serial.config must precede serial.receive in the program unless the request explicitly says an already-configured session exists. serial.receive places received data in BUFFER; serial.write_buffer sends BUFFER to serial.')
    if 'lora.receive' in ops:
        cards.append('LORA RX: lora.receive places the packet in mutable BUFFER. If the request asks for the next packet/continuous listening, put receive and routing in a CFG cycle and jump back to receive; forwarding BUFFER must not destroy it first.')
    if 'tracking' in gs:
        cards.append('TRACKING: tracking.config/rotation configure tracking and tracking.start activates it. If tracking must remain active while periodic work runs, start tracking once outside the periodic cycle; do not stop it unless explicitly requested.')
    if 'vib' in gs:
        cards.append('VIB_AUTO: explicit learning/initial values belong in vib_auto.config, then vib_auto.start. Alarm/warning handling is event-driven: wait.event for the requested VIB event, perform actions, and if more events are requested jump back to the wait. Do not invent learning/initial values.')
    if 'accel' in gs:
        cards.append('ACCEL EVENT: accel.config configures the requested mode (e.g. MOTION/FALL), then wait.event waits for that event. Repeated event handling must jump back to wait.event after the requested actions.')
    if 'hall' in gs:
        cards.append('HALL EVENT: hall.wait blocks for/captures the next Hall event. If only one event is requested, the manual program may finish afterward. If next/repeated events are requested, actions must jump/loop back to hall.wait.')
    if 'ha' in gs and 'app.send_buffer' in ops:
        cards.append('APPLICATION ROUTING: app.send_buffer forwards the current mutable BUFFER to the application plane. For repeated sources, route BUFFER and then return to the source wait/read operation.')
    if 'debug' in gs:
        cards.append('DEBUG ROUTING: debug.buffer outputs the current BUFFER. It does not configure or receive serial data; preserve the serial.config -> serial.receive dependency when serial is the source.')
    if 'power' in gs:
        cards.append('MINIMUM POWER: system.minimum_power is an explicit power-mode action, not a terminal state. Requested one-shot sensor/read/send actions still follow it in program order; do not report them as missing merely because minimum-power mode was requested.')
    if periodic:
        cards.append('PERIODIC CFG: implement the requested period explicitly as a real cycle: label/loop -> requested work -> wait(requested interval) -> jump/back-edge. Never invent a different interval. A one-shot action is not equivalent to periodic behavior.')
    elif continuous and not eventish and 'lora.receive' not in ops:
        cards.append('CONTINUOUS CFG: continuous/repeating behavior requires a real CFG cycle. Perform the requested read/actions, then jump/loop back to the monitoring point. Do not add an unrequested wait/delay.')
    if eventish and continuous:
        cards.append('REPEATED EVENT CFG: configure/start the event source once when required; label the wait point; wait for event -> perform all event actions -> jump back to the wait point. Do not reconfigure the source on every iteration unless explicitly requested.')
    if 'gpio' in gs and any(x in low for x in ('limit','limite','greater','less','maior','menor','if ','se ','si ')):
        cards.append('GPIO CONDITION: read the source value, load comparison constants with register.set, use register.compare (LT/GT/EQ), then jump_if_zero to form explicit true/false control flow. gpio.write performs the requested output state.')
    if 'gpio' in gs and any(x in low for x in ('mudar','mudança','mudanca','change','changes','cambio')):
        cards.append('GPIO CHANGE: sample an initial previous state once, read current state in the loop, compare current vs previous, perform side effects only on change, and update previous=current on the correct changed path before returning to monitoring.')
    if any(x in low for x in ('counter','contador','contador','increment','incremente','aumente')):
        cards.append('COUNTER: initialize counter and constants with register.set. register.add/sub operands are register IDs. For threshold behavior, compare after increment when the request says "when counter reaches" the threshold; reset only on the threshold action path if requested.')
    return cards

def build_lean_ir_system_prompt(source: str='') -> str:
    """Small provider prompt for fallback planning.

    Unlike build_ir_system_prompt(), this intentionally omits the global KB,
    full schema dump and retrieved examples. The provider sees only the root
    contract plus dependency-closed action forms selected for this request.
    """
    ops,forms,groups=_lean_action_forms(source)
    cards=_lean_dependency_cards(source,ops,groups)
    expected_mode=_requested_program_mode(source)
    allowed=','.join(ops)
    forms_json=json.dumps(forms,ensure_ascii=False,separators=(',',':'))
    groups_json=json.dumps(groups,ensure_ascii=False,separators=(',',':'))
    cards_text=('\nDEPENDENCY SEMANTICS:\n- '+'\n- '.join(cards)) if cards else ''
    return f"""You are the nRFClaw Semantic IR planner. Return exactly one JSON object, no markdown.

ROOT CONTRACT:
- ir_version: 1
- status: COMPILE | MISSING_CAPABILITY | AMBIGUOUS | RESOURCE_CONFLICT
- language: pt | en | es | other
- program: {{\"mode\":\"{expected_mode}\",\"schedule\":{{...}}?,\"actions\":[...]}} or null only for non-COMPILE status
- unresolved_clauses: string[]
- missing_capabilities: string[]
- notes: string[]
- Every action.op is a JSON string.
- Never emit bytecode or pseudo-code.
- Preserve every requested clause, literal, value, condition and continuous behavior.
- Do not invent waits/delays/cadences that the user did not request.
- Continuous/repeating behavior requires a real CFG cycle using loop or label/jump.
- register.add/sub operands are register IDs; load constants with register.set.
- register.compare produces 1/0 for LT|GT|EQ; branch with jump_if_zero.
- LoRa RF parameters omitted by the user use the persisted/current profile.
- BLE interval omitted by the user uses the runtime/default cadence.
- Numeric fields are JSON numbers, never quoted numbers.
- Schedule is authenticated PROGRAM METADATA, not an action.op. AT uses time/date/timezone or epoch_utc; EVERY uses interval_s (+ optional anchor_epoch_utc); WEEKLY uses days[] + time + timezone.

STRICT CAPABILITY CONTRACT:
The dependency-closed action forms below are AVAILABLE and sufficient building blocks for all recognized capabilities in this request.
If status=MISSING_CAPABILITY, every missing_capabilities item MUST name a canonical primitive/capability that is NOT in AVAILABLE_OPS and MUST correspond to an explicit user clause that cannot be expressed by any available composition.
Never use MISSING_CAPABILITY to mean \"I am unsure how to compose the program\".
If all requested clauses can be composed from AVAILABLE_OPS, status MUST be COMPILE.

SELECTED_GROUPS={groups_json}
AVAILABLE_OPS={allowed}
ACTION_FORMS={forms_json}
{cards_text}
"""


def _strict_missing_capability(source: str, obj: dict[str, Any], available_ops: list[str]) -> None:
    """Reject false MISSING_CAPABILITY when the model names available actions."""
    if obj.get('status') != 'MISSING_CAPABILITY':
        return
    missing=[str(x).strip() for x in (obj.get('missing_capabilities') or []) if str(x).strip()]
    if not missing:
        raise SemanticIRError('strict capability contract: MISSING_CAPABILITY requires explicit missing_capabilities')
    available=set(available_ops)
    bad=[]
    for item in missing:
        head=re.split(r'[:\s]',item,maxsplit=1)[0].strip('`"\'')
        if head in available:
            bad.append(head)
    if bad:
        raise SemanticIRError('strict capability contract: provider reported available primitive(s) as missing: '+','.join(sorted(set(bad))))


def _lean_continuous_cfg_guard(source: str, obj: dict[str, Any]) -> None:
    """Objective guard against dropping the back-edge from continuous intents."""
    if obj.get('status') != 'COMPILE':
        return
    low=source.lower()
    continuous=any(x in low for x in (
        'continuamente','continuously','continue monitorando','continue monitoring',
        'sempre que','whenever','cada vez que','every time','forever','para sempre','siempre',
        'próximo','proximo','next packet','next event','repita','repeat'))
    if not continuous:
        return
    acts=(obj.get('program') or {}).get('actions') or []
    def walk(xs):
        for a in xs or []:
            if not isinstance(a,dict): continue
            yield a
            if isinstance(a.get('actions'),list):
                yield from walk(a['actions'])
    flat=list(walk(acts)); ops=[a.get('op') for a in flat]
    if 'loop' in ops:
        return
    labels={a.get('name') for a in flat if a.get('op')=='label'}
    if any(a.get('op')=='jump' and a.get('label') in labels for a in flat):
        return
    raise SemanticIRError('semantic invariant: continuous/repeating request requires an explicit loop or label/jump CFG cycle')


def _lean_repairable_error(exc: Exception) -> bool:
    """Only one repair for objective structural/semantic validator defects."""
    text=str(exc).lower()
    keys=(
        'strict capability contract:',
        'continuous/repeating request requires',
        'semantic invariant:',
        'must be', 'required', 'invalid', 'unknown operation', 'out of range',
    )
    return any(k in text for k in keys)


class LeanSemanticIRError(AgentError):
    """Lean planner failure that preserves provider telemetry for the harness."""
    def __init__(self, message: str, metadata: dict[str, Any] | None = None, raw: str = ""):
        super().__init__(message)
        self.metadata = metadata or {}
        self.raw = raw


def _lean_failure_metadata(cfg: AgentConfig, groups: list[str], available_ops: list[str],
                           system: str, attempt_log: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        'planner':'lean-c2g2','provider':cfg.provider,'model':cfg.model,
        'selected_groups':groups,'available_ops_count':len(available_ops),
        'system_prompt_chars':len(system),'attempts':len(attempt_log),
        'repairs':max(0,len(attempt_log)-1),'first_pass_valid':False,
        'total_provider_ms':round(sum(float(x.get('elapsed_ms',0) or 0) for x in attempt_log),1),
        'attempt_log':attempt_log,
        'prompt_tokens':sum(int(x.get('prompt_tokens',0) or 0) for x in attempt_log),
        'completion_tokens':sum(int(x.get('completion_tokens',0) or 0) for x in attempt_log),
        'total_tokens':sum(int(x.get('total_tokens',0) or 0) for x in attempt_log),
        'cost':sum(float(x.get('cost',0) or 0) for x in attempt_log),
    }


def plan_ir_lean_with_metadata(source: str, cfg: AgentConfig|None=None,
                               allow_repair: bool=True) -> tuple[dict[str, Any], dict[str, Any]]:
    """Lean provider fallback: one first pass + at most one objective repair."""
    cfg=cfg or load_config()
    system=build_lean_ir_system_prompt(source)
    available_ops,_,groups=_lean_action_forms(source)
    attempt_log=[]; raw=''; last_error=None; request=source
    max_attempts=2 if allow_repair else 1
    for attempt in range(max_attempts):
        t0=time.monotonic()
        raw=call_provider(cfg,system,request,semantic_ops=available_ops)
        provider_ms=round((time.monotonic()-t0)*1000,1)
        obj=None
        try:
            obj=_normalize_provider_ir(_extract_json(raw,allow_pseudo=False))
            obj=validate_ir(obj,require_compile=False)
            _strict_missing_capability(source,obj,available_ops)
            _validate_source_semantics(source,obj)
            _lean_continuous_cfg_guard(source,obj)
            pmeta=last_provider_meta()
            attempt_log.append({'attempt':attempt+1,'elapsed_ms':provider_ms,'valid':True,'error':'',**pmeta})
            meta={
                'planner':'lean-c2g2','provider':cfg.provider,'model':cfg.model,
                'selected_groups':groups,'available_ops_count':len(available_ops),
                'system_prompt_chars':len(system),'attempts':attempt+1,'repairs':attempt,
                'first_pass_valid': attempt==0,
                'total_provider_ms':round(sum(x['elapsed_ms'] for x in attempt_log),1),
                'attempt_log':attempt_log,
                'prompt_tokens':sum(int(x.get('prompt_tokens',0) or 0) for x in attempt_log),
                'completion_tokens':sum(int(x.get('completion_tokens',0) or 0) for x in attempt_log),
                'total_tokens':sum(int(x.get('total_tokens',0) or 0) for x in attempt_log),
                'cost':sum(float(x.get('cost',0) or 0) for x in attempt_log),
            }
            return obj,meta
        except (SemanticIRError,AgentError) as exc:
            last_error=exc; pmeta=last_provider_meta()
            attempt_log.append({'attempt':attempt+1,'elapsed_ms':provider_ms,'valid':False,'error':str(exc),**pmeta})
            if attempt+1 >= max_attempts or not allow_repair or not _lean_repairable_error(exc):
                break
            request=(
                'The authoritative local validator rejected the previous Semantic IR.\n'
                f'ERROR: {exc}\n'
                'Repair only this objective defect. Preserve every other user requirement and value. '
                'Return one COMPLETE Semantic IR JSON object only. Do not explain.\n\n'
                'ORIGINAL REQUEST:\n'+source+'\n\nREJECTED IR:\n'+raw
            )
    detail='; '.join(f"#{x['attempt']} {x['error']}" for x in attempt_log)
    meta=_lean_failure_metadata(cfg,groups,available_ops,system,attempt_log)
    raise LeanSemanticIRError(
        f'lean Semantic IR rejected after {len(attempt_log)} attempt(s): {last_error}; {detail}',
        metadata=meta, raw=raw)
