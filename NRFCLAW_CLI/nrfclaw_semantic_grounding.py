#!/usr/bin/env python3
"""R3.8.20c2e dependency-aware Semantic IR grounding.

Start from lexical intent, then expand to the minimum complete capability
closure needed to express composed requests.  The local validator/compiler
still knows the full IR and remains authoritative.
"""
from __future__ import annotations
import json,re,os
from pathlib import Path
HERE=Path(__file__).resolve().parent
CORPUS=HERE/'r3820b_benchmark_500.jsonl'
IR_REF=HERE/'nrfclaw_semantic_ir_reference_r3820c.json'

# Small control-flow baseline. loop is included because it can be more compact
# than explicit labels/jumps and is already part of the Semantic IR validator.
ALWAYS={'label','jump','jump_if_zero','wait','loop'}

# Capability families.  lora.config is intentionally NOT part of the generic
# LoRa family: a send/receive request may use the persisted radio profile.
GROUPS={
 'lora': {'lora.receive','lora.send_literal','lora.send_buffer'},
 'lora_config': {'lora.config'},
 'ble': {'ble.ndp','ble.advertiser','ble.name','ble.adv_start','ble.advertise_buffer'},
 'buffer': {'buffer.prepend','buffer.set_literal','buffer.parse_int','buffer.parse_fixed','buffer.format_register','buffer.format_fixed','buffer.format_2reg'},
 'gpio': {'gpio.read','gpio.write'},
 'register': {'register.set','register.copy','register.add','register.sub','register.compare'},
 'accel': {'accel.config','wait.event'},
 'battery': {'battery.read'}, 'temperature': {'temperature.read'}, 'hall': {'hall.wait'},
 'serial': {'serial.config','serial.receive','serial.write_buffer'},
 'tracking': {'tracking.config','tracking.rotation','tracking.start','tracking.stop'},
 'vib': {'vib_auto.config','vib_auto.start','vib_auto.stop','wait.event'},
 'ha': {'ha.event','app.send_buffer'}, 'debug': {'debug.buffer'},
 'power': {'system.minimum_power'},
 'schedule': set(),
}

KEYWORDS={
 'lora': ('lora','lorawan'),
 'ble': ('ble','bluetooth','beacon','advert','anunc','baliza'),
 'buffer': ('buffer','prefix','prefixo','prepend','payload','pacote','packet','recebido','received',
            'format','formate','formata','formar','forme','mensaje','mensagem','message','concat','única mensagem','single message'),
 'gpio': ('gpio','p0.','pino','pin '),
 'register': ('registr','counter','contador','increment','decrement','incremente','aumente','zer','zero',
              'maior','menor','greater','less','igual','equal','cuando llegue','quando chegar','when it reaches'),
 'accel': ('accel','aceler','motion','movimento','tap','fall','queda','walk','vibration','vibra'),
 'battery': ('battery','bateria','vbat'),
 'temperature': ('temperature','temperatura','ds18'),
 'hall': ('hall',),
 'serial': ('serial','uart'),
 'tracking': ('tracking','openhaystack','find my'),
 'vib': ('vib_auto','machine_on','machine_off','warning','alarm','alarme'),
 'ha': ('home assistant','ha event','ndp','aplicação','aplicacao','application'),
 'debug': ('debug','rtt'),
 'power': ('minimum-power','minimum power','potência mínima','potencia minima'),
 'schedule': (' at ','às ','a las ','daily','todos os dias','weekly','monday','tuesday','wednesday','thursday','friday','saturday','sunday'),
}

FORMAT_WORDS=('format','formate','formata','formar','forme','mensagem','message','mensaje','prefix','prefixo')
COMPARE_WORDS=('maior','menor','greater','less','igual','equal','chegar a','reaches','llegue a','>','<','==')
CHANGE_WORDS=('mudar','mudança','mudanca','change','cambio','cambie','altere','changes')
CONTINUOUS_WORDS=('continu','repita','repeat','repeating','forever','siempre','novo','next','próximo','proximo')


def _tokens(s): return set(re.findall(r"[a-zA-ZÀ-ÿ0-9_.=+-]+",s.lower()))


def _contains_any(text, words):
    return any(w in text for w in words)


def _explicit_lora_config(text: str) -> bool:
    """Only expose lora.config when the user actually asks to configure RF."""
    if not ('lora' in text or 'lorawan' in text): return False
    if any(x in text for x in ('configure lora','configurar lora','configura lora','config loRa'.lower(),
                               'frequency','frequência','frecuencia','mhz','dbm','sf7','sf8','sf9','sf10','sf11','sf12',
                               'bw125','bw250','bw500','coding rate','cr4/')):
        return True
    return False


def select_operations(prompt):
    """Select primary families then compute a minimal dependency closure."""
    t=prompt.lower(); ops=set(ALWAYS); groups=[]
    for g,keys in KEYWORDS.items():
        if any(k in t for k in keys):
            groups.append(g); ops.update(GROUPS[g])

    # Generic LoRa requests use the persisted/default radio profile.  Expose
    # lora.config only for explicit RF configuration intent.
    if _explicit_lora_config(t):
        groups.append('lora_config'); ops.update(GROUPS['lora_config'])

    if re.match(r'^\s*(?:every|a cada|cada)\s+\d+\s*(?:seconds?|segundos?|minutes?|minutos?|hours?|horas?)\b',t) and 'schedule' not in groups:
        groups.append('schedule')

    # -------- dependency closure --------
    # Received payload forwarded elsewhere needs buffer/data movement tools.
    # Do not add buffer formatters merely because a request sends the same literal
    # through LoRa and BLE.
    relay_words=('buffer','pacote','packet','recebido','received','relay','retransm','encaminh')
    if 'lora' in groups and ('ble' in groups or 'serial' in groups or 'ha' in groups) and _contains_any(t,relay_words):
        ops.update(GROUPS['buffer'])

    # Literal BLE payloads need a deterministic buffer initializer.  Unlike
    # buffer.prepend, buffer.set_literal replaces stale buffer content.
    literal_ble = ('ble' in groups and any(q in prompt for q in ("'", '"')) and
                   any(x in t for x in ('anunc','advert','beacon','ble','bluetooth')))
    if literal_ble:
        ops.update({'buffer.set_literal','ble.advertise_buffer','ble.adv_start'})

    # Any dynamic numeric formatting requires register formatters and a buffer
    # transport.  Supplying all three formatter variants costs little and keeps
    # the agent from falsely declaring string/itoa capability missing.
    if _contains_any(t, FORMAT_WORDS):
        ops.update(GROUPS['buffer'])
        if any(g in groups for g in ('register','battery','temperature','hall','gpio')):
            ops.update(GROUPS['register'])
        if 'lora' in groups: ops.add('lora.send_buffer')
        if 'ble' in groups: ops.add('ble.advertise_buffer')

    # Two sensor/value fields in one message specifically needs format_2reg.
    value_groups=[g for g in ('battery','temperature','hall','gpio','register') if g in groups]
    if len(value_groups) >= 2 and _contains_any(t, FORMAT_WORDS):
        ops.add('buffer.format_2reg')

    # Parsing and conditional routing need a constant register, compare result,
    # jump_if_zero and GPIO/app actions as applicable.
    conditional = _contains_any(t, COMPARE_WORDS) or any(x in t for x in ('se ','if ','caso contrário','otherwise','si '))
    if conditional:
        ops.update(GROUPS['register'])
        ops.update({'jump_if_zero','label','jump'})

    # Detecting GPIO changes requires remembering the previous sample and
    # comparing it with the current sample.
    if 'gpio' in groups and _contains_any(t, CHANGE_WORDS):
        ops.update(GROUPS['register'])
        ops.update({'jump_if_zero','label','jump'})

    # Counter semantics always need initialized constants/add/compare even when
    # the lexical classifier only saw a sensor/event term.
    if any(x in t for x in ('contador','counter','incremente','increment','aumente')):
        ops.update(GROUPS['register'])

    # Serial text -> integer -> condition.
    if 'serial' in groups and any(x in t for x in ('inteiro','integer','parse','converta','convert')):
        ops.update({'buffer.parse_int'})
        ops.update(GROUPS['register'])

    # Event-based accelerometer/VIB flows depend on wait.event.
    if 'accel' in groups or 'vib' in groups:
        ops.add('wait.event')

    # Continuous behavior needs an available loop mechanism even if no cadence
    # keyword was present.
    if _contains_any(t, CONTINUOUS_WORDS):
        ops.update({'loop','label','jump'})

    # De-duplicate group labels while preserving discovery order.
    groups=list(dict.fromkeys(groups))
    return sorted(ops),groups


def retrieve_examples(prompt,k=2):
    if not CORPUS.exists() or os.getenv('NRFCLAW_AGENT_BENCHMARK')=='1': return []
    q=_tokens(prompt); scored=[]
    for line in CORPUS.read_text(encoding='utf-8').splitlines():
        if not line.strip(): continue
        x=json.loads(line); tx=_tokens(x.get('phrase','')); score=len(q&tx)
        score += 1.5*len(set(re.findall(r'\d+',prompt)) & set(re.findall(r'\d+',x.get('phrase',''))))
        score += 2*len(set(re.findall(r"['\"]([^'\"]+)['\"]",prompt)) & set(re.findall(r"['\"]([^'\"]+)['\"]",x.get('phrase',''))))
        scored.append((score,x))
    scored.sort(key=lambda z:(-z[0],z[1].get('id',0)))
    return [x for score,x in scored[:k] if score>0]


def grounding_bundle(prompt,k=2):
    ops,groups=select_operations(prompt); parts=[]
    if IR_REF.exists():
        ref=json.loads(IR_REF.read_text(encoding='utf-8'))
        action_forms=[]
        for op in ops:
            if op not in ref.get('operations',{}):
                continue
            row={'op':op}; row.update(ref['operations'][op]); action_forms.append(row)
        slim={'ir_version':ref.get('ir_version',1),'selected_groups':groups,
              'dependency_closed':True,'action_forms':action_forms}
        parts.append('===== AUTHORITATIVE SEMANTIC IR ACTION FORMS (DEPENDENCY-CLOSED) =====\n'+json.dumps(slim,ensure_ascii=False,separators=(',',':')))

    # Compact runtime/default semantics prevent false ambiguity.  These are
    # product semantics, not guessed RF values.
    notes=[]
    if 'schedule' in groups:
        notes.append('Schedule is authenticated PROGRAM METADATA, not a VM action/opcode. Firmware Schedule ABI modes are MANUAL=0, AT=1, EVERY=2, WEEKLY=3, BOOT=4. AT lowers to UTC epoch; EVERY uses interval seconds plus anchor epoch; WEEKLY uses Monday=bit0..Sunday=bit6 and UTC seconds since midnight. Never return MISSING_CAPABILITY merely because schedule is not an action.op.')
        notes.append('Semantic IR: AT => program.mode=AT + program.schedule={time, optional date, timezone} or {epoch_utc}; EVERY => {interval_s, optional anchor_epoch_utc}; WEEKLY => {days:[MON..SUN], time, timezone}.')
    if 'lora' in groups and 'lora_config' not in groups:
        notes.append('LoRa RF parameters were not requested: use the persisted/current LoRa profile. Do NOT require lora.config and do NOT return AMBIGUOUS merely because frequency/power/SF/BW/CR are omitted.')
    if 'ble' in groups:
        notes.append('If BLE advertising is requested without an explicit interval, emit ble.advertiser without interval_ms to use the runtime default/current advertiser cadence. Do NOT return AMBIGUOUS for an omitted BLE interval.')
        if any(q in prompt for q in ("'", '"')):
            notes.append('To advertise a literal payload over BLE, use buffer.set_literal(value) followed by ble.advertise_buffer. ble.name changes the local device name and is not a generic payload substitute.')
    if _contains_any(prompt.lower(), FORMAT_WORDS):
        notes.append('Dynamic numeric message construction is supported by buffer.format_register, buffer.format_fixed and buffer.format_2reg; do not claim generic itoa/string concatenation is missing when one of these typed formatters can represent the request.')
    if _contains_any(prompt.lower(), COMPARE_WORDS) or any(x in prompt.lower() for x in ('se ','if ','caso contrário','otherwise','si ')):
        notes.append('Register conditions are supported with register.set + register.compare (LT|GT|EQ) + jump_if_zero; immediate constants can be loaded into a register before comparison.')
    if notes:
        parts.append('===== DEPENDENCY / DEFAULT SEMANTICS =====\n- '+'\n- '.join(notes))

    ex=retrieve_examples(prompt,k)
    allowed=set(ops)|{'loop'}
    def _ops(actions):
        out=set()
        for a in actions or []:
            if isinstance(a,dict):
                if isinstance(a.get('op'),str): out.add(a['op'])
                if a.get('op')=='loop': out |= _ops(a.get('actions'))
        return out
    ex=[x for x in ex if _ops((x.get('ir',{}).get('program') or {}).get('actions')) <= allowed]
    if ex:
        rows=[json.dumps({'user':x['phrase'],'expected_ir':x['ir']},ensure_ascii=False,separators=(',',':')) for x in ex]
        parts.append('===== RETRIEVED VALID IR EXAMPLES =====\n'+'\n'.join(rows))
    return '\n\n'.join(parts)
