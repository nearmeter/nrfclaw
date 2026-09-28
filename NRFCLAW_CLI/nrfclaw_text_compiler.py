#!/usr/bin/env python3
"""Deterministic natural-language -> nRFClaw VM compiler MVP.

R3.8.14b8: this module deliberately does not use an LLM and never invents
opcodes.  It recognizes a small, auditable Portuguese/English intent grammar,
lowers it to a textual IR, validates the requested semantics, and assembles
only frozen VM ABI instructions.
"""
from __future__ import annotations

from dataclasses import dataclass
import re
import struct
import unicodedata

# Frozen VM ABI values from include/nrfclaw_opcodes.h
OP_END = 0x00
OP_MOVI = 0x08
OP_JMP = 0x0B
OP_CMP_LT = 0x0E
OP_CMP_GT = 0x10
OP_WAIT_S = 0x11
OP_WAIT_EVENT = 0x12
OP_JZ = 0x13
OP_ACCEL_MOTION = 0x54
OP_DS18_READ = 0x5D
OP_BLE_APP_ROLE = 0x70
OP_BLE_ADV_CONFIG = 0x71
OP_BLE_ADV_START = 0x72
OP_BLE_ADV_NAME = 0x76
OP_TRACKING_CONFIG = 0x78
OP_TRACKING_START = 0x79
OP_TRACKING_ROTATION_SET = 0x7C

BLE_ROLE_OFF = 0
BLE_ROLE_ADVERTISER = 1
EVENT_ACCEL_MOTION = 16
SCHED_MANUAL = 0
SCHED_AT = 1
SCHED_EVERY = 2
SCHED_WEEKLY = 3
SCHED_BOOT = 4


class TextCompileError(ValueError):
    pass


class UnsupportedSemantics(TextCompileError):
    pass


@dataclass
class CompileResult:
    source: str
    intent: str
    schedule_mode: int
    ir: list[str]
    bytecode: bytes
    warnings: list[str]
    # R3.8.20c2g3a-r1: authenticated program schedule metadata. VM bytecode ABI unchanged.
    schedule: dict | None = None
    # B7.6f2l2: optional host-side provisioning action applied by `prompt --upload`
    # before bytecode upload. This deliberately reuses existing NDP management
    # instead of adding another VM opcode or changing the NinaLink wire format.
    provision: dict | None = None

    @property
    def schedule_name(self) -> str:
        return {SCHED_MANUAL:"MANUAL", SCHED_AT:"AT", SCHED_EVERY:"EVERY", SCHED_WEEKLY:"WEEKLY", SCHED_BOOT:"BOOT"}.get(self.schedule_mode, f"UNKNOWN({self.schedule_mode})")


class Assembler:
    def __init__(self):
        self.code = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str, int]] = []  # operand offset, label, pc-after

    def label(self, name: str):
        if name in self.labels:
            raise TextCompileError(f"duplicate label: {name}")
        self.labels[name] = len(self.code)

    def emit(self, *values: int):
        self.code.extend(v & 0xFF for v in values)

    def u16(self, value: int):
        self.code += struct.pack("<H", value & 0xFFFF)

    def u32(self, value: int):
        self.code += struct.pack("<I", value & 0xFFFFFFFF)

    def movi(self, reg: int, value: int):
        self.emit(OP_MOVI, reg)
        self.u32(value)

    def wait_s(self, seconds: int):
        self.emit(OP_WAIT_S)
        self.u32(seconds)

    def jmp(self, label: str):
        self.emit(OP_JMP)
        operand = len(self.code)
        self.u16(0)
        self.fixups.append((operand, label, len(self.code)))

    def jz(self, reg: int, label: str):
        self.emit(OP_JZ, reg)
        operand = len(self.code)
        self.u16(0)
        self.fixups.append((operand, label, len(self.code)))

    def finish(self) -> bytes:
        for operand, label, after in self.fixups:
            if label not in self.labels:
                raise TextCompileError(f"undefined label: {label}")
            rel = self.labels[label] - after
            if not -32768 <= rel <= 32767:
                raise TextCompileError(f"jump to {label} out of range")
            struct.pack_into("<h", self.code, operand, rel)
        if not self.code or self.code[-1] != OP_END:
            self.emit(OP_END)
        if len(self.code) > 1024:
            raise TextCompileError(f"program is {len(self.code)} bytes; VM maximum is 1024")
        return bytes(self.code)


def _fold(text: str) -> str:
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    return re.sub(r"\s+", " ", text.lower()).strip()


def _duration_seconds(text: str, *, default: int | None = None, context: str = "") -> int:
    unit = r"(?:segundos?|secs?|seconds?|s|minutos?|mins?|minutes?|m|horas?|hours?|h)"
    prefix = re.escape(context) + r"\s*" if context else ""
    m = re.search(prefix + rf"(\d+)\s*({unit})", text)
    if not m:
        if default is not None:
            return default
        raise TextCompileError("duration not found")
    value = int(m.group(1))
    u = m.group(2)
    if u.startswith(("min", "m")) and u not in ("ms",):
        return value * 60
    if u.startswith(("hora", "hour", "h")):
        return value * 3600
    return value


def _every_seconds(t: str) -> int | None:
    m = re.search(r"(?:a cada|every)\s+(\d+)\s*(segundos?|seconds?|secs?|s|minutos?|minutes?|mins?|m)", t)
    if not m:
        return None
    n = int(m.group(1))
    return n * 60 if m.group(2).startswith(("min", "m")) else n


def _scoped_every_seconds(t: str, subject_pattern: str) -> int | None:
    """Find an `a cada/every` interval scoped to a subject/action phrase.

    The MVP used one global `_every_seconds()` value. That made a phrase such
    as "leia a temperatura; ... tracking com atualizacao a cada 2 segundos"
    incorrectly apply the tracking cadence to the temperature loop while
    leaving TRACKING_CONFIG at its hard-coded 1000 ms default.
    """
    unit = r"(segundos?|seconds?|secs?|s|minutos?|minutes?|mins?|m)"
    patterns = [
        rf"(?:{subject_pattern})[^;.]*?(?:a cada|every)\s+(\d+)\s*{unit}",
        rf"(?:{subject_pattern})[^;.]*?(?:atualizacao|atualize|update|intervalo|interval)[^;.]*?(?:a cada|every)?\s*(\d+)\s*{unit}",
    ]
    for pattern in patterns:
        m = re.search(pattern, t)
        if m:
            n = int(m.group(1))
            return n * 60 if m.group(2).startswith(("min", "m")) else n
    return None


def _temperature_every_seconds(t: str) -> int | None:
    return _scoped_every_seconds(t, r"(?:temperatura|temperature)")


def _tracking_every_seconds(t: str) -> int | None:
    # Accept both "tracking ... a cada 2 segundos" and the more natural
    # "tracking com atualizacao a cada 2 segundos".
    return _scoped_every_seconds(t, r"(?:tracking|rastreamento)")


def _ndp_off_requested(t: str) -> bool:
    return bool(re.search(
        r"(?:desligue|deslige|desative|desabilite|disable|turn off)"
        r"(?:\s+(?:a|o))?(?:\s+(?:conexao|connection))?\s+ndp\b|\bndp\s+off\b",
        t,
    ))


def _wait_seconds(t: str) -> int:
    m = re.search(r"(?:aguarde|espere|wait)\s+(\d+)\s*(segundos?|seconds?|secs?|s|minutos?|minutes?|mins?|m)", t)
    if not m:
        return 0
    n = int(m.group(1))
    return n * 60 if m.group(2).startswith(("min", "m")) else n


def _temperature_condition(t: str) -> tuple[str, int] | None:
    """Return (comparison, threshold_mC) for the deterministic grammar.

    Supported comparisons in r3.8.14b4:
      GT: acima de, maior que, above, greater than
      LT: abaixo de, menor que, below, less than
    """
    patterns = [
        ("GT", r"(?:acima de|maior que|above|greater than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|degrees?|°?c|celsius)"),
        ("LT", r"(?:abaixo de|menor que|below|less than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|degrees?|°?c|celsius)"),
    ]
    for comparison, pattern in patterns:
        m = re.search(pattern, t)
        if m:
            threshold = int(round(float(m.group(1).replace(",", ".")) * 1000.0))
            return comparison, threshold
    return None


def _extract_beacon_name(source: str) -> str:
    # Preserve original capitalization where possible.
    patterns = [
        # Explicit naming forms have priority. Keep `name` word-bounded so it
        # cannot match the prefix of `named` (which previously returned only "d").
        r"\b(?:named|chamado|chamada|nomeado|nomeada|como)\b\s*[=:]?\s*[\"']([^\"']+)[\"']",
        r"\b(?:named|chamado|chamada|nomeado|nomeada|como)\b\s*[=:]?\s*([A-Za-z0-9_.-]+)",
        r"(?:anuncie|anunciar|announce)\s+[\"']([^\"']+)[\"']",
        r"(?:anuncie|anunciar|announce)\s+(.+?)(?=\s+(?:a cada|every|com periodicidade|com intervalo|at interval)|$)",
        r"\b(?:nome|name)\b\s*[=:]?\s*[\"']?([A-Za-z0-9_.-]+)",
    ]
    for p in patterns:
        m = re.search(p, source, flags=re.IGNORECASE)
        if m:
            name = m.group(1).strip(" .,:;\"'")
            if name:
                return name
    return "nRFClaw"


def _is_boot(t: str) -> bool:
    return bool(re.search(r"\bboot\b|inicio(?: do)? boot|inicializacao|startup", t)) or "persistente" in t


def _continuous_motion_requested(t: str) -> tuple[bool, int | None]:
    # This semantic cannot currently be proven by the VM: ACCEL_MOTION generates
    # an interrupt/event, but there is no native primitive that exposes a
    # continuous-motion state for N seconds.
    patterns = [
        r"moviment(?:o|acao)\s+(?:maior que|por mais de|durante)\s+(\d+)\s*(segundos?|s|minutos?|m)",
        r"motion\s+(?:for more than|for|longer than)\s+(\d+)\s*(seconds?|s|minutes?|m)",
    ]
    for p in patterns:
        m = re.search(p, t)
        if m:
            n = int(m.group(1))
            if m.group(2).startswith("m"):
                n *= 60
            return True, n
    return False, None


def _compile_beacon(source: str, t: str, schedule: int) -> CompileResult:
    name = _extract_beacon_name(source)
    wait = _wait_seconds(t)
    interval_s = _every_seconds(t) or 2
    interval_ms = interval_s * 1000
    if not 20 <= interval_ms <= 10240:
        raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")
    raw = name.encode("utf-8")
    if not 1 <= len(raw) <= 24:
        raise TextCompileError("Beacon local name must be 1..24 UTF-8 bytes")
    tx = 4
    mt = re.search(r"([+-]?\d+)\s*d\s*b\s*m", t)
    if mt:
        tx = int(mt.group(1))
    if not -128 <= tx <= 127:
        raise TextCompileError("TX power must fit signed int8")

    a = Assembler()

    # r3.8.14b2: a textual BOOT program that intends to own BLE must claim
    # the Application BLE plane immediately at VM start, before any semantic
    # WAIT.  The native NDP peripheral starts during firmware boot and uses
    # adaptive 100/500/2000 ms advertising.  If we wait first, an NDP link
    # can be established (or the adaptive state can already be NORMAL) before
    # the later role takeover.  That made long waits timing-dependent.
    #
    # ROLE OFF is therefore used as a low-power ownership barrier: it stops
    # the default NDP advertising and cancels its adaptive timer, while keeping
    # P0.21/NUS programming available.  The requested wait then occurs with
    # BLE Application advertising off, after which the program selects the
    # requested broadcaster role and starts its own advertisement.
    ir = ["CLAIM BLE ; Application role OFF during pre-action wait"]
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)

    if wait:
        ir.append(f"WAIT {wait}s")
        a.wait_s(wait)
    ir += [
        "BLE ROLE BEACON",
        f'BLE NAME "{name}"',
        f"BLE INTERVAL {interval_ms}ms",
        f"BLE TX_POWER {tx:+d}dBm",
        "BLE START",
        "END",
    ]
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_ADVERTISER)
    a.emit(OP_BLE_ADV_CONFIG)
    a.u16(interval_ms)
    a.emit(tx, 0)  # empty manufacturer/service payload
    a.emit(OP_BLE_ADV_NAME, len(raw))
    a.code += raw
    a.emit(OP_BLE_ADV_START, OP_END)
    return CompileResult(source, "beacon", schedule, ir, a.finish(), [])


def _compile_temperature_flow(source: str, t: str, schedule: int) -> CompileResult:
    # Temperature cadence is optional. Without a cadence, "leia a temperatura"
    # means one reading at boot. Do not steal a cadence scoped to Tracking.
    temperature_every = _temperature_every_seconds(t)
    tracking_every = _tracking_every_seconds(t)
    condition = _temperature_condition(t)
    if condition is None:
        raise TextCompileError(
            "temperature condition not understood; use e.g. 'acima de 25 graus' or 'menor que 1 grau'"
        )
    comparison, threshold = condition

    wants_motion = "motion" in t or "movimento" in t or "movimentacao" in t
    wants_tracking = "tracking" in t or "rastreamento" in t
    continuous, seconds = _continuous_motion_requested(t)
    if continuous:
        raise UnsupportedSemantics(
            f"continuous motion for {seconds}s is not representable safely by the current VM ABI: "
            "ACCEL_MOTION provides a wake/event but there is no MOTION_ACTIVE/MOTION_LOST primitive. "
            "A firmware CAP/opcode must be added before compiling this condition."
        )

    a = Assembler()
    cmp_name = "CMP_GT" if comparison == "GT" else "CMP_LT"
    cmp_opcode = OP_CMP_GT if comparison == "GT" else OP_CMP_LT
    ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF")
        a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    ir += ["LABEL TEMP_LOOP", "DS18 READ -> R0", f"MOVI R1 {threshold} ; milli-degC", f"{cmp_name} R2 R0 R1"]
    a.label("temp_loop")
    a.emit(OP_DS18_READ, 0)
    a.movi(1, threshold)
    a.emit(cmp_opcode, 2, 0, 1)
    a.jz(2, "temp_condition_false")

    if wants_motion:
        # Low-power default: threshold=250 mg, one 100ms duration tick.
        ir += ["ACCEL MOTION threshold=250mg duration=100ms", "WAIT_EVENT MOTION -> R3,R4"]
        a.emit(OP_ACCEL_MOTION)
        a.u16(250)
        a.emit(1)
        a.emit(OP_WAIT_EVENT, EVENT_ACCEL_MOTION, 3, 4)

    if wants_tracking:
        # Tracking cadence belongs to TRACKING_CONFIG, not to the temperature
        # sampler. Default remains 1000 ms only when the sentence does not
        # specify a tracking update interval.
        tracking_interval_ms = (tracking_every or 1) * 1000
        if not 100 <= tracking_interval_ms <= 10000:
            raise TextCompileError("tracking update interval must be between 100 ms and 10 seconds")
        # validate_program() requires OP_END to be the physical final instruction.
        # Do not emit END in the true branch before the false branch.
        ir += [f"TRACKING CONFIG interval={tracking_interval_ms}ms tx=+4dBm", "TRACKING ROTATION 10800s", "TRACKING START", "JMP PROGRAM_END"]
        a.emit(OP_TRACKING_CONFIG)
        a.u16(tracking_interval_ms)
        a.emit(4)
        a.emit(OP_TRACKING_ROTATION_SET)
        a.u32(10800)
        a.emit(OP_TRACKING_START)
        a.jmp("program_end")
    else:
        # Condition has no recognized action: fail instead of silently dropping it.
        raise UnsupportedSemantics("temperature condition has no supported action to compile")

    a.label("temp_condition_false")
    ir += ["LABEL TEMP_CONDITION_FALSE"]
    if temperature_every is not None:
        ir += [f"WAIT {temperature_every}s", "JMP TEMP_LOOP"]
        a.wait_s(temperature_every)
        a.jmp("temp_loop")
    else:
        # No temperature cadence was requested: this is a one-shot condition.
        ir += ["JMP PROGRAM_END ; one-shot temperature read"]
        a.jmp("program_end")
    ir += ["LABEL PROGRAM_END", "END"]
    a.label("program_end")
    a.emit(OP_END)
    warnings = []
    if wants_motion:
        warnings.append("motion uses low-power INT wake; default threshold=250mg, duration=100ms")
    return CompileResult(source, "temperature-flow", schedule, ir, a.finish(), warnings)



def _compile_motion_tracking(source: str, t: str, schedule: int) -> CompileResult:
    continuous, seconds = _continuous_motion_requested(t)
    if continuous:
        raise UnsupportedSemantics(
            f"continuous motion for {seconds}s is not representable safely by the current VM ABI: "
            "ACCEL_MOTION provides a wake/event but there is no MOTION_ACTIVE/MOTION_LOST primitive."
        )
    wants_tracking = "tracking" in t or "rastreamento" in t
    if not wants_tracking:
        raise UnsupportedSemantics("motion intent currently requires a supported action, e.g. 'ative tracking'")
    tracking_every = _tracking_every_seconds(t)
    tracking_interval_ms = (tracking_every or 1) * 1000
    if not 100 <= tracking_interval_ms <= 10000:
        raise TextCompileError("tracking update interval must be between 100 ms and 10 seconds")
    a = Assembler()
    ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF")
        a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    ir += [
        "ACCEL MOTION threshold=250mg duration=100ms (HP/INT wake)",
        "WAIT_EVENT MOTION -> R3,R4",
        f"TRACKING CONFIG interval={tracking_interval_ms}ms tx=+4dBm",
        "TRACKING ROTATION 10800s",
        "TRACKING START",
        "END",
    ]
    a.emit(OP_ACCEL_MOTION)
    a.u16(250)
    a.emit(1)
    a.emit(OP_WAIT_EVENT, EVENT_ACCEL_MOTION, 3, 4)
    a.emit(OP_TRACKING_CONFIG)
    a.u16(tracking_interval_ms)
    a.emit(4)
    a.emit(OP_TRACKING_ROTATION_SET)
    a.u32(10800)
    a.emit(OP_TRACKING_START, OP_END)
    return CompileResult(
        source, "motion-tracking", schedule, ir, a.finish(),
        ["motion uses high-pass INT1 wake so static gravity does not trigger the event"]
    )


def _compile_tracking(source: str, t: str, schedule: int) -> CompileResult:
    """Compile a direct Tracking intent with no sensor/event prerequisite.

    Examples:
      no boot ative tracking
      no boot ative tracking com atualização a cada 2 segundos
      no boot desligue ndp e ative tracking a cada 5 segundos

    TRACKING_START itself owns the BLE advertiser in firmware (r3.8.14b6), so
    an explicit BLE_ROLE_OFF is emitted only when the sentence explicitly asks
    for NDP to be disabled before Tracking starts.
    """
    tracking_every = _tracking_every_seconds(t)
    tracking_interval_ms = (tracking_every or 1) * 1000
    if not 100 <= tracking_interval_ms <= 10000:
        raise TextCompileError("tracking update interval must be between 100 ms and 10 seconds")

    a = Assembler()
    ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF")
        a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)

    ir += [
        f"TRACKING CONFIG interval={tracking_interval_ms}ms tx=+4dBm",
        "TRACKING ROTATION 10800s",
        "TRACKING START",
        "END",
    ]
    a.emit(OP_TRACKING_CONFIG)
    a.u16(tracking_interval_ms)
    a.emit(4)
    a.emit(OP_TRACKING_ROTATION_SET)
    a.u32(10800)
    a.emit(OP_TRACKING_START, OP_END)
    return CompileResult(source, "tracking", schedule, ir, a.finish(), [])

def compile_text(source: str, force_boot: bool = False) -> CompileResult:
    if not source or not source.strip():
        raise TextCompileError("empty prompt")
    t = _fold(source)
    schedule = SCHED_BOOT if (force_boot or _is_boot(t)) else SCHED_MANUAL

    if "beacon" in t or "beacom" in t or "anuncie" in t or "announce" in t:
        return _compile_beacon(source, t, schedule)
    if "temperatura" in t or "temperature" in t:
        return _compile_temperature_flow(source, t, schedule)
    if ("motion" in t or "movimento" in t or "movimentacao" in t) and ("tracking" in t or "rastreamento" in t):
        return _compile_motion_tracking(source, t, schedule)
    if "tracking" in t or "rastreamento" in t:
        return _compile_tracking(source, t, schedule)
    raise UnsupportedSemantics(
        "intent not recognized by the deterministic MVP. Supported families: Beacon advertising; "
        "temperature threshold -> optional motion event -> tracking; motion event -> tracking; direct tracking."
    )


def format_ir(result: CompileResult) -> str:
    lines = [f"PROGRAM {result.schedule_name}", f"INTENT {result.intent}"]
    lines.extend(f"  {line}" for line in result.ir)
    if result.warnings:
        lines.append("WARNINGS")
        lines.extend(f"  - {w}" for w in result.warnings)
    return "\n".join(lines)


def bytecode_hex(code: bytes) -> str:
    return code.hex(" ")


if __name__ == "__main__":
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("text", nargs="+")
    p.add_argument("--boot", action="store_true")
    ns = p.parse_args()
    r = compile_text(" ".join(ns.text), force_boot=ns.boot)
    print(format_ir(r))
    print("BYTECODE", bytecode_hex(r.bytecode))
