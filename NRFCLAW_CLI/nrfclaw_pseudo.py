#!/usr/bin/env python3
"""R3.8.16b1 nRFClaw pseudo-code compiler.

Human-readable, deterministic intermediate language.  Natural-language/AI
frontends may emit this language, but only this local compiler can emit VM
bytecode.  Unknown statements are rejected rather than approximated.
"""
from __future__ import annotations
import re
import struct
from typing import Iterable
from nrfclaw_text_compiler import Assembler, CompileResult, TextCompileError, SCHED_BOOT, SCHED_MANUAL
from nrfclaw_hybrid_semantic import (
    OP_END, OP_MOVI, OP_JMP, OP_JZ, OP_ADD, OP_SUB, OP_CMP_LT, OP_CMP_GT, OP_CMP_EQ,
    OP_WAIT_S, OP_DS18_READ, OP_BAT_READ, OP_GPIO_WRITE_IMM, OP_GPIO_READ,
    OP_BLE_APP_ROLE, BLE_ROLE_OFF, BLE_ROLE_ADVERTISER, OP_BLE_ADV_CONFIG,
    OP_BLE_ADV_START, OP_BLE_ADV_NAME, OP_BLE_ADV_BUF, OP_TRACKING_CONFIG, OP_TRACKING_START,
    OP_TRACKING_STOP, OP_TRACKING_ROTATION_SET, OP_LORA_CONFIG,
    OP_LORA_SEND_BYTES, OP_LORA_SEND_BUF, OP_LORA_RX_START,
    OP_SERIAL_CONFIG, OP_SERIAL_WRITE_BUF, OP_FORMAT_REG, OP_FORMAT_2REG, OP_PARSE_BUF_I32, OP_PARSE_BUF_FIXED, OP_FORMAT_REG_FIXED, OP_BUFFER_PREPEND, OP_BUFFER_SET, OP_WAIT_EVENT,
    OP_VIB_AUTO_START, OP_VIB_AUTO_STOP, OP_VIB_AUTO_CONFIG_TIME, OP_SYSTEM_MIN_POWER, OP_SERIAL_RX_BUF, OP_APP_SEND_BUF, OP_SERIAL_CONFIG_ECO, OP_WAIT_HALL, OP_APP_EVENT_SEND, OP_DEBUG_BUFFER,
    EVENT_ACCEL_MOTION, EVENT_VIB_MACHINE_ON, EVENT_VIB_WARNING,
    EVENT_VIB_ALARM, EVENT_VIB_MACHINE_OFF,
    FMT_U32, FMT_I32, FMT_CENTIVOLTS, FMT_MILLICELSIUS, FMT_U32_02,
)

# R3.8.20b: VM primitives that existed in firmware but were not yet exposed by pseudo.
OP_MOV = 0x01
OP_ACCEL_CONFIG = 0x5C
EVENT_ACCEL_TAP = 23
EVENT_ACCEL_FALL = 24
EVENT_ACCEL_WALK = 25

GRAMMAR = r'''nRFClaw pseudo-code (one statement per line):
  PROGRAM BOOT|MANUAL
  LABEL <name>
  END
  WAIT <N>s
  JMP <label>
  MOV R<n> = <integer>
  MOV R<dst> = R<src>
  ADD R<dst> = R<a> + R<b>
  SUB R<dst> = R<a> - R<b>
  IF R<n> == 0 GOTO <label>
  CMP R<dst> = R<a> LT|GT|EQ R<b>
  BAT READ -> R<n>
  TEMP READ -> R<n>
  GPIO P0.<pin> HIGH|LOW
  GPIO P0.<pin> READ -> R<n>
  ACCEL CONFIG mode=OFF|MOTION|TAP|FALL|WALK|VIBRATION odr=<Hz> fs=<g> threshold=<mg> duration=<ms> low_power=<0|1>
  SYSTEM MINIMUM POWER
  BLE NDP OFF|ON
  BLE ADVERTISER
  BLE NAME "<name>"
  BLE ADV interval=<ms>ms
  BLE ADV START
  BLE ADV BUFFER
  TRACKING CONFIG interval=<ms>ms tx=<dbm>dBm
  TRACKING ROTATION <seconds>s
  TRACKING START|STOP
  LORA CONFIG freq=<MHz>MHz tx=<dbm>dBm sf=<7..12> bw=<kHz> cr=<1..4>
  LORA SEND "literal"
  LORA SEND BUFFER
  LORA RX -> BUFFER
  SERIAL CONFIG tx=<pin> rx=<pin> baud=<baud> 8N1
  SERIAL CONFIG ECONOMY tx=<pin> rx=<pin> baud=<baud> 8N1
  SERIAL WRITE BUFFER
  SERIAL RX IDLE=<ms>ms -> BUFFER
  SERIAL RX LINE -> BUFFER
  SERIAL RX LENGTH=<1..64> -> BUFFER
  APP SEND BUFFER
  DEBUG BUFFER
  BUFFER PREPEND "literal"
  BUFFER SET "literal"
  PARSE BUFFER INT -> R<n>
  PARSE BUFFER DECIMAL SCALE=<10|100|1000|...> -> R<n>
  FORMAT BUFFER "prefix" + R<n> AS FIXED decimals=<0..6> suffix="..."
  FORMAT BUFFER "prefix" + R<n> AS U32|U32_02|I32|VOLTS|CELSIUS
  FORMAT BUFFER "p1" + R<n> AS <fmt> + "p2" + R<n> AS <fmt>
  VIB_AUTO CONFIG learning=<seconds>s initial=<seconds>s
  VIB_AUTO START|STOP
  HA EVENT cap=<u8> op=<u8> value=R<n>
  WAIT_EVENT MOTION|TAP|FALL|WALK|VIB_AUTO.MACHINE_ON|VIB_AUTO.WARNING|VIB_AUTO.ALARM|VIB_AUTO.MACHINE_OFF
  WAIT_HALL -> R<n>
'''

_EVENT = {
    'MOTION': EVENT_ACCEL_MOTION,
    'TAP': EVENT_ACCEL_TAP,
    'FALL': EVENT_ACCEL_FALL,
    'WALK': EVENT_ACCEL_WALK,
    'VIB_AUTO.MACHINE_ON': EVENT_VIB_MACHINE_ON,
    'VIB_AUTO.WARNING': EVENT_VIB_WARNING,
    'VIB_AUTO.ALARM': EVENT_VIB_ALARM,
    'VIB_AUTO.MACHINE_OFF': EVENT_VIB_MACHINE_OFF,
}
_FMT = {'U32':FMT_U32,'U32_02':FMT_U32_02,'I32':FMT_I32,'VOLTS':FMT_CENTIVOLTS,'CELSIUS':FMT_MILLICELSIUS}

def _clean_lines(text: str) -> list[str]:
    out=[]
    for raw in text.splitlines():
        line=raw.strip()
        # Canonical pseudo-code may include an explanatory '; comment'.
        # Semicolons inside quoted strings are preserved.
        if ';' in line:
            in_q=False; q=''; cut=None
            for i,ch in enumerate(line):
                if ch in ('\"', "'"):
                    if not in_q: in_q=True; q=ch
                    elif q==ch: in_q=False
                elif ch==';' and not in_q:
                    cut=i; break
            if cut is not None: line=line[:cut].rstrip()
        if not line or line.startswith('#') or line.startswith('//'): continue
        # strip numeric list prefixes and indentation markers
        line=re.sub(r'^[-*]\s*','',line)
        out.append(line)
    return out

def compile_pseudo(text: str, force_boot: bool=False) -> CompileResult:
    lines=_clean_lines(text)
    if not lines: raise TextCompileError('empty pseudo-code')
    schedule=SCHED_BOOT if force_boot else SCHED_MANUAL
    a=Assembler(); ir=[]
    for idx,line in enumerate(lines,1):
        s=line.strip(); u=s.upper()
        m=re.fullmatch(r'PROGRAM\s+(BOOT|MANUAL)',u)
        if m:
            schedule=SCHED_BOOT if m.group(1)=='BOOT' else SCHED_MANUAL; ir.append(f'PROGRAM {m.group(1)}'); continue
        m=re.fullmatch(r'LABEL\s+([A-Za-z_][A-Za-z0-9_]*)',s,re.I)
        if m: a.label(m.group(1)); ir.append(f'LABEL {m.group(1)}'); continue
        if u=='END': a.emit(OP_END); ir.append('END'); continue
        m=re.fullmatch(r'WAIT\s+(\d+)\s*s',s,re.I)
        if m: a.wait_s(int(m.group(1))); ir.append(f'WAIT {m.group(1)}s'); continue
        m=re.fullmatch(r'JMP\s+([A-Za-z_][A-Za-z0-9_]*)',s,re.I)
        if m: a.jmp(m.group(1)); ir.append(f'JMP {m.group(1)}'); continue
        m=re.fullmatch(r'MOV\s+R([0-7])\s*=\s*(-?\d+)',s,re.I)
        if m: a.movi(int(m.group(1)),int(m.group(2))); ir.append(s); continue
        m=re.fullmatch(r'MOV\s+R([0-7])\s*=\s*R([0-7])',s,re.I)
        if m: a.emit(OP_MOV,int(m.group(1)),int(m.group(2))); ir.append(s); continue
        m=re.fullmatch(r'ADD\s+R([0-7])\s*=\s*R([0-7])\s*\+\s*R([0-7])',s,re.I)
        if m: a.emit(OP_ADD,int(m.group(1)),int(m.group(2)),int(m.group(3))); ir.append(s); continue
        m=re.fullmatch(r'SUB\s+R([0-7])\s*=\s*R([0-7])\s*-\s*R([0-7])',s,re.I)
        if m: a.emit(OP_SUB,int(m.group(1)),int(m.group(2)),int(m.group(3))); ir.append(s); continue
        m=re.fullmatch(r'IF\s+R([0-7])\s*==\s*0\s+GOTO\s+([A-Za-z_][A-Za-z0-9_]*)',s,re.I)
        if m: a.jz(int(m.group(1)),m.group(2)); ir.append(s); continue
        m=re.fullmatch(r'CMP\s+R([0-7])\s*=\s*R([0-7])\s+(LT|GT|EQ)\s+R([0-7])',s,re.I)
        if m:
            op={'LT':OP_CMP_LT,'GT':OP_CMP_GT,'EQ':OP_CMP_EQ}[m.group(3).upper()]
            a.emit(op,int(m.group(1)),int(m.group(2)),int(m.group(4))); ir.append(s); continue
        m=re.fullmatch(r'BAT(?:TERY)?\s+READ\s*->\s*R([0-7])',s,re.I)
        if m: a.emit(OP_BAT_READ,int(m.group(1))); ir.append(s); continue
        m=re.fullmatch(r'(?:TEMP|TEMPERATURE|DS18)\s+READ\s*->\s*R([0-7])',s,re.I)
        if m: a.emit(OP_DS18_READ,int(m.group(1))); ir.append(s); continue
        m=re.fullmatch(r'GPIO\s+P0[.]?(\d{1,2})\s+(HIGH|LOW)',s,re.I)
        if m: a.emit(OP_GPIO_WRITE_IMM,int(m.group(1)),1 if m.group(2).upper()=='HIGH' else 0); ir.append(s); continue
        m=re.fullmatch(r'GPIO\s+P0[.]?(\d{1,2})\s+READ\s*->\s*R([0-7])',s,re.I)
        if m: a.emit(OP_GPIO_READ,int(m.group(1)),int(m.group(2))); ir.append(s); continue
        m=re.fullmatch(r'ACCEL\s+CONFIG\s+mode=(OFF|MOTION|TAP|FALL|WALK|VIBRATION)\s+odr=(\d+)\s+fs=(2|4|8|16)\s+threshold=(\d+)\s+duration=(\d+)\s+low_power=(0|1)',s,re.I)
        if m:
            modes={'OFF':0,'MOTION':1,'TAP':2,'FALL':3,'WALK':4,'VIBRATION':5}
            mode=modes[m.group(1).upper()]; odr=int(m.group(2)); fs=int(m.group(3)); th=int(m.group(4)); dur=int(m.group(5)); lp=int(m.group(6))
            if odr not in (1,10,25,50,100,200,400): raise TextCompileError(f'line {idx}: invalid ACCEL ODR')
            if not 0<=th<=65535 or not 0<=dur<=65535: raise TextCompileError(f'line {idx}: invalid ACCEL threshold/duration')
            a.emit(OP_ACCEL_CONFIG,mode); a.u16(odr); a.emit(fs); a.u16(th); a.u16(dur); a.emit(lp); ir.append(s); continue
        if u=='SYSTEM MINIMUM POWER': a.emit(OP_SYSTEM_MIN_POWER); ir.append(s); continue
        if u=='BLE NDP OFF': a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF); ir.append(s); continue
        if u=='BLE NDP ON': a.emit(OP_BLE_APP_ROLE,2); ir.append(s); continue
        if u=='BLE ADVERTISER': a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER); ir.append(s); continue
        m=re.fullmatch(r'BLE\s+NAME\s+\"(.*)\"',s,re.I)
        if m:
            raw=m.group(1).encode('utf-8')
            if not 1<=len(raw)<=24: raise TextCompileError(f'line {idx}: BLE name must be 1..24 bytes')
            a.emit(OP_BLE_ADV_NAME,len(raw)); a.code+=raw; ir.append(s); continue
        m=re.fullmatch(r'BLE\s+ADV\s+INTERVAL\s*=\s*(\d+)\s*ms',s,re.I)
        if m:
            ms=int(m.group(1)); a.emit(OP_BLE_ADV_CONFIG); a.u16(ms); a.emit(4 & 0xff, 0); ir.append(s); continue
        if u=='BLE ADV START': a.emit(OP_BLE_ADV_START); ir.append(s); continue
        if u=='BLE ADV BUFFER': a.emit(OP_BLE_ADV_BUF); ir.append(s); continue
        m=re.fullmatch(r'TRACKING\s+CONFIG\s+interval\s*=\s*(\d+)\s*ms(?:\s+tx\s*=\s*([+-]?\d+)\s*dBm)?',s,re.I)
        if m:
            ms=int(m.group(1)); tx=int(m.group(2) or 4); a.emit(OP_TRACKING_CONFIG); a.u16(ms); a.emit(tx & 0xff); ir.append(s); continue
        m=re.fullmatch(r'TRACKING\s+ROTATION\s+(\d+)\s*s',s,re.I)
        if m: a.emit(OP_TRACKING_ROTATION_SET); a.u32(int(m.group(1))); ir.append(s); continue
        if u=='TRACKING START': a.emit(OP_TRACKING_START); ir.append(s); continue
        if u=='TRACKING STOP': a.emit(OP_TRACKING_STOP); ir.append(s); continue
        m=re.fullmatch(r'LORA\s+CONFIG\s+freq\s*=\s*(\d+(?:[.]\d+)?)\s*MHz\s+tx\s*=\s*([+-]?\d+)\s*dBm\s+(?:sf\s*=\s*|SF)(\d+)\s+(?:bw\s*=\s*|BW)(\d+)\s+(?:cr\s*=\s*(\d+)|CR4/(\d+))',s,re.I)
        if m:
            hz=int(round(float(m.group(1))*1_000_000)); tx=int(m.group(2)); sf=int(m.group(3)); bw=int(m.group(4)); cr=int(m.group(5) or (int(m.group(6))-4))
            if not 7<=sf<=12: raise TextCompileError(f'line {idx}: LoRa SF must be 7..12')
            a.emit(OP_LORA_CONFIG); a.u32(hz); a.emit(tx & 0xff,sf); a.u16(bw); a.emit(cr); ir.append(s); continue
        m=re.fullmatch(r'LORA\s+SEND\s+"(.*)"',s,re.I)
        if m:
            raw=m.group(1).encode('utf-8');
            if not 1<=len(raw)<=32: raise TextCompileError(f'line {idx}: LoRa literal must be 1..32 bytes')
            a.emit(OP_LORA_SEND_BYTES,len(raw)); a.code+=raw; ir.append(s); continue
        if u=='LORA SEND BUFFER': a.emit(OP_LORA_SEND_BUF); ir.append(s); continue
        if u in ('LORA RX -> BUFFER','LORA RECEIVE -> BUFFER'): a.emit(OP_LORA_RX_START,1); ir.append(s); continue
        m=re.fullmatch(r'SERIAL\s+CONFIG\s+ECONOMY\s+tx\s*=\s*(?:D)?(\d+)\s+rx\s*=\s*(?:D)?(\d+)\s+(?:baud\s*=\s*)?(\d+)\s+8N1',s,re.I)
        if m:
            a.emit(OP_SERIAL_CONFIG_ECO,int(m.group(1)),int(m.group(2))); a.u32(int(m.group(3))); ir.append(s); continue
        m=re.fullmatch(r'SERIAL\s+CONFIG\s+tx\s*=\s*(?:D)?(\d+)\s+rx\s*=\s*(?:D)?(\d+)\s+(?:baud\s*=\s*)?(\d+)\s+8N1',s,re.I)
        if m:
            a.emit(OP_SERIAL_CONFIG,int(m.group(1)),int(m.group(2))); a.u32(int(m.group(3))); ir.append(s); continue
        if u=='SERIAL WRITE BUFFER': a.emit(OP_SERIAL_WRITE_BUF); ir.append(s); continue
        m=re.fullmatch(r'SERIAL\s+RX\s+IDLE\s*=\s*(\d+)\s*ms\s*->\s*BUFFER',s,re.I)
        if m:
            ms=int(m.group(1))
            if not 1<=ms<=60000: raise TextCompileError(f'line {idx}: SERIAL idle must be 1..60000 ms')
            a.emit(OP_SERIAL_RX_BUF,0); a.u16(ms); ir.append(s); continue
        if re.fullmatch(r'SERIAL\s+RX\s+LINE\s*->\s*BUFFER',s,re.I):
            a.emit(OP_SERIAL_RX_BUF,1); a.u16(0); ir.append(s); continue
        m=re.fullmatch(r'SERIAL\s+RX\s+LENGTH\s*=\s*(\d+)\s*->\s*BUFFER',s,re.I)
        if m:
            n=int(m.group(1))
            if not 1<=n<=64: raise TextCompileError(f'line {idx}: SERIAL length must be 1..64')
            a.emit(OP_SERIAL_RX_BUF,2); a.u16(n); ir.append(s); continue
        if u=='APP SEND BUFFER': a.emit(OP_APP_SEND_BUF); ir.append(s); continue
        if u=='DEBUG BUFFER': a.emit(OP_DEBUG_BUFFER); ir.append(s); continue
        m=re.fullmatch(r'BUFFER\s+PREPEND\s+"([^"]+)"',s,re.I)
        if m:
            raw=m.group(1).encode('utf-8')
            if not (1 <= len(raw) <= 48): raise TextCompileError('BUFFER PREPEND literal must be 1..48 UTF-8 bytes')
            a.emit(OP_BUFFER_PREPEND,len(raw)); a.code += raw; ir.append(s); continue
        m=re.fullmatch(r'BUFFER\s+SET\s+"([^"]+)"',s,re.I)
        if m:
            raw=m.group(1).encode('utf-8')
            if not (1 <= len(raw) <= 64): raise TextCompileError('BUFFER SET literal must be 1..64 UTF-8 bytes')
            a.emit(OP_BUFFER_SET,len(raw)); a.code += raw; ir.append(s); continue
        m=re.fullmatch(r'PARSE\s+BUFFER\s+INT\s*->\s*R([0-7])',s,re.I)
        if m: a.emit(OP_PARSE_BUF_I32,int(m.group(1))); ir.append(s); continue
        m=re.fullmatch(r'PARSE\s+BUFFER\s+DECIMAL\s+SCALE\s*=\s*(1|10|100|1000|10000|100000|1000000)\s*->\s*R([0-7])',s,re.I)
        if m:
            scale=int(m.group(1)); decimals=len(str(scale))-1
            a.emit(OP_PARSE_BUF_FIXED,int(m.group(2)),decimals); ir.append(s); continue
        m=re.fullmatch(r'FORMAT\s+BUFFER\s+"(.*?)"\s*\+\s*R([0-7])\s+AS\s+FIXED\s+decimals\s*=\s*([0-6])(?:\s+suffix\s*=\s*"(.*?)")?',s,re.I)
        if m:
            pre=m.group(1).encode('utf-8'); suf=(m.group(4) or '').encode('utf-8'); dec=int(m.group(3))
            if len(pre)>48 or len(suf)>16: raise TextCompileError(f'line {idx}: compact FORMAT prefix/suffix too long')
            a.emit(OP_FORMAT_REG_FIXED,len(pre)); a.code+=pre; a.emit(int(m.group(2)),dec,len(suf)); a.code+=suf; ir.append(s); continue
        m=re.fullmatch(r'FORMAT\s+BUFFER\s+"(.*?)"\s*\+\s*R([0-7])\s+AS\s+(U32|U32_02|I32|VOLTS|CELSIUS)\s*\+\s*"(.*?)"\s*\+\s*R([0-7])\s+AS\s+(U32|U32_02|I32|VOLTS|CELSIUS)',s,re.I)
        if m:
            p1=m.group(1).encode('utf-8'); p2=m.group(4).encode('utf-8')
            if len(p1)>32 or len(p2)>32: raise TextCompileError(f'line {idx}: FORMAT_2REG prefix >32 bytes')
            a.emit(OP_FORMAT_2REG,len(p1)); a.code+=p1; a.emit(int(m.group(2)),_FMT[m.group(3).upper()],len(p2)); a.code+=p2; a.emit(int(m.group(5)),_FMT[m.group(6).upper()]); ir.append(s); continue
        m=re.fullmatch(r'FORMAT\s+BUFFER\s+"(.*)"\s*\+\s*R([0-7])\s+AS\s+(U32|U32_02|I32|VOLTS|CELSIUS)',s,re.I)
        if m:
            raw=m.group(1).encode('utf-8');
            if len(raw)>48: raise TextCompileError(f'line {idx}: FORMAT prefix >48 bytes')
            a.emit(OP_FORMAT_REG,len(raw)); a.code+=raw; a.emit(int(m.group(2)),_FMT[m.group(3).upper()]); ir.append(s); continue
        m=re.fullmatch(r'VIB_AUTO CONFIG\s+LEARNING=(\d+)S\s+INITIAL=(\d+)S',u)
        if m:
            learning=int(m.group(1)); initial=int(m.group(2))
            if not 60<=learning<=604800 or not 0<=initial<=604800: raise TextCompileError('VIB_AUTO CONFIG time out of range')
            a.emit(OP_VIB_AUTO_CONFIG_TIME); a.code += struct.pack('<II',learning,initial); ir.append(s); continue
        if u=='VIB_AUTO START': a.emit(OP_VIB_AUTO_START,0); ir.append(s); continue
        if u=='VIB_AUTO STOP': a.emit(OP_VIB_AUTO_STOP); ir.append(s); continue
        m=re.fullmatch(r'HA\s+EVENT\s+cap\s*=\s*(\d+)\s+op\s*=\s*(\d+)\s+value\s*=\s*R([0-7])',s,re.I)
        if m:
            cap,op,reg=map(int,m.groups())
            if not (0<=cap<=255 and 0<=op<=255): raise TextCompileError(f'line {idx}: HA cap/op must be 0..255')
            a.emit(OP_APP_EVENT_SEND,cap,op,reg); ir.append(s); continue
        m=re.fullmatch(r'WAIT_EVENT\s+(.+)',s,re.I)
        if m:
            ev=m.group(1).upper();
            if ev not in _EVENT: raise TextCompileError(f'line {idx}: unknown event {m.group(1)!r}')
            a.emit(OP_WAIT_EVENT,_EVENT[ev],6,7); ir.append(s); continue
        m=re.fullmatch(r'WAIT_HALL\s*->\s*R([0-7])',s,re.I)
        if m: a.emit(OP_WAIT_HALL,int(m.group(1))); ir.append(s); continue
        raise TextCompileError(f'line {idx}: unsupported pseudo statement: {line!r}')
    bytecode=a.finish()
    return CompileResult(text,'pseudo',schedule,ir,bytecode,[])

def pseudo_from_result(result: CompileResult) -> str:
    lines=[f'PROGRAM {result.schedule_name}']
    for x in result.ir:
        if x.upper().startswith('PROGRAM '): continue
        lines.append(x)
    return '\n'.join(lines)
