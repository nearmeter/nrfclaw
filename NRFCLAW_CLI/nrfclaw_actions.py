#!/usr/bin/env python3
"""R3.8.16b1i canonical action layer.

Natural language is first normalized to canonical English.  This module then
extracts orthogonal verbs (SEND/RECEIVE/FORWARD/READ/WAIT/DISABLE/START/STOP/
DISPLAY) and their objects.  It deliberately does not emit bytecode; lowering
remains deterministic in nrfclaw_hybrid_semantic.py.
"""
from __future__ import annotations
from dataclasses import dataclass, asdict
import re
from typing import Any

@dataclass(frozen=True)
class Action:
    verb: str
    target: str
    source: str | None = None
    destination: str | None = None
    value: Any = None
    cadence_s: int | None = None
    phase: str | None = None
    trigger: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {k:v for k,v in asdict(self).items() if v is not None}

@dataclass(frozen=True)
class ActionPlan:
    actions: tuple[Action, ...]
    unresolved: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {"actions":[a.to_dict() for a in self.actions], "unresolved":list(self.unresolved)}


def _period_s(text: str) -> int | None:
    m=re.search(r'\bevery\s+(\d+)\s*(seconds?|minutes?|hours?)\b',text,re.I)
    if not m: return None
    n=int(m.group(1)); u=m.group(2).lower()
    return n*(3600 if u.startswith('hour') else 60 if u.startswith('minute') else 1)


def parse_action_plan(canonical: str) -> ActionPlan:
    """Extract canonical actions without guessing unknown semantics."""
    t=' '.join(canonical.lower().split())
    acts:list[Action]=[]; unresolved:list[str]=[]
    boot=bool(re.search(r'\bat boot\b',t))

    # DISABLE is independent and must never be swallowed by a later family.
    if re.search(r'\bdisable\s+(?:the\s+)?ndp\b',t):
        acts.append(Action('DISABLE','NDP',phase='BOOT' if boot else 'NOW'))

    # RECEIVE is direction-sensitive. Presence of receive/listen suppresses SEND
    # interpretation unless a separate explicit send clause is present.
    if re.search(r'\b(?:receive|listen(?:ing)?)\b[^.;]*\blora\b|\blora\b[^.;]*\b(?:receive|listen(?:ing)?)\b',t):
        acts.append(Action('RECEIVE','MESSAGE',source='LORA',destination='BUFFER',phase='CONTINUOUS'))

    # READ primitives.
    if re.search(r'\bread\b[^.;]*\btemperature\b',t):
        acts.append(Action('READ','TEMPERATURE',destination='R0',cadence_s=_period_s(t)))
    if re.search(r'\bread\b[^.;]*\bbattery\b',t):
        acts.append(Action('READ','BATTERY',destination='R0',cadence_s=_period_s(t)))

    # FORWARD received data.
    if re.search(r'\bforward\b[^.;]*\bserial\b',t):
        acts.append(Action('FORWARD','BUFFER',source='BUFFER',destination='SERIAL',trigger='LORA_RX'))

    # Beacon display of the last received payload.  START and DISPLAY are kept
    # separate so the lowerer can preserve data flow rather than invent a name.
    if re.search(r'\b(?:start|enable)\b[^.;]*\bbeacon\b',t):
        trigger='LORA_RX' if re.search(r'\bwhen\b[^.;]*(?:message|data)[^.;]*(?:arrive|received)',t) else None
        acts.append(Action('START','BEACON',trigger=trigger))
    if re.search(r'\bbeacon\b[^.;]*(?:display|show|advertise)[^.;]*(?:received (?:value|data|message)|value received|received value)',t):
        acts.append(Action('DISPLAY','BUFFER',source='BUFFER',destination='BEACON',trigger='LORA_RX'))

    # Generic LoRa SEND, but only if send/transmit is explicit.  RECEIVE alone
    # can no longer fall into the send path.
    send_clause=re.search(r'\b(?:send|transmit)\b[^.;]*\blora\b|\blora\b[^.;]*\b(?:send|transmit)\b',t)
    if send_clause:
        acts.append(Action('SEND','MESSAGE',destination='LORA',cadence_s=_period_s(t)))

    # WAIT duration is represented independently.
    wm=re.search(r'\bwait\s+(\d+)\s*(seconds?|minutes?|hours?)\b',t)
    if wm:
        n=int(wm.group(1)); u=wm.group(2)
        acts.append(Action('WAIT','TIME',value=n*(3600 if u.startswith('hour') else 60 if u.startswith('minute') else 1)))

    return ActionPlan(tuple(acts),tuple(unresolved))
