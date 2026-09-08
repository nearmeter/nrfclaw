#!/usr/bin/env python3
"""nRFClaw R3.8.20b Semantic IR — typed, versioned trust boundary.
Agents emit semantic operations only. Local validation/lowering emits pseudo-code;
only the deterministic pseudo compiler emits VM bytecode.
"""
from __future__ import annotations
from copy import deepcopy
from typing import Any
from nrfclaw_pseudo import compile_pseudo
from nrfclaw_text_compiler import CompileResult, TextCompileError
from nrfclaw_schedule_semantics import normalize_schedule
IR_VERSION=1; VM_ABI=1
STATUSES={"COMPILE","MISSING_CAPABILITY","AMBIGUOUS","RESOURCE_CONFLICT"}; LANGUAGES={"pt","en","es","other"}
FORMATS={"U32","U32_02","I32","VOLTS","CELSIUS"}; EVENTS={"MOTION","TAP","FALL","WALK","VIB_AUTO.MACHINE_ON","VIB_AUTO.WARNING","VIB_AUTO.ALARM","VIB_AUTO.MACHINE_OFF"}
class SemanticIRError(ValueError): pass
class SemanticIRMissingCapability(SemanticIRError):
    def __init__(self,capability,detail): super().__init__(detail); self.capability=capability; self.detail=detail

def schema_contract():
    # Model-facing root shape. Keep ONLY fields accepted by validate_ir().
    # Explanatory rules/reference metadata belong in the system prompt, never
    # inside the example object: models tend to copy example keys literally.
    return {
      "ir_version":1,
      "status":"COMPILE|MISSING_CAPABILITY|AMBIGUOUS|RESOURCE_CONFLICT",
      "language":"pt|en|es|other",
      "program":{"mode":"MANUAL|BOOT|AT|EVERY|WEEKLY","schedule":{"fields":"mode-specific schedule metadata"},"actions":["typed action"]},
      "unresolved_clauses":[],
      "missing_capabilities":[],
      "notes":[]
    }

def _obj(v,n):
    if not isinstance(v,dict): raise SemanticIRError(f"{n} must be an object")
    return v
def _list(v,n):
    if not isinstance(v,list): raise SemanticIRError(f"{n} must be an array")
    return v
def _int(v,n,lo,hi):
    if isinstance(v,bool) or not isinstance(v,(int,float)) or int(v)!=v: raise SemanticIRError(f"{n} must be an integer")
    v=int(v)
    if not lo<=v<=hi: raise SemanticIRError(f"{n} must be {lo}..{hi}")
    return v
def _txt(v,n,maxb,minb=0):
    if not isinstance(v,str): raise SemanticIRError(f"{n} must be a string")
    b=v.encode();
    if not minb<=len(b)<=maxb or any(c in v for c in '\r\n"'): raise SemanticIRError(f"{n} invalid or outside {minb}..{maxb} UTF-8 bytes")
    return v
def _only(a,allowed,p):
    x=set(a)-allowed
    if x: raise SemanticIRError(f"{p}: unknown fields: {', '.join(sorted(x))}")
def _reg(v,n): return _int(v,n,0,7)
def _actions(actions,where):
    for i,a0 in enumerate(actions):
        a=_obj(a0,f'{where}[{i}]'); p=f'{where}[{i}]'; op=a.get('op')
        if not isinstance(op,str): raise SemanticIRError(f'{p}.op must be string')
        noarg={"system.minimum_power","ble.adv_start","ble.advertise_buffer","lora.send_buffer","debug.buffer","serial.write_buffer","app.send_buffer","tracking.start","tracking.stop","vib_auto.start","vib_auto.stop"}
        if op in noarg: _only(a,{"op"},p)
        elif op=="label": _only(a,{"op","name"},p); _txt(a.get('name'),p+'.name',32,1)
        elif op=="jump": _only(a,{"op","label"},p); _txt(a.get('label'),p+'.label',32,1)
        elif op=="jump_if_zero": _only(a,{"op","register","label"},p); _reg(a.get('register'),p+'.register'); _txt(a.get('label'),p+'.label',32,1)
        elif op=="register.copy": _only(a,{"op","dst","src"},p); _reg(a.get('dst'),p+'.dst'); _reg(a.get('src'),p+'.src')
        elif op=="register.set": _only(a,{"op","register","value"},p); _reg(a.get('register'),p+'.register'); _int(a.get('value'),p+'.value',-2147483648,2147483647)
        elif op in {"register.add","register.sub"}: _only(a,{"op","dst","a","b"},p); [_reg(a.get(k),p+'.'+k) for k in ('dst','a','b')]
        elif op=="register.compare": _only(a,{"op","dst","a","b","condition"},p); [_reg(a.get(k),p+'.'+k) for k in ('dst','a','b')];
        elif op=="wait": _only(a,{"op","seconds"},p); _int(a.get('seconds'),p+'.seconds',1,0xffffffff)
        elif op=="gpio.read": _only(a,{"op","pin","register"},p); _int(a.get('pin'),p+'.pin',0,31); _reg(a.get('register'),p+'.register')
        elif op=="gpio.write": _only(a,{"op","pin","value"},p); _int(a.get('pin'),p+'.pin',0,31); _int(a.get('value'),p+'.value',0,1)
        elif op=="accel.config":
            _only(a,{"op","mode","odr_hz","full_scale_g","threshold_mg","duration_ms","low_power"},p)
            if a.get('mode') not in {"OFF","MOTION","TAP","FALL","WALK","VIBRATION"}: raise SemanticIRError(p+'.mode invalid')
            if a.get('odr_hz',10) not in {1,10,25,50,100,200,400}: raise SemanticIRError(p+'.odr_hz invalid')
            if a.get('full_scale_g',2) not in {2,4,8,16}: raise SemanticIRError(p+'.full_scale_g invalid')
            _int(a.get('threshold_mg',120),p+'.threshold_mg',0,65535); _int(a.get('duration_ms',100),p+'.duration_ms',0,65535)
            if not isinstance(a.get('low_power',True),bool): raise SemanticIRError(p+'.low_power boolean required')
        elif op=="wait.event": _only(a,{"op","event"},p); 
        elif op in {"battery.read","temperature.read","hall.wait"}: _only(a,{"op","register"},p); _reg(a.get('register'),p+'.register')
        elif op=="ble.ndp": _only(a,{"op","enabled"},p); 
        elif op=="ble.advertiser": _only(a,{"op","interval_ms"},p); _int(a.get('interval_ms',1000),p+'.interval_ms',20,10240)
        elif op=="ble.name": _only(a,{"op","value"},p); _txt(a.get('value'),p+'.value',24,1)
        elif op=="lora.config":
            _only(a,{"op","frequency_mhz","power_dbm","sf","bw_khz","cr"},p); fv=a.get('frequency_mhz',915)
            if not isinstance(fv,(int,float)) or not 100<=fv<=1000: raise SemanticIRError(p+'.frequency_mhz invalid')
            _int(a.get('power_dbm',14),p+'.power_dbm',-9,22); _int(a.get('sf',7),p+'.sf',7,12); _int(a.get('bw_khz',125),p+'.bw_khz',1,500); _int(a.get('cr',1),p+'.cr',1,4)
        elif op=="lora.receive": _only(a,{"op","target"},p); 
        elif op=="lora.send_literal": _only(a,{"op","value"},p); _txt(a.get('value'),p+'.value',32,1)
        elif op=="serial.config": _only(a,{"op","tx_pin","rx_pin","baud","economy"},p); _int(a.get('tx_pin'),p+'.tx_pin',0,31); _int(a.get('rx_pin'),p+'.rx_pin',0,31); _int(a.get('baud'),p+'.baud',300,1000000)
        elif op=="serial.receive": _only(a,{"op","mode","value"},p); 
        elif op=="buffer.prepend": _only(a,{"op","value"},p); _txt(a.get('value'),p+'.value',48,1)
        elif op=="buffer.set_literal": _only(a,{"op","value"},p); _txt(a.get('value'),p+'.value',64,1)
        elif op=="buffer.parse_int": _only(a,{"op","register"},p); _reg(a.get('register'),p+'.register')
        elif op=="buffer.parse_fixed": _only(a,{"op","register","scale"},p); _reg(a.get('register'),p+'.register');
        elif op=="buffer.format_register": _only(a,{"op","prefix","register","format"},p); _txt(a.get('prefix',''),p+'.prefix',48); _reg(a.get('register'),p+'.register')
        elif op=="buffer.format_fixed": _only(a,{"op","prefix","register","decimals","suffix"},p); _txt(a.get('prefix',''),p+'.prefix',48); _reg(a.get('register'),p+'.register'); _int(a.get('decimals'),p+'.decimals',0,6); _txt(a.get('suffix',''),p+'.suffix',16)
        elif op=="buffer.format_2reg": _only(a,{"op","prefix1","reg1","format1","prefix2","reg2","format2"},p); _txt(a.get('prefix1',''),p+'.prefix1',32); _txt(a.get('prefix2',''),p+'.prefix2',32); _reg(a.get('reg1'),p+'.reg1'); _reg(a.get('reg2'),p+'.reg2')
        elif op=="vib_auto.config": _only(a,{"op","learning_s","initial_s"},p); _int(a.get('learning_s'),p+'.learning_s',60,604800); _int(a.get('initial_s'),p+'.initial_s',0,604800)
        elif op=="tracking.config": _only(a,{"op","interval_ms","tx_dbm"},p); _int(a.get('interval_ms'),p+'.interval_ms',20,65535); _int(a.get('tx_dbm',4),p+'.tx_dbm',-40,20)
        elif op=="tracking.rotation": _only(a,{"op","seconds"},p); _int(a.get('seconds'),p+'.seconds',1,0xffffffff)
        elif op=="ha.event": _only(a,{"op","cap","operation","register"},p); _int(a.get('cap'),p+'.cap',0,255); _int(a.get('operation'),p+'.operation',0,255); _reg(a.get('register'),p+'.register')
        elif op=="loop": _only(a,{"op","actions"},p); body=_list(a.get('actions'),p+'.actions'); _actions(body,p+'.actions')
        else: raise SemanticIRError(f'{p}: unknown Semantic IR operation {op!r}')
        if op=="register.compare" and a.get('condition') not in {"LT","GT","EQ"}: raise SemanticIRError(p+'.condition invalid')
        if op=="wait.event" and a.get('event') not in EVENTS: raise SemanticIRError(p+'.event invalid')
        if op=="ble.ndp" and not isinstance(a.get('enabled'),bool): raise SemanticIRError(p+'.enabled boolean required')
        if op=="lora.receive" and a.get('target','buffer')!='buffer': raise SemanticIRError(p+'.target must be buffer')
        if op=="serial.config" and not isinstance(a.get('economy',False),bool): raise SemanticIRError(p+'.economy boolean required')
        if op=="serial.receive":
            if a.get('mode') not in {'LINE','IDLE','LENGTH'}: raise SemanticIRError(p+'.mode invalid')
            if a.get('mode')=='IDLE': _int(a.get('value'),p+'.value',1,60000)
            if a.get('mode')=='LENGTH': _int(a.get('value'),p+'.value',1,64)
        if op=="buffer.parse_fixed" and a.get('scale') not in {1,10,100,1000,10000,100000,1000000}: raise SemanticIRError(p+'.scale invalid')
        if op=="buffer.format_register" and a.get('format') not in FORMATS: raise SemanticIRError(p+'.format invalid')
        if op=="buffer.format_2reg" and (a.get('format1') not in FORMATS or a.get('format2') not in FORMATS): raise SemanticIRError(p+' format invalid')

def validate_ir(ir,require_compile=False):
    ir=deepcopy(_obj(ir,'semantic IR')); allowed={"ir_version","status","language","program","unresolved_clauses","missing_capabilities","notes"}; _only(ir,allowed,'IR')
    if ir.get('ir_version')!=1: raise SemanticIRError('Semantic IR version mismatch')
    if ir.get('status') not in STATUSES: raise SemanticIRError('invalid Semantic IR status')
    ir['language']=ir.get('language','other') if ir.get('language','other') in LANGUAGES else 'other'
    for k in ('unresolved_clauses','missing_capabilities','notes'):
        ir[k]=ir.get(k,[]); _list(ir[k],k)
    if require_compile and ir['status']!='COMPILE': raise SemanticIRError('agent returned '+ir['status'])
    if ir['status']=='COMPILE':
        pr=_obj(ir.get('program'),'program'); _only(pr,{"mode","schedule","actions"},'program')
        mode=pr.get('mode')
        if mode not in {'BOOT','MANUAL','AT','EVERY','WEEKLY'}: raise SemanticIRError('program.mode invalid')
        sch=pr.get('schedule')
        if mode in {'AT','EVERY','WEEKLY'}:
            sch=_obj(sch,'program.schedule')
            try: normalize_schedule({**sch,'mode':mode})
            except Exception as exc: raise SemanticIRError(f'program.schedule invalid: {exc}') from exc
        elif sch not in (None,{}):
            _obj(sch,'program.schedule')
        acts=_list(pr.get('actions'),'program.actions');
        if not acts: raise SemanticIRError('program.actions empty')
        _actions(acts,'program.actions')
    return ir

def ir_to_pseudo(ir):
    ir=validate_ir(ir,True); mode=ir['program']['mode']; lines=[f"PROGRAM {'BOOT' if mode=='BOOT' else 'MANUAL'}"]; lc=[0]
    def low(acts):
        for a in acts:
            op=a['op']
            if op=='system.minimum_power': lines.append('SYSTEM MINIMUM POWER')
            elif op=='label': lines.append('LABEL '+a['name'])
            elif op=='jump': lines.append('JMP '+a['label'])
            elif op=='jump_if_zero': lines.append(f"IF R{a['register']} == 0 GOTO {a['label']}")
            elif op=='register.copy': lines.append(f"MOV R{a['dst']} = R{a['src']}")
            elif op=='register.set': lines.append(f"MOV R{a['register']} = {a['value']}")
            elif op=='register.add': lines.append(f"ADD R{a['dst']} = R{a['a']} + R{a['b']}")
            elif op=='register.sub': lines.append(f"SUB R{a['dst']} = R{a['a']} - R{a['b']}")
            elif op=='register.compare': lines.append(f"CMP R{a['dst']} = R{a['a']} {a['condition']} R{a['b']}")
            elif op=='wait': lines.append(f"WAIT {a['seconds']}s")
            elif op=='gpio.read': lines.append(f"GPIO P0.{a['pin']} READ -> R{a['register']}")
            elif op=='gpio.write': lines.append(f"GPIO P0.{a['pin']} {'HIGH' if a['value'] else 'LOW'}")
            elif op=='accel.config': lines.append(f"ACCEL CONFIG mode={a['mode']} odr={a.get('odr_hz',10)} fs={a.get('full_scale_g',2)} threshold={a.get('threshold_mg',120)} duration={a.get('duration_ms',100)} low_power={1 if a.get('low_power',True) else 0}")
            elif op=='wait.event': lines.append('WAIT_EVENT '+a['event'])
            elif op=='battery.read': lines.append(f"BAT READ -> R{a['register']}")
            elif op=='temperature.read': lines.append(f"TEMP READ -> R{a['register']}")
            elif op=='hall.wait': lines.append(f"WAIT_HALL -> R{a['register']}")
            elif op=='ble.ndp': lines.append('BLE NDP '+('ON' if a['enabled'] else 'OFF'))
            elif op=='ble.advertiser': lines.extend(['BLE ADVERTISER',f"BLE ADV INTERVAL={a.get('interval_ms',1000)}ms"])
            elif op=='ble.name': lines.append(f"BLE NAME \"{a['value']}\"")
            elif op=='ble.adv_start': lines.append('BLE ADV START')
            elif op=='ble.advertise_buffer': lines.append('BLE ADV BUFFER')
            elif op=='lora.config': lines.append(f"LORA CONFIG freq={a.get('frequency_mhz',915)}MHz tx={a.get('power_dbm',14)}dBm sf={a.get('sf',7)} bw={a.get('bw_khz',125)} cr={a.get('cr',1)}")
            elif op=='lora.receive': lines.append('LORA RX -> BUFFER')
            elif op=='lora.send_literal': lines.append(f"LORA SEND \"{a['value']}\"")
            elif op=='lora.send_buffer': lines.append('LORA SEND BUFFER')
            elif op=='serial.config': lines.append(f"SERIAL CONFIG {'ECONOMY ' if a.get('economy') else ''}tx={a['tx_pin']} rx={a['rx_pin']} baud={a['baud']} 8N1")
            elif op=='serial.receive': lines.append('SERIAL RX LINE -> BUFFER' if a['mode']=='LINE' else f"SERIAL RX {a['mode']}={a['value']}{'ms' if a['mode']=='IDLE' else ''} -> BUFFER")
            elif op=='serial.write_buffer': lines.append('SERIAL WRITE BUFFER')
            elif op=='app.send_buffer': lines.append('APP SEND BUFFER')
            elif op=='debug.buffer': lines.append('DEBUG BUFFER')
            elif op=='buffer.prepend': lines.append(f"BUFFER PREPEND \"{a['value']}\"")
            elif op=='buffer.set_literal': lines.append(f"BUFFER SET \"{a['value']}\"")
            elif op=='buffer.parse_int': lines.append(f"PARSE BUFFER INT -> R{a['register']}")
            elif op=='buffer.parse_fixed': lines.append(f"PARSE BUFFER DECIMAL SCALE={a['scale']} -> R{a['register']}")
            elif op=='buffer.format_register': lines.append(f"FORMAT BUFFER \"{a.get('prefix','')}\" + R{a['register']} AS {a['format']}")
            elif op=='buffer.format_fixed': lines.append(f"FORMAT BUFFER \"{a.get('prefix','')}\" + R{a['register']} AS FIXED decimals={a['decimals']} suffix=\"{a.get('suffix','')}\"")
            elif op=='buffer.format_2reg': lines.append(f"FORMAT BUFFER \"{a.get('prefix1','')}\" + R{a['reg1']} AS {a['format1']} + \"{a.get('prefix2','')}\" + R{a['reg2']} AS {a['format2']}")
            elif op=='vib_auto.config': lines.append(f"VIB_AUTO CONFIG learning={a['learning_s']}s initial={a['initial_s']}s")
            elif op=='vib_auto.start': lines.append('VIB_AUTO START')
            elif op=='vib_auto.stop': lines.append('VIB_AUTO STOP')
            elif op=='tracking.config': lines.append(f"TRACKING CONFIG interval={a['interval_ms']}ms tx={a.get('tx_dbm',4)}dBm")
            elif op=='tracking.rotation': lines.append(f"TRACKING ROTATION {a['seconds']}s")
            elif op=='tracking.start': lines.append('TRACKING START')
            elif op=='tracking.stop': lines.append('TRACKING STOP')
            elif op=='ha.event': lines.append(f"HA EVENT cap={a['cap']} op={a['operation']} value=R{a['register']}")
            elif op=='loop': lc[0]+=1; lab=f'IR_LOOP_{lc[0]}'; lines.append('LABEL '+lab); low(a['actions']); lines.append('JMP '+lab)
    low(ir['program']['actions']); lines.append('END'); return '\n'.join(lines)
def compile_ir(ir,source=''):
    ir=validate_ir(ir,True)
    pseudo=ir_to_pseudo(ir)
    try: r=compile_pseudo(pseudo)
    except TextCompileError as e: raise SemanticIRError(f'deterministic pseudo compiler rejected Semantic IR lowering: {e}') from e
    mode=ir['program']['mode']; semantic_schedule=dict(ir['program'].get('schedule') or {}); semantic_schedule['mode']=mode
    try: lowered=normalize_schedule(semantic_schedule)
    except Exception as e: raise SemanticIRError(f'schedule lowering failed: {e}') from e
    r.schedule_mode=lowered['mode']; r.schedule=lowered
    if source:r.source=source
    r.intent='semantic-ir'; return r,pseudo
