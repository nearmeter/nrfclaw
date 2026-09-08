#!/usr/bin/env python3
"""R3.8.20c2g3a schedule semantic integration.

The VM bytecode ABI is unchanged. Schedule is a separate authenticated 12-byte
program metadata record matching include/nrfclaw_schedule.h.
"""
from __future__ import annotations
from datetime import datetime, timedelta, timezone
import re

SCHED_MANUAL=0; SCHED_AT=1; SCHED_EVERY=2; SCHED_WEEKLY=3; SCHED_BOOT=4
DOW={"MON":0,"TUE":1,"WED":2,"THU":3,"FRI":4,"SAT":5,"SUN":6}
DOW_WORDS={
 "monday":"MON","mon":"MON","segunda":"MON","segunda-feira":"MON","lunes":"MON",
 "tuesday":"TUE","tue":"TUE","terca":"TUE","terça":"TUE","terça-feira":"TUE","martes":"TUE",
 "wednesday":"WED","wed":"WED","quarta":"WED","quarta-feira":"WED","miércoles":"WED","miercoles":"WED",
 "thursday":"THU","thu":"THU","quinta":"THU","quinta-feira":"THU","jueves":"THU",
 "friday":"FRI","fri":"FRI","sexta":"FRI","sexta-feira":"FRI","viernes":"FRI",
 "saturday":"SAT","sat":"SAT","sabado":"SAT","sábado":"SAT","sábado-feira":"SAT","sab":"SAT",
 "sunday":"SUN","sun":"SUN","domingo":"SUN",
}

def _parse_hms(text:str):
    m=re.fullmatch(r'\s*(\d{1,2}):(\d{2})(?::(\d{2}))?\s*', text)
    if not m: raise ValueError('schedule time must be HH:MM or HH:MM:SS')
    h,mi,se=int(m.group(1)),int(m.group(2)),int(m.group(3) or 0)
    if not (0<=h<=23 and 0<=mi<=59 and 0<=se<=59): raise ValueError('invalid schedule time')
    return h,mi,se

def normalize_schedule(schedule:dict|None, now:datetime|None=None)->dict:
    s=dict(schedule or {"mode":"MANUAL"}); mode=str(s.get('mode','MANUAL')).upper()
    if mode not in {'MANUAL','BOOT','AT','EVERY','WEEKLY'}: raise ValueError('invalid schedule.mode')
    if mode=='MANUAL': return {"mode":SCHED_MANUAL,"dow_mask":0,"arg0":0,"arg1":0,"semantic":{"mode":"MANUAL"}}
    if mode=='BOOT': return {"mode":SCHED_BOOT,"dow_mask":0,"arg0":0,"arg1":0,"semantic":{"mode":"BOOT"}}
    now=now or datetime.now().astimezone()
    if now.tzinfo is None: now=now.astimezone()
    if mode=='EVERY':
        sec=int(s.get('interval_s',0));
        if sec<=0: raise ValueError('EVERY requires interval_s > 0')
        anchor=int(s.get('anchor_epoch_utc') or now.timestamp())
        return {"mode":SCHED_EVERY,"dow_mask":0,"arg0":sec,"arg1":anchor,"semantic":s}
    if mode=='AT':
        if s.get('epoch_utc') is not None:
            epoch=int(s['epoch_utc']);
            if epoch<=0: raise ValueError('AT epoch_utc must be > 0')
        else:
            h,mi,se=_parse_hms(str(s.get('time','')))
            tzmode=str(s.get('timezone','LOCAL')).upper()
            tz=timezone.utc if tzmode=='UTC' else now.tzinfo
            if s.get('date'):
                d=datetime.strptime(str(s['date']),'%Y-%m-%d').date()
                target=datetime(d.year,d.month,d.day,h,mi,se,tzinfo=tz)
            else:
                n=now.astimezone(tz); target=n.replace(hour=h,minute=mi,second=se,microsecond=0)
                if target<=n: target += timedelta(days=1)
            epoch=int(target.timestamp())
        return {"mode":SCHED_AT,"dow_mask":0,"arg0":epoch,"arg1":0,"semantic":s}
    # WEEKLY: firmware ABI stores Monday=bit0..Sunday=bit6 and UTC seconds since midnight.
    days=s.get('days') or []
    if not isinstance(days,list) or not days: raise ValueError('WEEKLY requires days[]')
    mask=0
    for d in days:
        key=str(d).upper()
        if key not in DOW: raise ValueError(f'invalid WEEKLY day {d!r}')
        mask |= 1<<DOW[key]
    h,mi,se=_parse_hms(str(s.get('time','')))
    tzmode=str(s.get('timezone','LOCAL')).upper()
    if tzmode=='UTC': utc_seconds=h*3600+mi*60+se
    else:
        # Convert the requested local wall time using the host's current UTC offset.
        # The firmware weekly ABI is fixed UTC and therefore does not auto-adjust DST.
        local_tz=now.tzinfo
        probe=now.astimezone(local_tz).replace(hour=h,minute=mi,second=se,microsecond=0)
        u=probe.astimezone(timezone.utc)
        utc_seconds=u.hour*3600+u.minute*60+u.second
    return {"mode":SCHED_WEEKLY,"dow_mask":mask,"arg0":utc_seconds,"arg1":0,"semantic":s}

def detect_schedule_intent(source:str)->dict|None:
    """Conservative standalone schedule parser. Returns None when no external schedule is explicit."""
    low=source.lower()
    if re.search(r'\b(on[ -]?boot|at boot|at startup|after reset|on reset|ao ligar|no boot|na inicializa[cç][aã]o|al arrancar|al inicio)\b',low):
        return {"mode":"BOOT"}
    # Weekly/every-day clock schedules.
    tm=re.search(r'\b(?:at|às|as|a las)\s+(\d{1,2}:\d{2}(?::\d{2})?)\b',low)
    if tm:
        days=[]
        if re.search(r'\b(every day|daily|todos os dias|todo dia|diariamente|cada día|cada dia)\b',low):
            days=list(DOW)
        else:
            for word,code in DOW_WORDS.items():
                if re.search(r'(?<!\w)'+re.escape(word)+r'(?!\w)',low): days.append(code)
            days=list(dict.fromkeys(days))
        tzmode='UTC' if re.search(r'\b(?:utc|gmt)\b',low) else 'LOCAL'
        if days: return {"mode":"WEEKLY","days":days,"time":tm.group(1),"timezone":tzmode}
        out={"mode":"AT","time":tm.group(1),"timezone":tzmode}
        dm=re.search(r'\b(20\d{2}-\d{2}-\d{2})\b',low)
        if dm: out['date']=dm.group(1)
        return out
    # Program-level periodic schedule. Standalone may keep legacy WAIT/JMP for
    # regression compatibility, while agents/Semantic IR can lower this to EVERY.
    em=re.search(r'\b(?:schedule|run|execute|program|agende|agendar|ejecutar|programar)(?:\s+(?:this|the|o|el|program|programa))?\s+(?:every|once every|a cada|cada)\s+(\d+)\s*(seconds?|secs?|s|segundos?|minutes?|mins?|m|minutos?|hours?|h|horas?)\b',low)
    if em:
        n=int(em.group(1)); u=em.group(2)
        if u.startswith(('minute','minuto','min','m')) and not u.startswith('ms'): n*=60
        elif u.startswith(('hour','hora','h')): n*=3600
        return {"mode":"EVERY","interval_s":n}
    return None

def schedule_mode_name(mode:int)->str:
    return {0:'MANUAL',1:'AT',2:'EVERY',3:'WEEKLY',4:'BOOT'}.get(mode,f'UNKNOWN({mode})')


def encode_schedule_wire(schedule:dict)->bytes:
    """Encode the authenticated nrfclaw_schedule_t wire ABI (<BBHII>, 12 bytes)."""
    import struct
    return struct.pack('<BBHII', int(schedule['mode']), int(schedule.get('dow_mask',0)), 0, int(schedule.get('arg0',0)), int(schedule.get('arg1',0)))

def format_schedule_abi(schedule:dict)->list[str]:
    """Human-readable schedule diagnostics for CLI preview/upload."""
    mode=int(schedule.get('mode',0)); name=schedule_mode_name(mode)
    lines=[f"SCHEDULE ABI: {name} mode={mode} dow_mask=0x{int(schedule.get('dow_mask',0)):02x} arg0={int(schedule.get('arg0',0))} arg1={int(schedule.get('arg1',0))}"]
    wire=encode_schedule_wire(schedule)
    lines.append(f"SCHEDULE WIRE (12 bytes): {wire.hex(' ')}")
    if mode==SCHED_AT and int(schedule.get('arg0',0))>0:
        epoch=int(schedule['arg0'])
        dt_utc=datetime.fromtimestamp(epoch, timezone.utc)
        dt_local=dt_utc.astimezone()
        lines.append(f"SCHEDULE AT UTC:   {dt_utc.isoformat()}")
        lines.append(f"SCHEDULE AT LOCAL: {dt_local.isoformat()}")
    elif mode==SCHED_EVERY:
        lines.append(f"SCHEDULE EVERY: interval={int(schedule.get('arg0',0))}s anchor_epoch_utc={int(schedule.get('arg1',0))}")
    elif mode==SCHED_WEEKLY:
        mask=int(schedule.get('dow_mask',0)); days=[d for d,b in DOW.items() if mask & (1<<b)]
        sec=int(schedule.get('arg0',0)); h=sec//3600; mi=(sec%3600)//60; se=sec%60
        lines.append(f"SCHEDULE WEEKLY: days={','.join(days)} utc_time={h:02d}:{mi:02d}:{se:02d}")
    return lines

def validate_schedule_clause(source:str, program:dict|None)->list[str]:
    """Authoritative source->Schedule ABI semantic oracle.

    Returns compact violated invariants. It validates authenticated schedule
    metadata independently of VM actions/bytecode.
    """
    intent=detect_schedule_intent(source)
    expected=str((intent or {'mode':'MANUAL'}).get('mode','MANUAL')).upper()
    p=program if isinstance(program,dict) else {}
    actual=str(p.get('mode','')).upper()
    errors=[]
    if actual != expected:
        errors.append(f'schedule.mode expected {expected}, got {actual or "<missing>"}')
        return errors
    if expected in {'MANUAL','BOOT'}:
        return errors
    meta=p.get('schedule')
    if not isinstance(meta,dict):
        return [f'program.schedule required for {expected}']
    # Compare semantic fields before binary lowering. This avoids time-dependent
    # epoch comparisons for AT while still preventing clause loss/alteration.
    if expected=='AT':
        if intent.get('epoch_utc') is not None:
            if int(meta.get('epoch_utc',0) or 0) != int(intent['epoch_utc']):
                errors.append(f'AT epoch_utc expected {intent["epoch_utc"]}')
        else:
            if str(meta.get('time','')) != str(intent.get('time','')):
                errors.append(f'AT time expected {intent.get("time")}')
            if intent.get('date') and str(meta.get('date','')) != str(intent['date']):
                errors.append(f'AT date expected {intent["date"]}')
            if str(meta.get('timezone','LOCAL')).upper() != str(intent.get('timezone','LOCAL')).upper():
                errors.append(f'AT timezone expected {intent.get("timezone","LOCAL")}')
    elif expected=='EVERY':
        if int(meta.get('interval_s',0) or 0) != int(intent.get('interval_s',0)):
            errors.append(f'EVERY interval_s expected {intent.get("interval_s")}')
    elif expected=='WEEKLY':
        got_days={str(x).upper() for x in (meta.get('days') or [])}
        exp_days={str(x).upper() for x in (intent.get('days') or [])}
        if got_days != exp_days: errors.append(f'WEEKLY days expected {sorted(exp_days)}')
        if str(meta.get('time','')) != str(intent.get('time','')):
            errors.append(f'WEEKLY time expected {intent.get("time")}')
        if str(meta.get('timezone','LOCAL')).upper() != str(intent.get('timezone','LOCAL')).upper():
            errors.append(f'WEEKLY timezone expected {intent.get("timezone","LOCAL")}')
    return errors
