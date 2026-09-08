#!/usr/bin/env python3
"""R3.8.20c2g3 standalone semantic certification.

This module is deliberately independent from the lowerers.  It extracts a
conservative intent contract from natural-language clauses that the standalone
path claims to understand, then verifies the emitted pseudo IR.  Unknown or
partially preserved clauses cause abstention rather than silent compilation.
"""
from __future__ import annotations
from dataclasses import dataclass, asdict
import re, unicodedata
from typing import Any

@dataclass
class Certification:
    passed: bool
    covered: list[str]
    failures: list[str]
    contract: dict[str, Any]
    def to_dict(self): return asdict(self)

def _fold(s:str)->str:
    s=unicodedata.normalize('NFKD',s)
    s=''.join(c for c in s if not unicodedata.combining(c))
    s=s.lower().replace('°',' ')
    return re.sub(r'\s+',' ',s).strip()

def _seconds(n:int,unit:str)->int:
    u=_fold(unit)
    if u.startswith(('min','m')) and not u.startswith(('ms','mil')): return n*60
    if u.startswith(('hour','hora','h')): return n*3600
    return n

def _has(ir:list[str],pat:str)->bool:
    return any(re.search(pat,x,re.I) for x in ir)

def _quoted_prefix(source:str)->str|None:
    m=re.search(r'(?:prefix|prefixo|prefijo)\s*(?:[:=]|with|as|como|de)?\s*["\']([^"\']{1,32})["\']',source,re.I)
    if m:return m.group(1)
    # "message with prefix 'T='" and close variants
    m=re.search(r'(?:with|com|con)\s+(?:the\s+|o\s+|el\s+)?(?:prefix|prefixo|prefijo)\s*["\']([^"\']{1,32})["\']',source,re.I)
    return m.group(1) if m else None

def extract_contract(source:str)->dict[str,Any]:
    quote_source=source.translate(str.maketrans({"\u2018":"'","\u2019":"'","\u201c":"\"","\u201d":"\""}))
    t=_fold(quote_source); req=[]; cond=[]; values={}
    def add(name,**kw):
        item={'clause':name}; item.update(kw); req.append(item)

    if re.search(r'\b(?:at boot|on boot|boot|startup|na inicializacao|ao iniciar|en el arranque)\b',t): values['boot']=True
    if re.search(r'(?:disable|turn off|desative|desabilite|desactiva)\s+(?:the\s+)?ndp|ndp\s+(?:off|desligado)',t): add('ndp.off')
    if re.search(r'temperature|temperatura',t): add('temperature.read')
    if re.search(r'battery|bateria',t): add('battery.read')
    if re.search(r'(?:send|transmit|envie|enviar|manda|mande|report|forward)[^.;]{0,30}(?:via|over|por|pelo|pela)\s+(?:the\s+)?lora|\blora\b\s+(?:send|transmit|tx)',t): add('lora.send')
    if re.search(r'(?:enable|start|activate|inicie|iniciar|ative|habilite|inicia|activa)\s+(?:the\s+)?tracking|tracking\s+(?:start|on)',t): add('tracking.start')
    if re.search(r'(?:stop|disable|pare|parar|desative)\s+(?:the\s+)?tracking|tracking\s+(?:stop|off)',t): add('tracking.stop')
    if re.search(r'(?:send|route|envie|encaminhe).{0,40}(?:application|aplicacao|aplicacion|\bapp\b)',t): add('app.send_buffer')
    if re.search(r'(?:debug|diagnostic)',t) and re.search(r'send|write|route|envie|escreva',t): add('debug.buffer')
    if re.search(r'\bserial|\buart',t):
        if re.search(r'configure|configurar|baud|tx\s*\d+|rx\s*\d+',t): add('serial.config')
        if re.search(r'(?:serial|uart)\s+(?:receive|read|rx|receba|reciba)|(?:receive|read|receba|reciba)\s+(?:from|da|de|de la)\s+(?:the\s+)?(?:serial|uart)',t): add('serial.receive')
    if re.search(r'\bhall\b|\breed\b',t) and re.search(r'wait|esper|aguard|mudanca|change|event',t): add('hall.wait')
    if re.search(r'\bfall\b|queda',t) and re.search(r'wait|when|quando|event|detect',t): add('fall.event')
    if re.search(r'minimum[- ]power|minimo consumo|consumo minimo|minimum power',t): add('system.minimum_power')
    # Dynamic state clauses must not collapse into static literal transmission.
    has_counter=bool(re.search(r'counter|contador',t))
    if has_counter and re.search(r'increment|incremente|incrementa|aumente|decrement|decremente',t): add('register.add')
    if has_counter and re.search(r'format|forme|formate|prefix|prefixo|seguida|followed|test(?:e)?\s+lora|lora.{0,30}counter|contador.{0,30}lora',t) and re.search(r'lora',t): add('dynamic.lora_buffer')
    if re.search(r'ble|bluetooth|beacon|advertis|anunc',t):
        if re.search(r'advertis|anunc|beacon',t): add('ble.advertising')

    # Explicit period/cadence.
    m=re.search(r'(?:every|a cada|cada|once every)\s*(\d+)\s*(seconds?|secs?|s|minutes?|mins?|m|hours?|h|segundos?|minutos?|horas?)',t)
    if m:
        sec=_seconds(int(m.group(1)),m.group(2)); values['period_s']=sec
        ctx=t[max(0,m.start()-90):m.start()]
        # Cadence belongs to the closest semantic owner. BLE advertising is autonomous
        # and therefore must NOT be certified as a VM WAIT loop.
        if re.search(r'(?:update|actuali[cz]|atualiz).{0,25}(?:buffer|payload)|(?:buffer|payload).{0,25}(?:update|actuali[cz]|atualiz)',ctx): add('periodic.loop',seconds=sec)
        elif re.search(r'beacon|advertis|anunc',ctx): add('ble.interval',seconds=sec)
        elif re.search(r'tracking|rastreamento|seguimiento',ctx): add('tracking.interval',seconds=sec)
        else: add('periodic.loop',seconds=sec)
    elif re.search(r'continuously|continuamente|repeat|repita|forever|sempre',t):
        add('continuous.loop')

    # Explicit temperature comparisons.  Keep relation/value, not merely presence of CMP.
    patterns=[
      ('GT',r'(?:temperature|temperatura)[^.;,]{0,70}?(?:exceeds?|greater than|above|maior que|acima de|superior a)\s*(-?\d+(?:[.,]\d+)?)'),
      ('LT',r'(?:temperature|temperatura)[^.;,]{0,70}?(?:below|less than|menor que|abaixo de|inferior a)\s*(-?\d+(?:[.,]\d+)?)'),
      ('GT',r'(?:if|when|se|quando)[^.;,]{0,40}?(?:temperature|temperatura)[^.;,]{0,40}?(?:exceeds?|greater than|above|maior que|acima de|superior a)\s*(-?\d+(?:[.,]\d+)?)'),
    ]
    for op,pat in patterns:
        m=re.search(pat,t)
        if m:
            val=float(m.group(1).replace(',','.')); cond.append({'source':'temperature','op':op,'value':val}); add('conditional.temperature',op=op,value=val); break
    # Event-gated conditions are represented natively by WAIT_EVENT, not CMP/JZ.
    motion_event=bool(re.search(r'\b(?:if|when|quando|se|si)\b[^.;]{0,90}\bmotion\b[^.;]{0,45}(?:detect|detected|detection|movimento)',t)
                      or re.search(r'\b(?:if|when|quando|se|si)\b[^.;]{0,90}door\s+(?:opens?|opened|opening)',t))
    vib_event=bool(re.search(r'\b(?:if|when|quando|se|si)\b[^.;]{0,120}(?:vibration|vibracao|vibra)[^.;]{0,80}(?:different|anomal|abnormal|diferent)',t))
    if motion_event: add('motion.event')
    if vib_event: add('vib.alarm.event')
    if re.search(r'home assistant|\bha\b',t) and re.search(r'send|message|notify|notification|envie|mensagem|evento|event',t): add('ha.event')
    nm=re.search(r'(?:beacon\s+named|named\s+beacon|name(?:d)?)[\s:=]*[\'\"]([^\'\"]{1,24})[\'\"]',quote_source,re.I)
    if nm: add('ble.name',value=nm.group(1))

    # Numeric/logical if/else needs CMP/JZ. Event predicates above use WAIT_EVENT.
    if re.search(r'\b(?:if|se|si)\b|\botherwise\b|caso contrario|senao|else\b',t) and not (motion_event or vib_event): add('conditional.branch')

    prefix=_quoted_prefix(quote_source)
    if prefix is not None: values['prefix']=prefix; add('payload.prefix',value=prefix)
    return {'requirements':req,'conditions':cond,'values':values}

def certify(source:str, ir:list[str], schedule:int|None=None, schedule_meta:dict|None=None)->Certification:
    c=extract_contract(source); failures=[]; covered=[]; vals=c['values']
    from nrfclaw_schedule_semantics import detect_schedule_intent, normalize_schedule
    sched_intent=detect_schedule_intent(source)
    def check(name,ok,reason):
        (covered if ok else failures).append(name if ok else reason)

    if vals.get('boot') and not sched_intent:
        check('boot.mode', schedule==4 or _has(ir,r'^PROGRAM BOOT$'), 'missing boot scheduling')

    if sched_intent and sched_intent.get('mode') in {'AT','WEEKLY','BOOT'}:
        try:
            expected=normalize_schedule(sched_intent)
            if sched_intent.get('mode')=='BOOT':
                ok=(schedule==expected['mode'] or _has(ir,r'^PROGRAM BOOT$'))
            else:
                ok=(schedule==expected['mode'] and isinstance(schedule_meta,dict) and schedule_meta.get('mode')==expected['mode'])
            check('schedule.'+sched_intent['mode'].lower(),ok,'missing or incorrect authenticated '+sched_intent['mode']+' schedule metadata')
        except Exception as exc:
            failures.append('schedule lowering failed: '+str(exc))

    for r in c['requirements']:
        name=r['clause']; ok=True; reason='missing '+name
        if name=='ndp.off': ok=_has(ir,r'BLE NDP OFF')
        elif name=='temperature.read': ok=_has(ir,r'^(?:TEMP|DS18) READ')
        elif name=='battery.read': ok=_has(ir,r'^BAT READ')
        elif name=='lora.send': ok=_has(ir,r'^LORA SEND')
        elif name=='tracking.start': ok=_has(ir,r'^TRACKING START')
        elif name=='tracking.stop': ok=_has(ir,r'^TRACKING STOP')
        elif name=='app.send_buffer': ok=_has(ir,r'^APP SEND BUFFER')
        elif name=='debug.buffer': ok=_has(ir,r'^DEBUG BUFFER')
        elif name=='serial.config': ok=_has(ir,r'^SERIAL CONFIG')
        elif name=='serial.receive': ok=_has(ir,r'^SERIAL RX')
        elif name=='hall.wait': ok=_has(ir,r'^(?:HALL WAIT|WAIT_HALL)')
        elif name=='fall.event': ok=_has(ir,r'^WAIT_EVENT FALL')
        elif name=='system.minimum_power': ok=_has(ir,r'^SYSTEM MINIMUM POWER')
        elif name=='register.add': ok=_has(ir,r'^ADD '); reason='missing counter increment/register.add'
        elif name=='dynamic.lora_buffer': ok=_has(ir,r'^FORMAT BUFFER') and _has(ir,r'^LORA SEND BUFFER'); reason='dynamic value collapsed to static LoRa literal'
        elif name=='ble.advertising': ok=_has(ir,r'^BLE (?:ADVERTISER|ROLE BEACON|ADV START|START|ADV BUFFER)')
        elif name=='ble.interval':
            ms=int(r['seconds'])*1000; ok=_has(ir,rf'^BLE (?:ADV )?INTERVAL(?:=|\s+){ms}ms$')
            reason=f'missing BLE advertising interval {ms}ms'
        elif name=='tracking.interval':
            ms=int(r['seconds'])*1000; ok=_has(ir,rf'^TRACKING (?:START|CONFIG).*{ms}|^TRACKING INTERVAL={ms}ms')
            reason=f'missing tracking interval {ms}ms'
        elif name=='periodic.loop':
            sec=int(r['seconds']); ok=_has(ir,rf'^WAIT\s+{sec}s$') and _has(ir,r'^JMP ')
            reason=f'missing periodic loop/wait {sec}s'
        elif name=='continuous.loop': ok=_has(ir,r'^JMP '); reason='missing continuous CFG back-edge'
        elif name=='motion.event': ok=_has(ir,r'^WAIT_EVENT MOTION'); reason='missing WAIT_EVENT MOTION event gate'
        elif name=='vib.alarm.event': ok=_has(ir,r'^WAIT_EVENT VIB_AUTO[.]ALARM'); reason='missing WAIT_EVENT VIB_AUTO.ALARM event gate'
        elif name=='ha.event': ok=_has(ir,r'^HA EVENT '); reason='missing Home Assistant event emission'
        elif name=='ble.name':
            v=re.escape(str(r['value'])); ok=_has(ir,rf'^BLE NAME ["\']{v}["\']$'); reason=f'missing BLE beacon name {r["value"]!r}'
        elif name=='conditional.branch': ok=_has(ir,r'^CMP ') and _has(ir,r'^(?:IF .* GOTO|JZ )'); reason='missing conditional compare/branch'
        elif name=='conditional.temperature':
            op=r['op']; val=float(r['value']);
            cmp_ok=_has(ir,rf'^CMP .*\b{op}\b')
            # Current temperature lowerers use either degrees or millicelsius constants.
            candidates={int(round(val)),int(round(val*1000))}
            mov_ok=False
            for line in ir:
                m=re.search(r'(?:MOVI?\s+R\d+\s*(?:=\s*)?|MOV\s+R\d+\s*=\s*)(-?\d+)',line,re.I)
                if m and int(m.group(1)) in candidates: mov_ok=True; break
            ok=cmp_ok and mov_ok and _has(ir,r'^(?:IF .* GOTO|JZ )')
            reason=f'missing temperature condition {op} {val:g}'
        elif name=='payload.prefix':
            p=re.escape(str(r['value'])); ok=_has(ir,rf'(?:FORMAT BUFFER|BUFFER (?:SET|PREPEND)).*["\']?{p}')
            reason=f'missing payload prefix {r["value"]!r}'
        check(name,ok,reason)

    # Full-clause coverage guard: an explicit "then <action>" must not disappear.
    # We rely on the concrete action checks above; if a then-clause names an unsupported
    # domain, abstain rather than silently ignoring it.
    t=_fold(source)
    for m in re.finditer(r'\b(?:then|entao|então|depois|luego)\s+([^.;]+)',source,re.I):
        frag=_fold(m.group(1))
        known=bool(re.search(r'lora|tracking|ble|bluetooth|gpio|temperature|temperatura|battery|bateria|serial|uart|app|application|debug|wait|aguard|esper|fall|queda|continu|monitor|repeat|repit|stop|disable|enable|start',frag))
        if not known and len(frag.split())>=2:
            failures.append('uncovered then-clause: '+m.group(1).strip())

    return Certification(not failures,covered,failures,c)
