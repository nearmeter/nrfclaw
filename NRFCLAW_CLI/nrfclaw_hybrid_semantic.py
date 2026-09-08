#!/usr/bin/env python3
"""nRFClaw R3.8.16 Hybrid Semantic Engine.

Default host-side natural-language path.  No LLM/GPU/network required.
It combines:
  * deterministic compositional parsing,
  * a tiny local semantic matcher (token + character n-grams),
  * slot/unit extraction,
  * strict lowering to the frozen VM ABI,
  * legacy R3.8.14b8 compiler fallback.

The engine never invents opcodes.  Unknown/ambiguous semantics are rejected.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import difflib
import json
import math
import re
import struct
import unicodedata
from typing import Any

from nrfclaw_language import canonicalize_to_english
from nrfclaw_actions import ActionPlan, parse_action_plan
from nrfclaw_standalone_certifier import certify as certify_standalone
from nrfclaw_schedule_semantics import detect_schedule_intent, normalize_schedule

from nrfclaw_text_compiler import (
    Assembler, CompileResult, TextCompileError, UnsupportedSemantics,
    compile_text as legacy_compile_text,
    SCHED_BOOT, SCHED_MANUAL, SCHED_AT, SCHED_EVERY, SCHED_WEEKLY,
    OP_END, OP_MOVI, OP_JMP, OP_JZ, OP_CMP_LT, OP_CMP_GT,
    OP_WAIT_S, OP_DS18_READ, OP_ACCEL_MOTION, OP_WAIT_EVENT,
    OP_TRACKING_CONFIG, OP_TRACKING_START,
    OP_TRACKING_ROTATION_SET, OP_BLE_APP_ROLE,
    BLE_ROLE_OFF, BLE_ROLE_ADVERTISER, EVENT_ACCEL_MOTION,
)

# Frozen VM ABI values not exported by the legacy text compiler module.
OP_ADD = 0x02
OP_SUB = 0x03
OP_MOD = 0x06
OP_CMP_EQ = 0x0C
OP_STATE_SET = 0x42
OP_STATE_GET = 0x43
OP_PERSIST_SAVE = 0x44
OP_PERSIST_LOAD = 0x45
OP_WAIT_HALL = 0x22
OP_BAT_READ = 0x30
OP_GPIO_WRITE_IMM = 0x50
OP_LORA_SEND_BYTES = 0x61
OP_LORA_CONFIG = 0x62
OP_FORMAT_REG = 0x63
OP_LORA_SEND_BUF = 0x64
OP_LORA_RX_START = 0x65
OP_SERIAL_WRITE_BUF = 0x66
OP_BLE_ADV_BUF = 0x67
OP_APP_EVENT_SEND = 0x68
OP_DEBUG_BUFFER = 0x69
OP_SERIAL_CONFIG = 0x58
OP_BLE_ADV_CONFIG = 0x71
OP_BLE_ADV_START = 0x72
OP_BLE_ADV_NAME = 0x76
OP_TRACKING_STOP = 0x7A
OP_VIB_AUTO_START = 0x80
OP_VIB_AUTO_STOP = 0x81
OP_HALL_CONFIG = 0x82
OP_VIB_AUTO_CONFIG_TIME = 0x83
OP_SYSTEM_MIN_POWER = 0x84
OP_SERIAL_RX_BUF = 0x85
OP_APP_SEND_BUF = 0x86
OP_SERIAL_CONFIG_ECO = 0x87
OP_FORMAT_2REG = 0x88
OP_PARSE_BUF_I32 = 0x89
OP_PARSE_BUF_FIXED = 0x8A
OP_FORMAT_REG_FIXED = 0x8B
OP_BUFFER_PREPEND = 0x8C
OP_BUFFER_SET = 0x8D
OP_GPIO_READ = 0x52
OP_MOV = 0x01
EVENT_VIB_MACHINE_ON = 0x40
EVENT_VIB_LEARN_COMPLETE = 0x41
EVENT_VIB_WARNING = 0x42
EVENT_VIB_ALARM = 0x43
EVENT_VIB_MACHINE_OFF = 0x44

FMT_U32 = 0
FMT_I32 = 1
FMT_CENTIVOLTS = 2
FMT_MILLICELSIUS = 3
FMT_U32_02 = 4

SEMANTIC_ENGINE_VERSION = 32
DEFAULT_CONFIG = Path.home() / ".config" / "nrfclaw" / "semantic.json"
BUILTIN_EXAMPLES = Path(__file__).resolve().parent / "nrfclaw_semantic_examples.json"
USER_EXAMPLES = Path.home() / ".config" / "nrfclaw" / "semantic_examples.json"

DEFAULTS = {
    "battery_low_cv": None,      # deliberately unset: product/user must define it
    "battery_check_s": 60,
    "tracking_interval_ms": 1000,
    "tracking_tx_power_dbm": 4,
    "tracking_rotation_s": 10800,
}


def _fold(text: str) -> str:
    s = unicodedata.normalize("NFKD", text)
    s = "".join(c for c in s if not unicodedata.combining(c))
    s = s.lower().replace("°", " ")
    s = re.sub(r"\s+", " ", s).strip()
    return s


def _tokens(text: str) -> set[str]:
    return set(re.findall(r"[a-z0-9_]+", _fold(text)))


def _char_ngrams(text: str, n: int = 3) -> set[str]:
    s = re.sub(r"[^a-z0-9]+", " ", _fold(text))
    if len(s) < n:
        return {s} if s else set()
    return {s[i:i+n] for i in range(len(s)-n+1)}


def _similarity(a: str, b: str) -> float:
    ta, tb = _tokens(a), _tokens(b)
    tok = len(ta & tb) / max(1, len(ta | tb))
    ga, gb = _char_ngrams(a), _char_ngrams(b)
    ng = len(ga & gb) / max(1, len(ga | gb))
    seq = difflib.SequenceMatcher(None, _fold(a), _fold(b)).ratio()
    return 0.45 * tok + 0.35 * ng + 0.20 * seq


def load_config(path: Path | None = None) -> dict[str, Any]:
    path = DEFAULT_CONFIG if path is None else path
    cfg = dict(DEFAULTS)
    if path.exists():
        try:
            obj = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(obj, dict):
                for k in cfg:
                    if k in obj:
                        cfg[k] = obj[k]
        except Exception:
            pass
    return cfg


def save_config(cfg: dict[str, Any], path: Path | None = None) -> None:
    path = DEFAULT_CONFIG if path is None else path
    path.parent.mkdir(parents=True, exist_ok=True)
    out = {k: cfg.get(k, DEFAULTS[k]) for k in DEFAULTS}
    path.write_text(json.dumps(out, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def update_config(*, battery_low_v: float | None = None,
                  battery_check_s: int | None = None) -> dict[str, Any]:
    cfg = load_config()
    if battery_low_v is not None:
        if not 1.5 <= battery_low_v <= 5.5:
            raise TextCompileError("battery low threshold must be 1.5..5.5 V")
        cfg["battery_low_cv"] = int(round(battery_low_v * 100.0))
    if battery_check_s is not None:
        if not 1 <= battery_check_s <= 86400:
            raise TextCompileError("battery check interval must be 1..86400 s")
        cfg["battery_check_s"] = int(battery_check_s)
    save_config(cfg)
    return cfg


def _load_examples() -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    for path in (BUILTIN_EXAMPLES, USER_EXAMPLES):
        if not path.exists():
            continue
        try:
            obj = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(obj, list):
                out.extend(x for x in obj if isinstance(x, dict) and isinstance(x.get("text"), str))
        except Exception:
            continue
    return out


def semantic_matches(source: str, limit: int = 3) -> list[tuple[float, dict[str, Any]]]:
    ranked = [(_similarity(source, e["text"]), e) for e in _load_examples()]
    ranked.sort(key=lambda x: x[0], reverse=True)
    return ranked[:limit]


def teach_example(text: str, family: str, canonical: str = "") -> Path:
    USER_EXAMPLES.parent.mkdir(parents=True, exist_ok=True)
    arr = []
    if USER_EXAMPLES.exists():
        try:
            obj = json.loads(USER_EXAMPLES.read_text(encoding="utf-8"))
            if isinstance(obj, list): arr = obj
        except Exception: pass
    arr.append({"text": text, "family": family, "canonical": canonical})
    USER_EXAMPLES.write_text(json.dumps(arr, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return USER_EXAMPLES


def _is_boot(t: str) -> bool:
    return bool(re.search(r"\bboot\b|ao ligar|quando (?:o )?equipamento ligar|quando iniciar|inicializacao|startup|persistente", t))


def _parse_seconds_fragment(fragment: str) -> int | None:
    # R3.8.16b: natural singular cadence ("a cada minuto/segundo/hora") implies one.
    m = re.search(r"(?:(\d+)\s*)?(milissegundos?|ms|segundos?|secs?|seconds?|s|minutos?|mins?|minutes?|m|horas?|hours?|h)\b", fragment)
    if not m: return None
    n = int(m.group(1) or 1); u = m.group(2)
    if u.startswith("ms"): return max(1, int(round(n / 1000.0)))
    if u.startswith(("min", "m")) and u != "ms": return n * 60
    if u.startswith(("hora", "hour", "h")): return n * 3600
    return n


def _scoped_interval_s(t: str, subject: str) -> int | None:
    patterns = [
        rf"(?:{subject})[^;.]*?(?:a cada|de)\s+(?:(\d+)\s*)?(segundos?|s|minutos?|m|horas?|h)",
        rf"(?:{subject})[^;.]*?(?:intervalo|atualizacao|periodicidade)[^;.]*?(\d+)\s*(segundos?|s|minutos?|m|horas?|h)",
    ]
    for p in patterns:
        m = re.search(p, t)
        if m:
            return _parse_seconds_fragment((m.group(1) or "1") + m.group(2))
    return None


def _temperature_threshold(t: str) -> tuple[str, int] | None:
    pats = [
        ("GT", r"(?:acima de|maior que|superior a|passar de|passar dos?|above|greater than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)"),
        ("LT", r"(?:abaixo de|menor que|inferior a|below|less than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)"),
    ]
    for op, p in pats:
        m = re.search(p, t)
        if m:
            return op, int(round(float(m.group(1).replace(",", ".")) * 1000))
    return None


def _battery_threshold_cv(t: str, cfg: dict[str, Any]) -> int | None:
    m = re.search(r"bater(?:ia|y)[^;.]*?(?:abaixo de|menor que|below|less than)\s*(\d+(?:[\.,]\d+)?)\s*v\b", t)
    if m:
        return int(round(float(m.group(1).replace(",", ".")) * 100.0))
    if re.search(r"bateria (?:estiver |ficar )?baixa|low battery|battery (?:is )?low", t):
        v = cfg.get("battery_low_cv")
        if v is None:
            raise UnsupportedSemantics(
                "'bateria baixa' needs a configured numeric threshold. Set it once with "
                "'semantic-set --battery-low-v <volts>' or write e.g. 'bateria abaixo de 2.8V'."
            )
        return int(v)
    return None


def _gpio_pin(t: str) -> int | None:
    m = re.search(r"(?:pino|pin|gpio)\s*(?:p0[\._])?(\d{1,2})\b", t)
    return int(m.group(1)) if m else None


def _word_number(token: str) -> int | None:
    table={"um":1,"uma":1,"one":1,"dois":2,"duas":2,"two":2,"tres":3,"three":3,"quatro":4,"four":4,"cinco":5,"five":5,"seis":6,"six":6,"sete":7,"seven":7,"oito":8,"eight":8,"nove":9,"nine":9,"dez":10,"ten":10}
    token=_fold(token)
    if token.isdigit(): return int(token)
    return table.get(token)


def _lora_repeat(t: str) -> tuple[int, int]:
    count = 1; every = 0
    num=r"(\d+|um|uma|dois|duas|tres|três|quatro|cinco|seis|sete|oito|nove|dez|one|two|three|four|five|six|seven|eight|nine|ten)"
    m = re.search(rf"(?:faca|faça|realize|envie)\s*{num}\s*(?:envios?|vezes|transmissoes?)", t)
    if m:
        count=_word_number(m.group(1)) or 1
    m = re.search(r"(?:envios?|vezes|transmissoes?)[^;.]*?(?:a cada|de|com intervalo de)\s*(\d+)\s*(segundos?|s|minutos?|m)", t)
    if not m:
        m = re.search(rf"{num}\s*(?:envios?|vezes|transmissoes?)[^;.]*?(?:a cada|de|com intervalo de)\s*(\d+)\s*(segundos?|s|minutos?|m)", t)
        if m:
            count=_word_number(m.group(1)) or count
            every=_parse_seconds_fragment(m.group(2)+m.group(3)) or 0
            return count,every
    if m: every=_parse_seconds_fragment(m.group(1)+m.group(2)) or 0
    if not 1 <= count <= 32: raise TextCompileError("LoRa repeat count must be 1..32")
    return count,every



def _vib_alarm_requested(t: str) -> bool:
    """Recognize explicit VIB_AUTO alarms and natural machine-anomaly wording.

    Product semantics: when the user speaks about a machine/vibration anomaly
    without naming another detector, nRFclaw maps it to the validated
    VIB_AUTO.ALARM event. This is a semantic alias, not a new VM opcode.
    """
    if re.search(r"vib[_ ]?auto", t) and re.search(r"anomalia|alarm|alarme|anomaly", t):
        return True
    if re.search(r"(?:maquina|machine|equipamento|equipment)[^.;]{0,90}(?:anomalia|alarm|alarme|anomaly)", t):
        return True
    if re.search(r"(?:anomalia|anomaly)[^.;]{0,60}(?:vibracao|vibration|maquina|machine)", t):
        return True
    return False

def _ndp_off_requested(t: str) -> bool:
    return bool(re.search(
        r"(?:desligue|deslige|desative|desabilite|desactiva|disable|deactivate|turn off|switch off)[^;.]{0,40}(?:ndp|bluetooth)|(?:ndp|bluetooth)\s+(?:off|desligado|desativado|desabilitado|deactivated)",
        t,
    ))


def _lora_frequency_hz(t: str, default: int = 915_000_000) -> int:
    m = re.search(r"(?:frequencia(?:\s+de)?|frequency(?:\s+of)?|em|using)\s*(\d+(?:[\.,]\d+)?)\s*mhz", t)
    if not m:
        m = re.search(r"\b(\d+(?:[\.,]\d+)?)\s*mhz\b", t)
    hz = int(round(float(m.group(1).replace(",", ".")) * 1_000_000.0)) if m else default
    if not 150_000_000 <= hz <= 960_000_000:
        raise TextCompileError("LoRa frequency must be 150..960 MHz for the current LLCC68 profile")
    return hz


def _lora_profile(t: str) -> tuple[int, int, int, int, int]:
    freq = _lora_frequency_hz(t)
    m = re.search(r"(?:tx\s*power|potencia(?:\s+tx)?|power)\s*(?:de\s*)?([+-]?\d+)\s*dbm", t)
    if not m:
        m = re.search(r"(?:lora[^;.]{0,100}?)\b([+-]?\d+)\s*dbm\b", t)
    power = int(m.group(1)) if m else 14
    m = re.search(r"\bsf\s*(\d{1,2})\b", t)
    sf = int(m.group(1)) if m else 7
    m = re.search(r"(?:bw|bandwidth|largura[^;.]{0,15})\s*(\d+)\s*khz", t)
    bw = int(m.group(1)) if m else 125
    m = re.search(r"(?:cr|coding\s*rate)\s*(?:4/)?([5-8]|[1-4])\b", t)
    cr = 1
    if m:
        raw = int(m.group(1)); cr = raw - 4 if raw >= 5 else raw
    if not -9 <= power <= 22: raise TextCompileError("LoRa TX power must be -9..+22 dBm")
    if not 5 <= sf <= 11: raise TextCompileError("LoRa SF must be 5..11")
    if bw not in (125,250,500): raise TextCompileError("LoRa BW must be 125, 250 or 500 kHz")
    if not 1 <= cr <= 4: raise TextCompileError("LoRa CR must be 1..4 (4/5..4/8)")
    return freq,power,sf,bw,cr


def _emit_lora_config(a: Assembler, ir: list[str], profile: tuple[int,int,int,int,int]) -> None:
    freq,power,sf,bw,cr = profile
    ir.append(f"LORA CONFIG freq={freq/1_000_000:g}MHz tx={power:+d}dBm SF{sf} BW{bw} CR4/{cr+4}")
    a.emit(OP_LORA_CONFIG); a.u32(freq); a.emit(power,sf); a.u16(bw); a.emit(cr)


def _lora_rf_config_requested(t: str) -> bool:
    """True only when the prompt explicitly requests an RF parameter change."""
    patterns = (
        r"\b\d+(?:[\.,]\d+)?\s*mhz\b",
        r"(?:tx\s*power|txpower|potencia(?:\s+tx)?|potência(?:\s+tx)?|power)[^;.]{0,20}[+-]?\d+\s*dbm\b",
        r"\b[+-]?\d+\s*dbm\b",
        r"\bsf\s*\d{1,2}\b",
        r"(?:\bbw\b|bandwidth|largura(?:\s+de)?\s+banda)[^;.]{0,20}\d+\s*khz\b",
        r"(?:\bcr\b|coding\s*rate|taxa(?:\s+de)?\s+codifica)[^;.]{0,20}(?:4/)?[1-8]\b",
    )
    return any(re.search(p, t, re.I) for p in patterns)


def _emit_lora_config_if_requested(a: Assembler, ir: list[str], t: str) -> bool:
    if not _lora_rf_config_requested(t):
        return False
    _emit_lora_config(a, ir, _lora_profile(t))
    return True


def _explicit_payload_prefix(source: str) -> str | None:
    """Return an explicitly named dynamic payload/prefix string.

    R3.8.18k accepts natural forms such as:
      com payload 'T='
      com prefixo 'T='
      with payload "T="
      with prefix "T="
    The quotes are required so the boundary of the dynamic prefix is exact.
    """
    m = re.search(
        r"(?:\bpayload\b|\bprefixo\b|\bprefix\b)\s*(?:[:=]\s*|(?:como|as)\s+)?[\"']([^\"']*)[\"']",
        source, re.I)
    return m.group(1) if m else None


# R3.8.20c2e4: safe unquoted dynamic prefixes and lightweight format wording.
# Quoted values remain byte-for-byte authoritative.  For unquoted forms, final
# sentence punctuation is not part of the prefix ("prefixo B=." -> "B=").
_nrfclaw_c2e_quoted_payload_prefix = _explicit_payload_prefix
def _explicit_payload_prefix(source: str) -> str | None:
    value = _nrfclaw_c2e_quoted_payload_prefix(source)
    if value is not None:
        return value
    m = re.search(r'(?:prepend|prefixe|adicione|adicionar|agregue|agregar)\s+(?:o\s+|el\s+|the\s+)?(?:prefixo\s+|prefijo\s+|prefix\s+)?[\"\']([^\"\']+)[\"\']', source, re.I)
    if m:
        return m.group(1)
    m = re.search(r'(?:formate|format|forme)\s+(?:(?:a|o|la|el|the)\s+)?(?:mensagem|message|mensaje|payload|buffer)\s*[\"\']([^\"\']+)[\"\']', source, re.I)
    if m:
        return m.group(1)
    # First prefer explicit payload/prefix nouns.  Only then accept the compact
    # formatting form ("formate T=" / "format V=").  Keeping them separate
    # prevents "formate com prefixo T=" from accidentally capturing "com".
    m = re.search(
        r"(?:\bpayload\b|\bprefixo\b|\bprefix\b)"
        r"\s*(?:[:=]\s*|(?:como|as|with)\s+)?([A-Za-z0-9_.:+/=~\-]{1,15})",
        source, re.I)
    if not m:
        m = re.search(
            r"(?:\bformate\b|\bformat\b)\s+"
            r"(?:(?:com|with)\s+)?(?:(?:prefixo|prefix)\s+)?"
            r"([A-Za-z0-9_.:+/=~\-]{1,15})", source, re.I)
    if not m:
        return None
    value = m.group(1).rstrip(".,;!?")
    return value or None



def _quoted_prefix_before_variable(source: str, variable_words: str) -> str | None:
    # Supports payload "bateria = " mais bateria atual / "Temp=" + variável.
    for m in re.finditer(r'["\']([^"\']*)["\']', source):
        tail = _fold(source[m.end():m.end()+100])
        if re.search(variable_words, tail):
            return m.group(1)
    return None


def _serial_baud(t: str, default: int = 57600) -> int:
    m = re.search(r"(?:serial|uart)[^;.]{0,80}?\b(1200|2400|4800|9600|14400|19200|28800|38400|57600|76800|115200|230400|250000|460800|921600|1000000)\b", t)
    if not m:
        # Also accept compact "57600 8N1".
        m = re.search(r"\b(1200|2400|4800|9600|14400|19200|28800|38400|57600|76800|115200|230400|250000|460800|921600|1000000)\s+8n1\b", t)
    return int(m.group(1)) if m else default


def _tracking_interval_ms(t: str, cfg: dict[str, Any]) -> int:
    s = _scoped_interval_s(t, r"tracking|rastreamento")
    ms = int((s * 1000) if s is not None else cfg.get("tracking_interval_ms", 1000))
    if not 100 <= ms <= 10000: raise TextCompileError("tracking interval must be 100..10000 ms")
    return ms


def _emit_tracking_start(a: Assembler, ir: list[str], interval_ms: int, cfg: dict[str, Any]):
    tx = int(cfg.get("tracking_tx_power_dbm", 4)); rot = int(cfg.get("tracking_rotation_s", 10800))
    ir += [f"TRACKING CONFIG interval={interval_ms}ms tx={tx:+d}dBm", f"TRACKING ROTATION {rot}s", "TRACKING START"]
    a.emit(OP_TRACKING_CONFIG); a.u16(interval_ms); a.emit(tx)
    a.emit(OP_TRACKING_ROTATION_SET); a.u32(rot)
    a.emit(OP_TRACKING_START)


def _compile_temp_gpio(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"temperatura|temperature", t) and re.search(r"pino|pin|gpio", t)):
        return None
    cond = _temperature_threshold(t); pin = _gpio_pin(t)
    if cond is None or pin is None: return None
    if not 0 <= pin <= 31: raise TextCompileError("GPIO pin must be 0..31")
    period = _scoped_interval_s(t, r"temperatura|temperature")
    if period is None:
        # natural form: "leia a temperatura a cada 30 segundos"
        m = re.search(r"(?:leia|ler|read)[^;.]*?temperatura[^;.]*?(?:a cada|de)\s*(\d+)\s*(segundos?|s|minutos?|m|horas?|h)", t)
        if m: period = _parse_seconds_fragment(m.group(1)+m.group(2))
    if period is None:
        raise UnsupportedSemantics("temperature/GPIO control needs a sampling cadence, e.g. 'a cada 30 segundos'")
    op, threshold = cond
    cmpop = OP_CMP_GT if op == "GT" else OP_CMP_LT
    cmpname = "CMP_GT" if op == "GT" else "CMP_LT"
    a=Assembler(); ir=[]
    a.label("loop"); ir += ["LABEL LOOP", "DS18 READ -> R0", f"MOVI R1 {threshold} ; milli-degC", f"{cmpname} R2 R0 R1"]
    a.emit(OP_DS18_READ,0); a.movi(1,threshold); a.emit(cmpop,2,0,1); a.jz(2,"else")
    ir.append(f"GPIO P0.{pin} HIGH"); a.emit(OP_GPIO_WRITE_IMM,pin,1); a.jmp("wait")
    a.label("else"); ir += ["LABEL ELSE", f"GPIO P0.{pin} LOW"]
    a.emit(OP_GPIO_WRITE_IMM,pin,0)
    a.label("wait"); ir += [f"WAIT {period}s", "JMP LOOP", "END"]
    a.wait_s(period); a.jmp("loop"); a.emit(OP_END)
    return CompileResult(source,"temperature-gpio-control",schedule,ir,a.finish(),[
        "GPIO writes are accepted only on pins allowed by the compiled board profile/native GPIO arbitration."
    ])


def _compile_tracking_battery_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"tracking|rastreamento",t) and re.search(r"bateria|battery",t) and re.search(r"lora",t)):
        return None
    if not re.search(r"pare|parar|stop|desligue.*tracking|encerre.*tracking", t):
        return None
    low_cv = _battery_threshold_cv(t,cfg)
    if low_cv is None:
        raise UnsupportedSemantics("battery condition needs 'bateria baixa' (configured threshold) or an explicit voltage threshold")
    poll = int(cfg.get("battery_check_s",60))
    m = re.search(r"(?:bateria|battery)[^;.]*?(?:a cada|every)\s*(\d+)\s*(segundos?|s|minutos?|m)",t)
    if m: poll = _parse_seconds_fragment(m.group(1)+m.group(2)) or poll
    count,every = _lora_repeat(t)
    if count > 1 and every <= 0:
        raise UnsupportedSemantics("repeated LoRa sends need a cadence, e.g. '5 envios a cada 10 segundos'")
    msg = b"BATTERY_LOW"
    if len(msg)>32: raise TextCompileError("LoRa message too long")
    interval_ms = _tracking_interval_ms(t,cfg)
    a=Assembler(); ir=[]
    _emit_tracking_start(a,ir,interval_ms,cfg)
    a.label("bat_loop"); ir += ["LABEL BATTERY_MONITOR", "BAT READ -> R0", f"MOVI R1 {low_cv} ; centivolts", "CMP_LT R2 R0 R1"]
    a.emit(OP_BAT_READ,0); a.movi(1,low_cv); a.emit(OP_CMP_LT,2,0,1); a.jz(2,"battery_ok")
    ir += ["TRACKING STOP", f'LORA MESSAGE "{msg.decode()}" x{count} every {every}s']
    a.emit(OP_TRACKING_STOP)
    a.movi(4,count); a.movi(5,1); a.label("lora_loop")
    a.emit(OP_LORA_SEND_BYTES,len(msg)); a.code += msg
    # decrement R4 using SUB opcode 0x03: dst,a,b
    a.emit(OP_SUB,4,4,5)
    a.jz(4,"done")
    if every: a.wait_s(every)
    a.jmp("lora_loop")
    a.label("battery_ok"); ir += [f"WAIT {poll}s", "JMP BATTERY_MONITOR"]
    a.wait_s(poll); a.jmp("bat_loop")
    a.label("done"); ir += ["LOW-POWER IDLE (VM END)", "END"]
    a.emit(OP_END)
    return CompileResult(source,"tracking-battery-lora",schedule,ir,a.finish(),[
        "'sleep' currently lowers to VM END/normal firmware low-power idle; there is no dedicated System OFF VM opcode yet.",
        f"battery low threshold={low_cv/100:.2f}V; battery monitor cadence={poll}s",
    ])


def _compile_lora_battery_stream(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not ("lora" in t and re.search(r"bateria|battery", t) and re.search(r"payload|pacote|packet", t)):
        return None
    if not re.search(r"envie|enviando|send|transmita|transmit", t):
        return None
    prefix = _explicit_payload_prefix(source) or _quoted_prefix_before_variable(source, r"bateria(?: atual)?|battery(?: current)?")
    if prefix is None:
        return None
    period = _scoped_interval_s(t, r"lora|pacote|packet|envio|send")
    if period is None:
        m = re.search(r"(?:a cada|every)\s*(\d+)\s*(segundos?|s|minutos?|m|horas?|h)", t)
        if m: period = _parse_seconds_fragment(m.group(1)+m.group(2))
    if period is None:
        raise UnsupportedSemantics("periodic LoRa battery payload needs a cadence, e.g. 'a cada 30 segundos'")
    raw = prefix.encode("utf-8")
    if len(raw) > 48: raise TextCompileError("dynamic payload prefix must be <=48 UTF-8 bytes")
    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)
    a.label("loop"); ir += ["LABEL LOOP", "BAT READ -> R0", f'FORMAT BUFFER "{prefix}" + R0 as VOLTS', "LORA SEND BUFFER", f"WAIT {period}s", "JMP LOOP", "END"]
    a.emit(OP_BAT_READ,0)
    a.emit(OP_FORMAT_REG,len(raw)); a.code += raw; a.emit(0,FMT_CENTIVOLTS)
    a.emit(OP_LORA_SEND_BUF)
    a.wait_s(period); a.jmp("loop"); a.emit(OP_END)
    return CompileResult(source,"lora-battery-stream",schedule,ir,a.finish(),[
        "LoRa profile is applied natively before the loop; the LLCC68 returns to sleep after each TX.",
        "Battery register is centivolts and FORMAT_REG renders it as decimal volts."
    ])


def _compile_lora_rx_serial_bridge(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not ("lora" in t and re.search(r"receb|recibe|receive|listening|listen|escut|escucha|incoming|wait\s+for|route|relay|bridge", t) and re.search(r"serial|serie|uart", t)):
        return None
    if not re.search(r"retransm|reenvia|forward|encaminh|repasse|envie|envia|envialos|mande|send|write|escreva|route|relay|pass|bridge|dados? recebido|datos? recibidos?|received data", t):
        return None
    if "8n1" not in t:
        # Framing is fixed by native serial today; require explicit 8N1 only if user specified another framing.
        bad = re.search(r"\b[5678][eno][12]\b", t)
        if bad: raise UnsupportedSemantics("current SERIAL capability supports 8N1 framing only")
    baud=_serial_baud(t)
    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)
    ir.append(f"SERIAL CONFIG TX=D15 RX=D16 {baud} 8N1")
    a.emit(OP_SERIAL_CONFIG,15,16); a.u32(baud)
    a.label("rx_loop"); ir += ["LABEL RX_LOOP", "LORA RX NEXT PACKET -> BUFFER", "SERIAL WRITE BUFFER", "JMP RX_LOOP", "END"]
    a.emit(OP_LORA_RX_START,1)
    a.emit(OP_SERIAL_WRITE_BUF)
    a.jmp("rx_loop"); a.emit(OP_END)
    return CompileResult(source,"lora-rx-serial-bridge",schedule,ir,a.finish(),[
        "R3.8.16a captures one LoRa packet asynchronously, forwards its raw bytes to UART, then rearms RX.",
        "SERIAL uses the board logical D15/D16 mapping; framing is fixed at 8N1."
    ])



def _compile_landing_motion_tracking(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2g3a-r1d: landing-page motion -> Tracking standalone phrase.

    Natural `if/when motion is detected` is an event gate, not a numeric CMP/JZ
    condition.  WAIT_EVENT is therefore the deterministic control-flow primitive.
    """
    if not re.search(r'\b(?:enable|activate|start)\s+(?:the\s+)?tracking\b|\btracking\b[^.;]{0,35}\b(?:if|when)\b', t):
        return None
    if not re.search(r'\b(?:if|when)\b[^.;]{0,70}\bmotion\b[^.;]{0,40}\b(?:is\s+)?detected\b|\bmotion\b[^.;]{0,40}\b(?:detected|detection)\b', t):
        return None
    interval=_tracking_interval_ms(t,cfg)
    tx=int(cfg.get('tracking_tx_power_dbm',4)); rot=int(cfg.get('tracking_rotation_s',10800))
    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}',
           'ACCEL CONFIG mode=MOTION odr=10 fs=2 threshold=250 duration=100 low_power=1',
           'WAIT_EVENT MOTION',
           f'TRACKING CONFIG interval={interval}ms tx={tx:+d}dBm',
           f'TRACKING ROTATION {rot}s','TRACKING START','END']
    r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
    r.intent='motion-tracking-event'
    r.warnings.append('Motion condition is lowered as WAIT_EVENT MOTION; no numeric compare/branch is required.')
    return r


def _compile_landing_motion_beacon(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2g3a-r1d: motion/door-open event -> named BLE beacon."""
    if not re.search(r'beacon|broadcast|broadcasting|advertis',t):
        return None
    if not re.search(r'motion|door\s+(?:opens?|opened|opening)',t):
        return None
    if not re.search(r'\b(?:if|when)\b',t):
        return None
    quote_src=source.translate(str.maketrans({"\u2018":"'","\u2019":"'","\u201c":"\"","\u201d":"\""}))
    nm=(re.search(r'(?:beacon\s+named|named\s+beacon|name(?:d)?)[\s:=]*[\'\"]([^\'\"]{1,24})[\'\"]',quote_src,re.I)
        or re.search(r'[\'\"]([^\'\"]{1,24})[\'\"]',quote_src))
    if not nm:
        raise UnsupportedSemantics('motion-to-beacon requires an explicit beacon name')
    name=nm.group(1).strip()
    interval_ms=2000
    m=re.search(r'(?:every|interval(?:\s+of)?|a cada)\s*(\d+)\s*(milliseconds?|ms|seconds?|s)',t)
    if m:
        interval_ms=int(m.group(1)) if m.group(2).startswith(('milli','ms')) else int(m.group(1))*1000
    if not 20 <= interval_ms <= 10240:
        raise TextCompileError('Beacon interval must be 20..10240 ms on the current backend')
    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}',
           'ACCEL CONFIG mode=MOTION odr=10 fs=2 threshold=250 duration=100 low_power=1',
           'WAIT_EVENT MOTION','BLE ADVERTISER',f'BLE NAME "{name}"',
           f'BLE ADV INTERVAL={interval_ms}ms','BLE ADV START','END']
    r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
    r.intent='motion-beacon-event'
    r.warnings.append('"door opens" is deterministically mapped to the configured motion event for this landing-page phrase.')
    return r


def _compile_temperature_ble_beacon(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2g3a-r1f: periodic DS18B20 -> dynamic BLE advertising buffer."""
    if not re.search(r'temperature|temperatura', t):
        return None
    if not re.search(r'beacon|advertis|broadcast', t):
        return None
    # Richer threshold/tracking flows own their semantics.
    if re.search(r'tracking|rastreamento|above|below|greater than|less than|exceeds?', t):
        return None
    m=re.search(r'(?:every|a cada|cada)\s*(\d+)\s*(milliseconds?|ms|seconds?|s|minutes?|m)', t)
    if not m:
        raise UnsupportedSemantics('temperature-to-BLE-beacon requires an explicit update interval')
    unit=m.group(2)
    period_s=_parse_seconds_fragment(m.group(1)+unit)
    interval_ms=(int(m.group(1)) if unit.startswith(('milli','ms')) else period_s*1000)
    if not 20 <= interval_ms <= 10240:
        raise TextCompileError('BLE beacon interval must be 20..10240 ms on the current backend')
    prefix=_quoted_prefix_before_variable(source,r'temperature|temperatura') or 'T='
    raw=prefix.encode('utf-8')
    if len(raw)>20: raise TextCompileError('BLE dynamic temperature prefix too long')
    a=Assembler(); ir=['BLE ADVERTISER',f'BLE ADV INTERVAL={interval_ms}ms']
    a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER)
    a.emit(OP_BLE_ADV_CONFIG); a.u16(interval_ms); a.emit(4,0)
    a.label('temp_beacon_loop'); ir += ['LABEL TEMP_BEACON_LOOP','DS18 READ -> R0',f'FORMAT BUFFER "{prefix}" + R0 as CELSIUS','BLE ADV BUFFER',f'WAIT {period_s}s','JMP TEMP_BEACON_LOOP','END']
    a.emit(OP_DS18_READ,0)
    a.emit(OP_FORMAT_REG,len(raw)); a.code+=raw; a.emit(0,FMT_MILLICELSIUS)
    a.emit(OP_BLE_ADV_BUF); a.wait_s(period_s); a.jmp('temp_beacon_loop'); a.emit(OP_END)
    return CompileResult(source,'temperature-ble-beacon',schedule,ir,a.finish(),[
        'BLE advertising is autonomous between temperature updates; the VM sleeps between samples.'
    ])


def _compile_vib_auto_lora_beacon(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2g3a-r1f: natural vibration anomaly -> LoRa or named BLE beacon."""
    has_vib=bool(re.search(r'vibration|vibracao|vibra',t))
    has_diff=bool(re.search(r'different|anomal|abnormal|changes?\s+from|learned baseline',t))
    if not (has_vib and has_diff): return None
    has_lora='lora' in t and bool(re.search(r'send|transmit',t))
    has_ble=bool(re.search(r'beacon|advertis|broadcast',t))
    if not (has_lora or has_ble): return None
    # Do not steal the established Home Assistant family.
    if re.search(r'home assistant|\bha\b',t): return None
    learning=_vib_learning_seconds(t) or 3600
    initial=_vib_initial_seconds(t)
    if initial is None: initial=60
    quote_src=source.translate(str.maketrans({"\u2018":"'","\u2019":"'","\u201c":"\"","\u201d":"\""}))
    qm=re.search(r"['\"]([^'\"]{1,24})['\"]",quote_src)
    literal=qm.group(1) if qm else 'VIB'
    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}',
           f'VIB_AUTO CONFIG learning={learning}s initial={initial}s','VIB_AUTO START']
    if has_ble:
        interval_ms=2000
        m=re.search(r'(?:every|interval(?:\s+of)?)\s*(\d+)\s*(milliseconds?|ms|seconds?|s)',t)
        if m: interval_ms=int(m.group(1)) if m.group(2).startswith(('milli','ms')) else int(m.group(1))*1000
        if not 20 <= interval_ms <= 10240: raise TextCompileError('BLE beacon interval must be 20..10240 ms')
        lines += ['BLE ADVERTISER',f'BLE NAME "{literal}"',f'BLE ADV INTERVAL={interval_ms}ms']
    lines += ['LABEL vib_alarm_loop','WAIT_EVENT VIB_AUTO.ALARM']
    if has_lora: lines += [f'LORA SEND "{literal}"']
    if has_ble: lines += ['BLE ADV START']
    lines += ['JMP vib_alarm_loop','END']
    r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
    r.intent='vib-auto-lora-beacon'
    r.warnings += ['Natural vibration-difference wording maps to VIB_AUTO.ALARM.',
                   'Default learned-baseline profile uses learning=3600s and initial=60s when omitted.']
    return r


def _compile_landing_vibration_ha(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2g3a-r1d: HVAC/equipment vibration anomaly -> Home Assistant event."""
    has_subject=bool(re.search(r'hvac|machine|equipment|motor|pump|fan',t))
    has_vib=bool(re.search(r'vibration|vibracao|vibra',t))
    has_diff=bool(re.search(r'becomes?\s+different|different|changes?\s+from|anomal|abnormal',t))
    has_ha=bool(re.search(r'home assistant|\bha\b',t))
    has_send=bool(re.search(r'send|message|notify|notification|envie|mensagem',t))
    if not (has_subject and has_vib and has_diff and has_ha and has_send):
        return None
    # Named-machine monitoring defaults remain 1 h / 60 s, but explicit
    # VIB_AUTO learning and installation/arming delay in the prompt win.
    learning=_vib_learning_seconds(t) or 3600
    initial=_vib_initial_seconds(t)
    if initial is None: initial=60
    if not 60 <= learning <= 604800:
        raise TextCompileError("VIB_AUTO learning time must be 60..604800 seconds")
    if not 0 <= initial <= 604800:
        raise TextCompileError("VIB_AUTO initial/arming delay must be 0..604800 seconds")
    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}',
           'BLE NDP ON',f'VIB_AUTO CONFIG learning={learning}s initial={initial}s','VIB_AUTO START',
           'LABEL vib_alarm_loop','WAIT_EVENT VIB_AUTO.ALARM',
           'HA EVENT cap=13 op=4 value=R7','JMP vib_alarm_loop','END']
    r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
    r.intent='vib-auto-ha-anomaly'
    r.warnings += [
        'Natural vibration-difference wording maps to VIB_AUTO.ALARM.',
        f'VIB_AUTO profile uses learning={learning}s and initial={initial}s; explicit prompt values override the 3600s/60s defaults.',
        'Home Assistant notification is emitted through the Application/NDP event mailbox.'
    ]
    return r


def _compile_ndp_motion_ha(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"ndp|home assistant|\bha\b", t) and re.search(r"motion|moviment", t) and re.search(r"mensagem|message|evento|event", t)):
        return None
    a=Assembler(); ir=["BLE ROLE NDP/PERIPHERAL", "ACCEL MOTION threshold=250mg duration=100ms (HP/INT wake)"]
    # Peripheral role is 2 in current ABI.
    a.emit(OP_BLE_APP_ROLE,2)
    a.emit(OP_ACCEL_MOTION); a.u16(250); a.emit(1)
    a.label("loop"); ir += ["LABEL LOOP", "WAIT_EVENT MOTION -> R3,R4", "HA APP EVENT capability=MOTION value=R3 (mailbox)", "JMP LOOP", "END"]
    a.emit(OP_WAIT_EVENT,EVENT_ACCEL_MOTION,3,4)
    # capability 3 = accelerometer convention for this host-side semantic layer; operation 4=trigger/event.
    a.emit(OP_APP_EVENT_SEND,3,4,3)
    a.jmp("loop"); a.emit(OP_END)
    return CompileResult(source,"ndp-motion-ha",schedule,ir,a.finish(),[
        "APP_EVENT_SEND uses a one-slot store-and-forward mailbox if HA is temporarily disconnected."
    ])



def _compile_battery_to_beacon(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"bateria|battery", t) and re.search(r"beacon|advertis|anunc", t)):
        return None
    if not re.search(r"leia|ler|read|measure|med|lee|lea|obten|obt[eé]n", t):
        return None
    # Scope sampling cadence to the battery clause and advertising cadence to the beacon clause.
    mbat = re.search(r"(?:leia|ler|read|measure)[^;,.]{0,50}(?:bateria|battery)[^;,.]{0,50}?(?:a cada|every)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)", t)
    bat_s = _parse_seconds_fragment(mbat.group(1)+mbat.group(2)) if mbat else None
    if bat_s is None:
        mupd=re.search(r'(?:atualize|update|actualice|actualiza)[^;,.]{0,60}(?:buffer|valor|value)[^;,.]{0,40}?(?:a cada|cada|every|once every)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)',t)
        if mupd: bat_s=_parse_seconds_fragment(mupd.group(1)+mupd.group(2))
    mbeacon = re.search(r"(?:beacon|advertis\w*|anunc\w*)[^;,.]{0,80}?(?:a cada|every|intervalo(?: de)?|interval(?: of)?)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m)", t)
    if not mbeacon:
        mbeacon = re.search(r"(?:a cada|every|intervalo(?: de)?|interval(?: of)?)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m)[^;,.]{0,40}(?:beacon|advertis\w*|anunc\w*)", t)
    beacon_s = _parse_seconds_fragment(mbeacon.group(1)+mbeacon.group(2)) if mbeacon else None
    if bat_s is None: raise UnsupportedSemantics("battery-to-beacon needs a battery sampling cadence")
    if beacon_s is None: raise UnsupportedSemantics("battery-to-beacon needs a beacon advertising cadence")
    beacon_ms=beacon_s*1000
    if not 20 <= beacon_ms <= 10240: raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")
    prefix=_explicit_payload_prefix(source) or "BAT="
    raw=prefix.encode('utf-8')
    if len(raw)>48: raise TextCompileError("dynamic battery prefix must be <=48 UTF-8 bytes")
    a=Assembler(); ir=[]
    if _ndp_off_requested(t): ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)
    ir += ["BLE ADVERTISER",f"BLE ADV INTERVAL={beacon_ms}ms"]
    a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER); a.emit(OP_BLE_ADV_CONFIG); a.u16(beacon_ms); a.emit(4,0)
    nm=re.search(r'(?:nome|name|nombre)\s*[\"\']([^\"\']+)[\"\']',source,re.I)
    if nm:
        name=nm.group(1);raw_name=name.encode('utf-8')
        if len(raw_name)>20: raise TextCompileError('BLE beacon name too long')
        ir.append(f'BLE NAME "{name}"');a.emit(OP_BLE_ADV_NAME,len(raw_name));a.code+=raw_name
    ir.append('BLE ADV START');a.emit(OP_BLE_ADV_START)
    a.label('loop'); ir += ["LABEL LOOP","BAT READ -> R0",f'FORMAT BUFFER "{prefix}" + R0 as VOLTS',"BLE ADV BUFFER",f"WAIT {bat_s}s","JMP LOOP","END"]
    a.emit(OP_BAT_READ,0); a.emit(OP_FORMAT_REG,len(raw)); a.code+=raw; a.emit(0,FMT_CENTIVOLTS); a.emit(OP_BLE_ADV_BUF); a.wait_s(bat_s); a.jmp('loop'); a.emit(OP_END)
    return CompileResult(source,'battery-to-beacon',schedule,ir,a.finish(),[f'Battery is sampled every {bat_s}s; BLE advertises the last formatted value every {beacon_s}s.', 'prefix/payload is applied only to the formatted dynamic value.'])

def _compile_lora_rx_to_beacon(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Continuously receive LoRa packets and advertise the latest packet as BLE beacon data.

    R3.8.18k deliberately accepts PT/EN wording without depending on the older
    action parser/canonicalizer being able to translate every connector word.
    The advertiser is configured up-front but starts only after the first LoRa
    packet, because OP_BLE_ADV_BUF atomically replaces the advertising payload
    and starts/restarts the non-connectable advertiser.
    """
    has_rx = bool(re.search(r"\b(?:receba|recebe|receber|receb|receive|listen|listening|escute|escutar|capture|captura)\w*\b[^;.]{0,100}\blora\b|\blora\b[^;.]{0,100}\b(?:receba|recebe|receber|receb|receive|listen|listening|escute|escutar|capture|captura)\w*\b", t))
    has_beacon = bool(re.search(r"\bbeacon\b|\banunc(?:ie|iar|io|io|e|ing|ement)\w*\b|\badvertis\w*\b", t))
    has_received_flow = bool(re.search(r"(?:o que foi recebido|dados? recebidos?|valor recebido|mensagem recebida|conte[uú]do recebido|conte[uú]do do buffer|buffer recebido|received (?:data|value|message|payload|buffer)|that buffer|the buffer|every newly received buffer|what was received)", t))
    if not (has_rx and has_beacon and has_received_flow):
        return None

    # Explicit beacon cadence. Natural forms include "a cada 2 segundos",
    # "intervalo de 2 segundos" and "every 2 seconds". Default remains 2 s.
    m = re.search(r"(?:a cada|cada|em intervalos? de|once every|every|intervalo(?: de)?|interval(?: of)?|tempo(?: de)?|time(?: of)?)\s*(\d+)\s*(segundos?|seconds?|secs?|s|minutos?|minutes?|mins?|m)\b", t)
    interval_s = _parse_seconds_fragment(m.group(1) + m.group(2)) if m else 2
    interval_ms = interval_s * 1000
    if not 20 <= interval_ms <= 10240:
        raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")

    tx = 4
    mt = re.search(r"([+-]?\d+)\s*d\s*b\s*m", t)
    if mt:
        tx = int(mt.group(1))

    a = Assembler(); ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a, ir, t)
    ir += ["BLE ADVERTISER", f"BLE ADV INTERVAL={interval_ms}ms"]
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_ADVERTISER)
    a.emit(OP_BLE_ADV_CONFIG); a.u16(interval_ms); a.emit(tx & 0xff, 0)
    prefix = _explicit_payload_prefix(source)
    if prefix is not None:
        raw_prefix = prefix.encode("utf-8")
        if not (1 <= len(raw_prefix) <= 48):
            raise TextCompileError("received-buffer prefix must be 1..48 UTF-8 bytes")
    else:
        raw_prefix = b""

    a.label("rx_loop")
    ir += ["LABEL RX_LOOP", "LORA RX -> BUFFER"]
    a.emit(OP_LORA_RX_START, 1)
    if raw_prefix:
        ir.append(f'BUFFER PREPEND "{prefix}"')
        a.emit(OP_BUFFER_PREPEND, len(raw_prefix)); a.code += raw_prefix
    ir += ["BLE ADV BUFFER", "JMP RX_LOOP", "END"]
    a.emit(OP_BLE_ADV_BUF)
    a.jmp("rx_loop"); a.emit(OP_END)
    warnings = [
        "Each received LoRa packet replaces the BLE advertiser payload; the latest value is then advertised at the requested interval.",
        "BLE advertising starts after the first LoRa packet is received.",
        "BLE ADV BUFFER currently accepts up to 24 payload bytes including any explicit prefix."
    ]
    if prefix is not None:
        warnings.append(f'Received LoRa bytes are advertised with the user-supplied prefix "{prefix}".')
    return CompileResult(source, "lora-rx-to-beacon", schedule, ir, a.finish(), warnings)


def _compile_beacon_temp_lora_tracking(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Beacon + temperature monitor with high LoRa alert and low tracking branch.

    Example:
      no boot inicie beacon como TESTE e transmissão cada 5 segundos;
      se a temperatura ficar acima de 30 graus envie uma mensagem lora informando a temperatura;
      e se ficar abaixo de 10 graus inicie o tracking

    The beacon interval is not incorrectly interpreted as a temperature cadence.
    When no explicit temperature sampling cadence is supplied, this flow uses the
    beacon cadence as an *explicitly reported default* so the condition can be
    monitored continuously without inventing a hidden timer.
    """
    if not (re.search(r"beacon|anunc|advertis", t) and
            re.search(r"temperatura|temperature", t) and
            "lora" in t and re.search(r"tracking|rastreamento", t)):
        return None

    high = re.search(r"(?:acima de|maior que|superior a|above|greater than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)", t)
    low = re.search(r"(?:abaixo de|menor que|inferior a|below|less than)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)", t)
    if not (high and low):
        return None
    high_mc = int(round(float(high.group(1).replace(',', '.')) * 1000.0))
    low_mc = int(round(float(low.group(1).replace(',', '.')) * 1000.0))
    if low_mc >= high_mc:
        raise TextCompileError("low temperature threshold must be lower than high threshold")

    # Beacon name: "beacon como TESTE", "beacon TESTE", or quoted name.
    name = "nRFClaw"
    mn = re.search(r"(?:beacon|anuncio|anúncio|advertising)\s+(?:como|chamado|named|name)?\s*[\"']?([A-Za-z0-9_.-]{1,24})[\"']?", source, re.I)
    if mn:
        cand = mn.group(1)
        if _fold(cand) not in {"como","chamado","named","name"}:
            name = cand
    raw_name=name.encode('utf-8')
    if not 1 <= len(raw_name) <= 24:
        raise TextCompileError("Beacon local name must be 1..24 UTF-8 bytes")

    # Scope the beacon cadence to the first clause. This avoids stealing the
    # interval from temperature/LoRa/tracking clauses.
    beacon_clause = re.split(r"[;.]", t, maxsplit=1)[0]
    mb = re.search(r"(?:a cada|cada|every|transmissao(?: a cada)?|transmissão(?: a cada)?)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m)", beacon_clause)
    beacon_s = _parse_seconds_fragment(mb.group(1)+mb.group(2)) if mb else 5
    beacon_ms = beacon_s * 1000
    if not 20 <= beacon_ms <= 10240:
        raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")

    # Explicit temperature cadence wins. Otherwise reuse the visible beacon
    # cadence and emit a warning. This is deterministic and user-visible.
    temp_s = _scoped_interval_s(t, r"temperatura|temperature")
    defaulted_temp = temp_s is None
    if temp_s is None:
        temp_s = beacon_s
    if temp_s <= 0:
        raise TextCompileError("temperature sampling cadence must be >0")

    tx=4
    mt=re.search(r"([+-]?\d+)\s*d\s*b\s*m", t)
    if mt: tx=int(mt.group(1))

    a=Assembler(); ir=[]
    # Own BLE immediately, then configure non-connectable beacon.
    ir += ["BLE NDP OFF", "BLE ADVERTISER", f"BLE ADV INTERVAL={beacon_ms}ms", f'BLE NAME "{name}"', "BLE ADV START"]
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_ADVERTISER)
    a.emit(OP_BLE_ADV_CONFIG); a.u16(beacon_ms); a.emit(tx & 0xff, 0)
    a.emit(OP_BLE_ADV_NAME, len(raw_name)); a.code += raw_name
    a.emit(OP_BLE_ADV_START)

    # LoRa is configured once and only transmits when high threshold is entered.
    _emit_lora_config_if_requested(a,ir,t)

    # R3 is the high-zone latch: 0 = not yet alerted, 1 = alert already sent.
    a.movi(3,0); a.movi(5,0); ir += ["MOV R3 = 0", "MOV R5 = 0"]
    a.label("temp_loop"); ir += ["LABEL TEMP_LOOP", "TEMP READ -> R0"]
    a.emit(OP_DS18_READ,0)

    # Low threshold has priority: once crossed, native Tracking takes BLE ownership.
    a.movi(1,low_mc); ir.append(f"MOV R1 = {low_mc}")
    a.emit(OP_CMP_LT,2,0,1); ir.append("CMP R2 = R0 LT R1")
    a.jz(2,"check_high"); ir.append("IF R2 == 0 GOTO CHECK_HIGH")
    _emit_tracking_start(a,ir,_tracking_interval_ms(t,cfg),cfg)
    a.jmp("done"); ir.append("JMP DONE")

    # High threshold. Send one dynamic temperature alert on entry to high zone.
    a.label("check_high"); ir.append("LABEL CHECK_HIGH")
    a.movi(1,high_mc); ir.append(f"MOV R1 = {high_mc}")
    a.emit(OP_CMP_GT,2,0,1); ir.append("CMP R2 = R0 GT R1")
    a.jz(2,"normal_zone"); ir.append("IF R2 == 0 GOTO NORMAL_ZONE")
    # If latch != 0, skip repeated alert. CMP_EQ yields 0 when R3 != 0.
    a.emit(OP_CMP_EQ,4,3,5); ir.append("CMP R4 = R3 EQ R5")
    a.jz(4,"wait_next"); ir.append("IF R4 == 0 GOTO WAIT_NEXT")
    prefix="TEMP="; raw=prefix.encode('utf-8')
    a.emit(OP_FORMAT_REG,len(raw)); a.code += raw; a.emit(0,FMT_MILLICELSIUS)
    ir.append('FORMAT BUFFER "TEMP=" + R0 AS CELSIUS')
    a.emit(OP_LORA_SEND_BUF); ir.append("LORA SEND BUFFER")
    a.movi(3,1); ir.append("MOV R3 = 1")
    a.jmp("wait_next"); ir.append("JMP WAIT_NEXT")

    # <= high resets the latch so a future threshold crossing can alert again.
    a.label("normal_zone"); ir.append("LABEL NORMAL_ZONE")
    a.movi(3,0); ir.append("MOV R3 = 0")
    a.label("wait_next"); ir.append("LABEL WAIT_NEXT")
    a.wait_s(temp_s); ir += [f"WAIT {temp_s}s", "JMP TEMP_LOOP"]
    a.jmp("temp_loop")
    a.label("done"); ir.append("LABEL DONE"); a.emit(OP_END); ir.append("END")

    warnings=[
        "High-temperature LoRa alert is edge-like: one alert is sent when entering > high threshold; it rearms after temperature returns to <= high threshold.",
        "When the low threshold starts Tracking, native Tracking takes BLE advertising ownership, so the TESTE beacon is replaced by Tracking advertising.",
    ]
    if defaulted_temp:
        warnings.append(f"No temperature sampling cadence was specified; using the visible beacon cadence ({temp_s}s) as the temperature-monitor cadence.")
    return CompileResult(source,"beacon-temp-lora-tracking",schedule,ir,a.finish(),warnings)


def _compile_temperature_ble_tracking(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"temperatura|temperature",t) and re.search(r"anunc|advertis|beacon",t) and re.search(r"tracking|rastreamento",t)):
        return None
    threshold=_temperature_threshold(t)
    if threshold is None or threshold[0] != "GT": return None
    read_period=_scoped_interval_s(t,r"temperatura|temperature")
    if read_period is None:
        m=re.search(r"(?:leia|read)[^;.]*?temperatura[^;.]*?(?:a cada|every)\s*(\d+)\s*(segundos?|s|minutos?|m)",t)
        if m: read_period=_parse_seconds_fragment(m.group(1)+m.group(2))
    adv_period=_scoped_interval_s(t,r"anunc|advertis|beacon")
    if read_period is None or adv_period is None:
        raise UnsupportedSemantics("temperature advertising needs separate read and advertising cadences")
    prefix=_quoted_prefix_before_variable(source,r"variavel|variable|temperatura|temperature") or "Temp="
    raw=prefix.encode('utf-8')
    if len(raw)>20: raise TextCompileError("BLE dynamic name prefix too long")
    _,thr=threshold
    a=Assembler(); ir=["BLE ROLE ADVERTISER",f"BLE ADV interval={adv_period*1000}ms"]
    a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER)
    a.emit(OP_BLE_ADV_CONFIG); a.u16(adv_period*1000); a.emit(4,0)
    a.label("loop"); ir += ["LABEL LOOP","DS18 READ -> R0",f'FORMAT BUFFER "{prefix}" + R0 as CELSIUS',"BLE ADV UPDATE BUFFER",f"MOVI R1 {thr}","CMP_GT R2 R0 R1"]
    a.emit(OP_DS18_READ,0)
    a.emit(OP_FORMAT_REG,len(raw)); a.code+=raw; a.emit(0,FMT_MILLICELSIUS)
    a.emit(OP_BLE_ADV_BUF)
    a.movi(1,thr); a.emit(OP_CMP_GT,2,0,1); a.jz(2,"continue")
    _emit_tracking_start(a,ir,_tracking_interval_ms(t,cfg),cfg); a.jmp("done")
    a.label("continue"); ir += [f"WAIT {read_period}s","JMP LOOP"]
    a.wait_s(read_period); a.jmp("loop")
    a.label("done"); ir.append("LABEL DONE"); a.emit(OP_END); ir.append("END")
    return CompileResult(source,"temperature-ble-tracking",schedule,ir,a.finish(),[
        "BLE advertising interval is autonomous; the VM wakes only at the temperature sampling cadence.",
        "TRACKING_START takes native ownership of BLE advertising when the threshold is crossed."
    ])


def _compile_direct_gpio(source: str,t: str,schedule:int,cfg:dict[str,Any])->CompileResult|None:
    pin=_gpio_pin(t)
    if pin is None:
        return None
    # Accept natural state verbs/adjectives regardless of whether the state
    # appears before or after "pin/GPIO".
    if not re.search(r"\b(?:high|low|on|off|alto|baixo|ligue|desligue|acione|enable|disable|drive|make|turn)\b", t):
        return None
    # Word boundaries are required: "desligue" contains "ligue".
    low = bool(re.search(r"\b(?:low|off|baixo|desligue|disable)\b", t))
    high = bool(re.search(r"\b(?:high|on|alto|acione|ligue|enable)\b", t)) and not low
    a=Assembler(); ir=[f"GPIO P0.{pin} {'HIGH' if high else 'LOW'}","END"]
    a.emit(OP_GPIO_WRITE_IMM,pin,1 if high else 0,OP_END)
    return CompileResult(source,"gpio",schedule,ir,a.finish(),[])




def _compile_gpio_change_counter_threshold(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Generic GPIO state-change -> counter -> threshold -> formatted LoRa action.

    The lowerer keeps the previous GPIO sample in R1.  R2 is the current
    sample, R0 the counter, R3 constant 1, R4 threshold and R5 compare result.
    It deliberately updates previous=current only on the detected-change path.
    """
    if not ('lora' in t and re.search(r'\b(?:gpio|pin|pino)\b', t) and re.search(r'\b(?:counter|contador)\b', t)):
        return None
    if not re.search(r'\b(?:change|changed|changes|mud\w*|alter\w*)\b', t):
        return None
    pin=_gpio_pin(t)
    if pin is None: return None
    m=re.search(r'(?:counter|contador)[^.;]{0,50}?(?:reach(?:es)?|cheg\w*|ating\w*|igual(?: a)?|equals?)\s*(\d+)',t,re.I)
    if not m:
        m=re.search(r'(?:when|quando)[^.;]{0,40}?(?:counter|contador)[^.;]{0,30}?(\d+)',t,re.I)
    if not m: return None
    threshold=int(m.group(1))
    if threshold < 1: raise TextCompileError('counter threshold must be >= 1')
    prefix=_explicit_payload_prefix(source) or 'CNT='
    raw=prefix.encode('utf-8')
    if len(raw)>48: raise TextCompileError('counter prefix is too long')

    a=Assembler();ir=[]
    _emit_lora_config_if_requested(a,ir,t)
    a.movi(0,0);a.movi(3,1);a.movi(4,threshold)
    ir += ['MOV R0 = 0','MOV R3 = 1',f'MOV R4 = {threshold}',f'GPIO P0.{pin} READ -> R1','LABEL GPIO_POLL']
    a.emit(OP_GPIO_READ,pin,1);a.label('gpio_poll')
    ir += [f'GPIO P0.{pin} READ -> R2','CMP R5 = R2 EQ R1','IF R5 == 0 GOTO GPIO_CHANGED','JMP GPIO_POLL']
    a.emit(OP_GPIO_READ,pin,2);a.emit(OP_CMP_EQ,5,2,1);a.jz(5,'gpio_changed');a.jmp('gpio_poll')
    a.label('gpio_changed');ir += ['LABEL GPIO_CHANGED','MOV R1 = R2','ADD R0 = R0 + R3',f'CMP R5 = R0 EQ R4','IF R5 == 0 GOTO GPIO_POLL']
    a.emit(OP_MOV,1,2);a.emit(OP_ADD,0,0,3);a.emit(OP_CMP_EQ,5,0,4);a.jz(5,'gpio_poll')
    ir += [f'FORMAT BUFFER "{prefix}" + R0 AS U32','LORA SEND BUFFER','MOV R0 = 0','JMP GPIO_POLL','END']
    a.emit(OP_FORMAT_REG,len(raw));a.code+=raw;a.emit(0,FMT_U32);a.emit(OP_LORA_SEND_BUF);a.movi(0,0);a.jmp('gpio_poll');a.emit(OP_END)
    return CompileResult(source,'gpio-change-counter-threshold-lora',schedule,ir,a.finish(),[
        f'GPIO P0.{pin} initial state is sampled before the loop.',
        'previous=current is updated only after a state change is detected.',
        f'Counter action fires at equality with {threshold}, then resets to zero.'
    ])


def _compile_hall_counter_ble_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Generic Hall change/event -> counter -> BLE marker + formatted LoRa counter."""
    if not (re.search(r'\bhall\b',t) and re.search(r'\b(?:counter|contador)\b',t) and 'lora' in t and re.search(r'beacon|ble|bluetooth|advertis|anunc',t)):
        return None
    if not re.search(r'change|mud\w*|alter\w*|wait|espere|aguarde',t): return None
    prefix=_explicit_payload_prefix(source) or 'H='
    raw=prefix.encode('utf-8')
    if len(raw)>48: raise TextCompileError('Hall counter prefix is too long')
    # A quoted BLE marker distinct from the LoRa prefix is treated as the beacon name.
    quoted=re.findall(r'["\']([^"\']+)["\']',source)
    name=next((q for q in quoted if q != prefix and not q.endswith('=')), 'HALL')
    nraw=name.encode('utf-8')
    if not 1<=len(nraw)<=24: raise TextCompileError('BLE Hall marker must be 1..24 UTF-8 bytes')
    a=Assembler();ir=[]
    _emit_lora_config_if_requested(a,ir,t)
    a.movi(0,0);a.movi(1,1);ir += ['MOV R0 = 0','MOV R1 = 1','BLE ADVERTISER','BLE ADV INTERVAL=1000ms',f'BLE NAME "{name}"','BLE ADV START']
    a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER);a.emit(OP_BLE_ADV_CONFIG);a.u16(1000);a.emit(4,0)
    a.emit(OP_BLE_ADV_NAME,len(nraw));a.code+=nraw;a.emit(OP_BLE_ADV_START)
    a.label('hall_loop');ir += ['LABEL HALL_LOOP','WAIT_HALL -> R2','ADD R0 = R0 + R1',f'FORMAT BUFFER "{prefix}" + R0 AS U32','LORA SEND BUFFER','JMP HALL_LOOP','END']
    a.emit(OP_WAIT_HALL,2);a.emit(OP_ADD,0,0,1);a.emit(OP_FORMAT_REG,len(raw));a.code+=raw;a.emit(0,FMT_U32);a.emit(OP_LORA_SEND_BUF);a.jmp('hall_loop');a.emit(OP_END)
    return CompileResult(source,'hall-counter-ble-lora',schedule,ir,a.finish(),['One counter increment and one LoRa report are emitted for each Hall event.'])


def _compile_serial_parse_compare_gpio(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Generic SERIAL line -> integer parse -> compare -> GPIO high/low branch."""
    if not (re.search(r'\b(?:serial|uart)\b',t) and re.search(r'\b(?:gpio|pin|pino)\b',t)):
        return None
    if not re.search(r'parse|convert|converta|converter|inteiro|integer|numero|number',t): return None
    cmpm=re.search(r'(?:greater than|maior que|acima de|>)\s*(\d+)',t,re.I)
    if not cmpm: return None
    threshold=int(cmpm.group(1));pin=_gpio_pin(t)
    if pin is None:return None
    txm=re.search(r'\btx\s*(?:=|pin\s*)?(\d{1,2})\b',t,re.I);rxm=re.search(r'\brx\s*(?:=|pin\s*)?(\d{1,2})\b',t,re.I)
    tx=int(txm.group(1)) if txm else 15;rx=int(rxm.group(1)) if rxm else 16
    baud=_serial_baud(t); economy=bool(re.search(r'econom|low power|baixo consumo',t,re.I))
    a=Assembler();ir=[]
    ir.append(f'SERIAL CONFIG {"ECONOMY " if economy else ""}TX=D{tx} RX=D{rx} {baud} 8N1')
    if economy:a.emit(OP_SERIAL_CONFIG_ECO,tx,rx);a.u32(baud)
    else:a.emit(OP_SERIAL_CONFIG,tx,rx);a.u32(baud)
    a.movi(1,threshold);ir.append(f'MOV R1 = {threshold}')
    a.label('serial_cmp_loop');ir += ['LABEL SERIAL_CMP_LOOP','SERIAL RX LINE -> BUFFER','PARSE BUFFER INT -> R0',f'CMP R2 = R0 GT R1','IF R2 == 0 GOTO SERIAL_ELSE',f'GPIO P0.{pin} HIGH','JMP SERIAL_CMP_LOOP','LABEL SERIAL_ELSE',f'GPIO P0.{pin} LOW','JMP SERIAL_CMP_LOOP','END']
    a.emit(OP_SERIAL_RX_BUF,1);a.u16(0);a.emit(OP_PARSE_BUF_I32,0);a.emit(OP_CMP_GT,2,0,1);a.jz(2,'serial_else');a.emit(OP_GPIO_WRITE_IMM,pin,1);a.jmp('serial_cmp_loop');a.label('serial_else');a.emit(OP_GPIO_WRITE_IMM,pin,0);a.jmp('serial_cmp_loop');a.emit(OP_END)
    return CompileResult(source,'serial-parse-compare-gpio',schedule,ir,a.finish(),[f'Parsed integer is compared against {threshold}; GPIO P0.{pin} is HIGH only when GT is true.'])


def _compile_gpio_compare_app_else_wait(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Generic GPIO comparison -> app action, else wait, then loop."""
    if not (re.search(r'\b(?:gpio|pin)\b',t) and re.search(r'application|aplicacao|aplicación|\bapp\b',t)):
        return None
    if not re.search(r'continu|repeat|repita|again|novamente',t): return None
    pin=_gpio_pin(t)
    if pin is None:return None
    # Current generic ABI contract for this family is equality to zero.
    if not re.search(r'(?:is|for|igual(?: a)?|==?)\s*(?:zero|0)\b|when it is zero|quando.*(?:zero|0)',t,re.I): return None
    waitm=re.search(r'(?:wait|aguarde|espere)\s*(\d+)\s*(seconds?|segundos?|s)',t,re.I)
    seconds=int(waitm.group(1)) if waitm else 1
    a=Assembler();ir=[];a.movi(1,0);ir.append('MOV R1 = 0');a.label('gpio_app_loop');ir += ['LABEL GPIO_APP_LOOP',f'GPIO P0.{pin} READ -> R0','CMP R2 = R0 EQ R1','IF R2 == 0 GOTO GPIO_APP_ELSE','APP SEND BUFFER','JMP GPIO_APP_LOOP','LABEL GPIO_APP_ELSE',f'WAIT {seconds}s','JMP GPIO_APP_LOOP','END']
    a.emit(OP_GPIO_READ,pin,0);a.emit(OP_CMP_EQ,2,0,1);a.jz(2,'gpio_app_else');a.emit(OP_APP_SEND_BUF);a.jmp('gpio_app_loop');a.label('gpio_app_else');a.wait_s(seconds);a.jmp('gpio_app_loop');a.emit(OP_END)
    return CompileResult(source,'gpio-compare-app-else-wait',schedule,ir,a.finish(),[f'GPIO P0.{pin} is re-read after each branch; ELSE waits {seconds}s.'])


def _compile_gpio_high_temp_battery_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """GPIO-high trigger -> read temperature+battery -> one combined LoRa payload.

    R3.8.18h intentionally precedes the generic temperature-stream lowerer so
    BATTERY and GPIO condition semantics cannot be discarded.  "Continuously"
    is implemented as a 1 s low-power poll with high-level deglitch/re-arm: one
    report per low->high activation, then wait for the pin to return low.
    """
    if not ("lora" in t and re.search(r"temperatura|temperature", t) and re.search(r"bateria|battery", t)):
        return None
    pm = re.search(r"(?:pino|pin|gpio)\s*(?:p0[.]?)?(\d{1,2})", t)
    if not pm:
        return None
    if not re.search(r"(?:alto|high)", t):
        return None
    if not re.search(r"(?:caso|se|if|when|quando)[^.;]{0,80}(?:alto|high)|(?:fique|ficar|goes?|becomes?)\s+(?:alto|high)", t):
        return None
    if not re.search(r"send|envie|envia|enviar|transmita|manda|mande", t):
        return None
    pin=int(pm.group(1))
    if not 0 <= pin <= 31:
        raise TextCompileError("GPIO pin must be 0..31")
    p1=b"TEMP="; p2=b",BAT="
    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a, ir, t)
    a.label("wait_high")
    ir += ["LABEL WAIT_HIGH", f"GPIO P0.{pin} READ -> R2", "IF R2 == 0 GOTO LOW_DELAY",
           "DS18 READ -> R0", "BAT READ -> R1",
           'FORMAT BUFFER "TEMP=" + R0 AS CELSIUS + ",BAT=" + R1 AS VOLTS',
           "LORA SEND BUFFER", "LABEL WAIT_LOW", f"GPIO P0.{pin} READ -> R2",
           "IF R2 == 0 GOTO WAIT_HIGH", "WAIT 1s", "JMP WAIT_LOW",
           "LABEL LOW_DELAY", "WAIT 1s", "JMP WAIT_HIGH", "END"]
    a.emit(OP_GPIO_READ,pin,2); a.jz(2,"low_delay")
    a.emit(OP_DS18_READ,0); a.emit(OP_BAT_READ,1)
    a.emit(OP_FORMAT_2REG,len(p1)); a.code += p1; a.emit(0,FMT_MILLICELSIUS,len(p2)); a.code += p2; a.emit(1,FMT_CENTIVOLTS)
    a.emit(OP_LORA_SEND_BUF)
    a.label("wait_low"); a.emit(OP_GPIO_READ,pin,2); a.jz(2,"wait_high"); a.wait_s(1); a.jmp("wait_low")
    a.label("low_delay"); a.wait_s(1); a.jmp("wait_high")
    a.emit(OP_END)
    return CompileResult(source,"gpio-high-temp-battery-lora",schedule,ir,a.finish(),[
        f"GPIO P0.{pin} is monitored once per second to keep idle current low.",
        "One LoRa report is emitted per low-to-high activation; the trigger rearms after the pin returns low.",
        'Payload format is ASCII: TEMP=<degC>,BAT=<V>.'
    ])


def _compile_temp_battery_periodic_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Generic two-sensor periodic LoRa report: temperature + battery -> one buffer."""
    if not (re.search(r'temperatura|temperature',t) and re.search(r'bateria|battery',t) and 'lora' in t):
        return None
    if not re.search(r'envie|enviar|send|transmit|report|forward|transmita|manda|mande',t):
        return None
    period=None
    m=re.search(r'(?:a cada|every|intervalos? de|once every)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)',t)
    if m: period=_parse_seconds_fragment(m.group(1)+m.group(2))
    if period is None:
        m=re.search(r'(?:wait|aguarde|espere|pause)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)[^.;]{0,50}(?:repeat|repita|repetindo|forever|continu)',t)
        if m: period=_parse_seconds_fragment(m.group(1)+m.group(2))
    if period is None and re.search(r'continuamente|continuously|sin parar|repetindo|repeat',t):
        raise UnsupportedSemantics('temperature+battery LoRa loop needs an explicit cadence')
    p1=b'T='; p2=b',B='
    a=Assembler();ir=[]
    _emit_lora_config_if_requested(a,ir,t)
    if period:
        a.label('loop');ir.append('LABEL LOOP')
    ir += ['DS18 READ -> R0','BAT READ -> R1','FORMAT BUFFER "T=" + R0 AS CELSIUS + ",B=" + R1 AS VOLTS','LORA SEND BUFFER']
    a.emit(OP_DS18_READ,0);a.emit(OP_BAT_READ,1)
    a.emit(OP_FORMAT_2REG,len(p1));a.code+=p1;a.emit(0,FMT_MILLICELSIUS,len(p2));a.code+=p2;a.emit(1,FMT_CENTIVOLTS)
    a.emit(OP_LORA_SEND_BUF)
    if period:
        ir += [f'WAIT {period}s','JMP LOOP'];a.wait_s(period);a.jmp('loop')
    ir.append('END');a.emit(OP_END)
    return CompileResult(source,'temp-battery-periodic-lora',schedule,ir,a.finish(),['Both sensors are sampled immediately before each LoRa transmission.'])


def _compile_minpower_hall_battery_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Minimum-power event loop: Hall wake -> battery report -> return to Hall wait."""
    if not (re.search(r'minimum[- ]power|minimo consumo|mínimo consumo|consumo minimo|consumo mínimo',t) and re.search(r'hall',t) and re.search(r'bateria|battery',t) and 'lora' in t):
        return None
    prefix=_explicit_payload_prefix(source) or 'B='
    raw=prefix.encode('utf-8')
    a=Assembler();ir=['SYSTEM MINIMUM POWER'];a.emit(OP_SYSTEM_MIN_POWER)
    a.label('loop');ir += ['LABEL LOOP','HALL WAIT','BAT READ -> R0',f'FORMAT BUFFER "{prefix}" + R0 AS VOLTS','LORA SEND BUFFER','JMP LOOP','END']
    a.emit(OP_WAIT_HALL,0);a.emit(OP_BAT_READ,0);a.emit(OP_FORMAT_REG,len(raw));a.code+=raw;a.emit(0,FMT_CENTIVOLTS);a.emit(OP_LORA_SEND_BUF);a.jmp('loop');a.emit(OP_END)
    return CompileResult(source,'minpower-hall-battery-lora',schedule,ir,a.finish(),['P0.21 remains available by firmware policy.'])

def _compile_lora_temperature_stream(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Periodic/current temperature -> formatted LoRa payload.

    This lowerer is intentionally more specific than direct-lora so clauses
    such as "disable NDP at boot and every 30 seconds send the temperature"
    cannot be swallowed by the generic static-message path.
    """
    if not ("lora" in t and re.search(r"temperature|temperatura", t)):
        return None
    if not re.search(r"send|transmit|report|forward|envie|envia|enviar|transmita|manda|mande", t):
        return None
    # Require semantics that the LoRa payload contains/reports temperature.
    if not re.search(r"containing|informando|temperature|temperatura", t):
        return None

    period = _scoped_interval_s(t, r"lora|message|mensagem|send|envie|temperature|temperatura")
    if period is None:
        m = re.search(r"(?:every|a cada)\s*(\d+)\s*(seconds?|secs?|s|minutes?|mins?|m|hours?|h|segundos?|minutos?|horas?)", t)
        if m:
            period = _parse_seconds_fragment(m.group(1) + m.group(2))
    if period is None:
        m = re.search(r'(?:wait|aguarde|espere|pause)\s*(\d+)\s*(seconds?|secs?|s|minutes?|mins?|m|hours?|h|segundos?|minutos?|horas?)[^.;]{0,60}(?:repeat|repita|repetindo|forever|continu)', t)
        if m:
            period = _parse_seconds_fragment(m.group(1) + m.group(2))
    if period is None:
        # A one-shot request with temperature is still well-defined.
        period = 0

    prefix = _explicit_payload_prefix(source) or _quoted_prefix_before_variable(source, r"temperature|temperatura") or "TEMP="
    raw = prefix.encode("utf-8")
    if len(raw) > 48:
        raise TextCompileError("dynamic temperature payload prefix must be <=48 UTF-8 bytes")

    a = Assembler(); ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a, ir, t)
    if period:
        a.label("loop"); ir.append("LABEL LOOP")
    ir += ["DS18 READ -> R0", f'FORMAT BUFFER "{prefix}" + R0 as CELSIUS', "LORA SEND BUFFER"]
    a.emit(OP_DS18_READ, 0)
    a.emit(OP_FORMAT_REG, len(raw)); a.code += raw; a.emit(0, FMT_MILLICELSIUS)
    a.emit(OP_LORA_SEND_BUF)
    if period:
        ir += [f"WAIT {period}s", "JMP LOOP"]
        a.wait_s(period); a.jmp("loop")
    ir.append("END"); a.emit(OP_END)
    warnings = [
        f'No explicit dynamic payload prefix was supplied; using "{prefix}".' if prefix == "TEMP=" else "LoRa temperature payload uses the user-supplied prefix.",
        "Temperature is read from DS18B20 immediately before each LoRa transmission.",
    ]
    return CompileResult(source, "lora-temperature-stream", schedule, ir, a.finish(), warnings)

def _compile_ndp_role_only(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Lower a standalone NDP/BLE role control request.

    This prevents a valid request such as "disable NDP at boot" from falling
    through to the generic AST diagnostic merely because it has no second action.
    """
    if not _ndp_off_requested(t):
        return None
    # Only claim a role-only prompt. Richer compositions must be handled by the
    # more-specific lowerers that appear earlier in the dispatch table.
    if re.search(r"lora|temperature|temperatura|tracking|rastreamento|vib[_ ]?auto|vibration|hall|reed|gpio|serial|uart|motion|moviment|beacon|advertis", t):
        return None
    a=Assembler(); ir=["BLE NDP OFF", "END"]
    a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    a.emit(OP_END)
    return CompileResult(source, "ndp-role-off", schedule, ir, a.finish(), [
        "NDP/Application BLE is disabled by the VM program when it starts."
    ])




# NRFCLAW_CLI_C2E_BASIC_BATTERY_FIX_V1
def _compile_basic_battery_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Safe one-shot battery -> formatted buffer -> LoRa path.

    Prevents a simple battery request from falling through to generic static
    LoRa and inventing the legacy default payload "NRFCLAW".
    More complex/periodic flows remain owned by their existing c2e lowerers.
    """
    if not ("lora" in t and re.search(r"\b(?:bateria|battery)\b", t)):
        return None
    # "battery reading" already denotes a measurement even when the request
    # starts directly with SEND (e.g. "Send the battery reading via LoRa").
    has_read = bool(re.search(r"\b(?:leia|ler|lee|read|get|measure|medir|mide|meça|meca)\b", t))
    # Measurement nouns also imply a read when the user asks to send/report them.
    has_reading_noun = bool(re.search(
        r"\b(?:battery\s+(?:reading|voltage|level)|leitura\s+da\s+bateria|lectura(?:\s+de\s+la)?\s+bateria)\b", t
    ))
    if not (has_read or has_reading_noun):
        return None
    if not re.search(r"\b(?:envie|envia|enviala|envialos|enviar|send|transmita|transmite|transmit|mande|manda|report|forward)\b", t):
        return None
    # Do not steal compound flows from existing specific lowerers.
    if re.search(r"\b(?:temperature|temperatura|gpio|hall|serial|uart|beacon|tracking|vib_auto|motion|fall|receive|receba|receber)\b", t):
        return None
    if re.search(r"\b(?:every|a cada|cada)\b", t):
        return None

    prefix = _explicit_payload_prefix(source) if '_explicit_payload_prefix' in globals() else None
    if prefix is None:
        m = re.search(r"(?:\bprefixo\b|\bprefix\b|\bpayload\b)\s*(?:[:=]\s*|(?:como|as|with)\s+)?([A-Za-z0-9_.:+/=~\-]{1,15})", source, re.I)
        if m:
            prefix = m.group(1)
    if prefix is None:
        prefix = "B="
    raw = prefix.encode("utf-8")
    if not 1 <= len(raw) <= 48:
        raise TextCompileError("dynamic battery payload prefix must be 1..48 UTF-8 bytes")

    a = Assembler(); ir = []
    if '_ndp_off_requested' in globals() and _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    if '_emit_lora_config_if_requested' in globals():
        _emit_lora_config_if_requested(a, ir, t)
    elif '_lora_rf_config_requested' in globals() and _lora_rf_config_requested(t):
        _emit_lora_config(a, ir, _lora_profile(t))
    ir += ["BAT READ -> R0", f'FORMAT BUFFER "{prefix}" + R0 as VOLTS', "LORA SEND BUFFER", "END"]
    a.emit(OP_BAT_READ, 0)
    a.emit(OP_FORMAT_REG, len(raw)); a.code += raw; a.emit(0, FMT_CENTIVOLTS)
    a.emit(OP_LORA_SEND_BUF)
    a.emit(OP_END)
    return CompileResult(source, "lora-battery-basic", schedule, ir, a.finish(), [
        "Battery is read immediately before the LoRa transmission.",
        "No RF profile is emitted unless explicitly requested; persisted/current LoRa settings remain authoritative.",
    ])

def _compile_direct_lora(source:str,t:str,schedule:int,cfg:dict[str,Any])->CompileResult|None:
    if "lora" not in t or not re.search(r"envie|envia|enviar|send|transmit|transmita|manda|mande",t): return None
    # RX/bridge semantics must never be misclassified as a static TX message.
    if re.search(r"receb|receive|listening|escut", t): return None

    # Accept the natural payload nouns used in PT/ES/EN prompts.  Historically
    # this lowerer only recognized mensagem/message, so e.g.
    #   envie a string 'teste' pelo LoRa
    # silently fell back to NRFCLAW.
    m=re.search(
        r"(?:mensagem|mensaje|message|string|texto|text|payload)\s*(?:[:=]\s*)?[\"']([^\"']+)[\"']",
        source, re.I)
    if m is None:
        # Also accept a directly quoted object after the TX verb:
        #   envie 'teste' pelo LoRa
        m=re.search(r"(?:envie|envia|enviar|send|transmit|transmita|manda|mande)\s+(?:a\s+|o\s+)?[\"']([^\"']+)[\"']", source, re.I)
    if m is None:
        # R3.8.18f: an explicit payload noun does not require quotation marks.
        # Examples:
        #   envie a string teste pelo lora
        #   send payload hello via lora
        # Capture conservatively up to the LoRa routing clause, cadence, or
        # end of sentence. Quoted forms above remain authoritative.
        m=re.search(
            r"(?:mensagem|mensaje|message|string|texto|text|payload)\s*(?:[:=]\s*)?"
            r"([^;,.]+?)"
            r"(?=\s+(?:pelo|pela|por|via|over|through|usando|using)\s+(?:o\s+)?lora\b"
            r"|\s+(?:a\s+cada|every)\s+\d+"
            r"|\s+(?:x|vezes|times)\s*\d+"
            r"|$)",
            source, re.I)
        if m is not None and re.fullmatch(
                r"(?:lora|(?:pelo|pela|por|via|over|through|usando|using)\s+(?:o\s+)?lora)",
                m.group(1).strip(), re.I):
            m = None
    payload=m.group(1).strip() if m else "NRFCLAW"
    raw=payload.encode("utf-8")
    if not 1<=len(raw)<=32: raise TextCompileError("LoRa static message must be 1..32 UTF-8 bytes")
    count,every=_lora_repeat(t)

    # R3.8.18g: a cadence without an explicit finite repeat count means a
    # continuous periodic stream.  Example: "envie 'teste' via lora a cada
    # 15 segundos".  Previously _lora_repeat() returned count=1/every=0 for
    # this natural form, so the cadence was silently discarded.
    periodic_every = 0
    cadence = re.search(
        r"(?:a cada|cada|every)\s+(\d+)\s*"
        r"(segundos?|seconds?|secs?|s|minutos?|minutes?|mins?|m|horas?|hours?|h)\b",
        t)
    if cadence:
        periodic_every = _parse_seconds_fragment(cadence.group(1) + cadence.group(2)) or 0

    repeat_num = r"(?:\d+|um|uma|dois|duas|tres|três|quatro|cinco|seis|sete|oito|nove|dez|one|two|three|four|five|six|seven|eight|nine|ten)"
    explicit_finite_repeat = bool(re.search(
        rf"(?:faca|faça|realize|envie)\s*{repeat_num}\s*(?:envios?|vezes|transmissoes?)"
        rf"|{repeat_num}\s*(?:envios?|vezes|transmissoes?)",
        t))
    continuous_periodic = periodic_every > 0 and not explicit_finite_repeat and count == 1

    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)

    if continuous_periodic:
        a.label("loop"); ir.append("LABEL LOOP")
        ir.append(f'LORA SEND "{payload}"')
        a.emit(OP_LORA_SEND_BYTES,len(raw)); a.code+=raw
        ir += [f"WAIT {periodic_every}s", "JMP LOOP"]
        a.wait_s(periodic_every); a.jmp("loop")
    else:
        if count>1:
            a.movi(4,count); a.movi(5,1); a.label("loop")
        ir.append(f'LORA SEND "{payload}" x{count} every {every}s')
        a.emit(OP_LORA_SEND_BYTES,len(raw)); a.code+=raw
        if count>1:
            a.emit(OP_SUB,4,4,5); a.jz(4,"done")
            if every: a.wait_s(every)
            a.jmp("loop"); a.label("done")
    a.emit(OP_END); ir.append("END")
    warnings=[]
    if m is None: warnings.append('No explicit LoRa payload was supplied; using default payload "NRFCLAW".')
    return CompileResult(source,"lora-message",schedule,ir,a.finish(),warnings)


def _compile_hall_wait(source:str,t:str,schedule:int,cfg:dict[str,Any])->CompileResult|None:
    if not re.search(r"hall|reed",t) or not re.search(r"aguarde|espere|quando|wait",t): return None
    # WAIT_HALL waits for the native Hall subsystem event. Configuration remains a separate capability.
    if not re.search(r"tracking|rastreamento",t): return None
    a=Assembler(); ir=["WAIT HALL -> R0"]
    a.emit(OP_WAIT_HALL,0)
    _emit_tracking_start(a,ir,_tracking_interval_ms(t,cfg),cfg)
    a.emit(OP_END); ir.append("END")
    return CompileResult(source,"hall-tracking",schedule,ir,a.finish(),[
        "HALL_CONFIG must already match the desired channel/mode; this flow only waits for the native Hall event."
    ])



@dataclass
class SemanticAST:
    source: str
    schedule: str
    actions: list[dict[str, Any]]
    conditions: list[dict[str, Any]]
    events: list[str]
    capabilities: list[str]
    missing_capabilities: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "schedule": self.schedule,
            "actions": self.actions,
            "conditions": self.conditions,
            "events": self.events,
            "capabilities": self.capabilities,
            "missing_capabilities": self.missing_capabilities,
        }


def parse_semantic_ast(source: str, force_boot: bool = False) -> SemanticAST:
    """R3.8.16b generic clause/entity pass.

    It is intentionally conservative: it records what the text says, and reports
    runtime gaps separately instead of calling the language itself unsupported.
    """
    t = _fold(source)
    actions: list[dict[str, Any]] = []
    conditions: list[dict[str, Any]] = []
    events: list[str] = []
    caps: set[str] = set()
    missing: set[str] = set()
    schedule = "BOOT" if (force_boot or _is_boot(t)) else "MANUAL"

    if _ndp_off_requested(t):
        actions.append({"op":"ble.role", "role":"off"}); caps.add("BLE_ROLE")
    elif re.search(r"modo ndp|conexao ndp|home assistant|\bha\b", t):
        actions.append({"op":"ble.role", "role":"ndp"}); caps.add("BLE_NDP")

    if re.search(r"beacon|anunc|advertis", t):
        caps.add("BLE_BEACON")
        first_clause = re.split(r"[;.]", t, maxsplit=1)[0]
        cadence = None
        m = re.search(r"(?:a cada|cada|every|transmissao(?: a cada)?|transmissão(?: a cada)?)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m)", first_clause)
        if m:
            cadence = _parse_seconds_fragment(m.group(1)+m.group(2))
        actions.append({"op":"ble.beacon.start", "interval_ms": (cadence*1000 if cadence else None)})

    if re.search(r"vib[_ ]?auto|vibracao|vibration|vibracion|detec(?:cao|ção) de vibracao|deteccion de vibracion|vibration detection", t) or _vib_alarm_requested(t):
        actions.append({"op":"vib_auto.start"}); caps.add("VIB_AUTO")
        _vl=_vib_learning_seconds(t); _vi=_vib_initial_seconds(t)
        if _vl is not None or _vi is not None:
            actions.append({"op":"vib_auto.configure_time","learning_s":_vl,"initial_s":_vi})
        if _vib_alarm_requested(t): events.append("VIB_AUTO.ALARM")
        if re.search(r"warning|aviso", t): events.append("VIB_AUTO.WARNING")
        if re.search(r"comec(?:ar|ou)|começar|entrou em funcionamento|maquina ligar|máquina ligar", t): events.append("VIB_AUTO.MACHINE_ON")
        if re.search(r"maquina (?:deslig|par)|máquina (?:deslig|par)|quando ela parar", t): events.append("VIB_AUTO.MACHINE_OFF")

    if re.search(r"movimento|movimentacao|movimentação|motion", t):
        caps.add("ACCEL_MOTION")
        if re.search(r"quando|aguarde|espere", t): events.append("MOTION")

    if "temperatura" in t or "temperature" in t:
        caps.add("DS18B20")
        cadence = _scoped_interval_s(t, r"temperatura|temperature")
        if cadence is None:
            m = re.search(r"temperatura[^;.]*?a cada\s+(?:(\d+)\s*)?(segundos?|s|minutos?|m|horas?|h)", t)
            if m: cadence = _parse_seconds_fragment((m.group(1) or "1")+m.group(2))
        actions.append({"op":"sensor.read", "sensor":"temperature", "cadence_s":cadence})
        for op,pat in (("GT",r"(?:acima de|maior que|ultrapass(?:ar|e)|passar de)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)"),("LT",r"(?:abaixo de|menor que)\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)")):
            for m in re.finditer(pat,t): conditions.append({"subject":"temperature","op":op,"value_mC":int(float(m.group(1).replace(',','.'))*1000)})
        for m in re.finditer(r"entre\s*(-?\d+(?:[\.,]\d+)?)\s*e\s*(-?\d+(?:[\.,]\d+)?)\s*(?:graus?|c|celsius)",t):
            conditions.append({"subject":"temperature","op":"RANGE","min_mC":int(float(m.group(1).replace(',','.'))*1000),"max_mC":int(float(m.group(2).replace(',','.'))*1000)})

    if "bateria" in t or "battery" in t:
        caps.add("BATTERY")
        actions.append({"op":"sensor.read", "sensor":"battery", "cadence_s":_scoped_interval_s(t,r"bateria|battery")})
        # Discourse inheritance: after battery is established, bare "abaixo de X volts" still means battery.
        for m in re.finditer(r"(?:abaixo de|menor que)\s*(\d+(?:[\.,]\d+)?)\s*(?:v|volts?)\b",t):
            conditions.append({"subject":"battery","op":"LT","value_cV":int(round(float(m.group(1).replace(',','.'))*100))})

    if "tracking" in t or "rastreamento" in t:
        caps.add("TRACKING")
        if re.search(r"pare|parar|stop|desligue.*tracking",t): actions.append({"op":"tracking.stop"})
        if re.search(r"inicie|ative|comece|tracking a cada|rastreamento",t):
            # A tracking rotation period is not the radio/advertising tracking interval.
            # Keep the configured runtime interval when the clause only specifies rotation.
            if re.search(r"(?:rotacao|rotação|rotation|rotacion)[^.;]{0,24}\d+",t):
                _ti=int(load_config().get("tracking_interval_ms",1000))
            else:
                _ti=_tracking_interval_ms(t,load_config())
            actions.append({"op":"tracking.start","interval_ms":_ti})

    if "lora" in t:
        caps.add("LORA")
        p=_lora_profile(t)
        actions.append({"op":"lora.configure","frequency_hz":p[0],"tx_power_dbm":p[1],"sf":p[2],"bw_khz":p[3],"cr":p[4]})
        if re.search(r"receb|escut|receive",t): actions.append({"op":"lora.receive"})
        if re.search(r"envie|envia|enviar|transmita|send|responda",t):
            send={"op":"lora.send"}
            cadence=_scoped_interval_s(t, r"lora|mensagem|message|envie|envia|send|transmita")
            if cadence is None:
                # A global cadence may belong to another transport (e.g. Beacon).
                # Accept a leading generic cadence only when LoRa is part of the
                # same leading clause.
                first_clause = re.split(r"[;.]", t, maxsplit=1)[0]
                if "lora" in first_clause:
                    m=re.search(r"(?:a cada|every|cada)\s*(?:(\d+)\s*)?(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)\b",first_clause)
                    if m: cadence=_parse_seconds_fragment((m.group(1) or "1")+m.group(2))
            if cadence is not None: send["cadence_s"]=cadence
            actions.append(send)
        # Packet-content branching needs a generic buffer comparator not present in ABI1.
        if re.search(r"quando receber (?:a mensagem|o pacote)|se o pacote|come[cç]ar com",t): missing.add("BUFFER_COMPARE")

    if re.search(r"serial|uart",t):
        caps.add("SERIAL")
        actions.append({"op":"serial.configure","baud":_serial_baud(t),"framing":"8N1"})
        if re.search(r"encaminhe|envie.*serial|porta serial",t): actions.append({"op":"serial.write"})

    if re.search(r"hall\s*1|hall1",t):
        caps.add("HALL1"); actions.append({"op":"hall.monitor","channel":1})
    if re.search(r"hall\s*2|hall2",t):
        caps.add("HALL2"); actions.append({"op":"hall.monitor","channel":2})
    if re.search(r"direcao|direção|frente|reverso",t) and "HALL1" in caps and "HALL2" in caps:
        missing.add("HALL_QUADRATURE_DIRECTION_VALIDATION")

    if re.search(r"contador|counter|contado\b",t):
        persistent=bool(re.search(r"contador persistente|counter persistent|continue do valor anterior|mesmo depois de desligar",t))
        caps.add("PERSISTENT_STATE" if persistent else "ARITHMETIC")
        actions.append({"op":"state.counter","persistent":persistent,"initial":0,"step":1})

    if re.search(r"hora de inicio|hora de início|quanto tempo|duracao|duração",t):
        missing.add("MONOTONIC_TIME")

    if re.search(r"mensagem.*ha|evento.*ha|home assistant",t): caps.add("HA_EVENT")
    if re.search(r"conte[uú]do recebido",t) and re.search(r"quando receber|se o pacote",t): missing.add("BUFFER_COMPARE")

    return SemanticAST(source,schedule,actions,conditions,events,sorted(caps),sorted(missing))



def _compile_lora_test_boot(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Autonomous LoRa bench-test stream with zero-padded counter payload.

    Natural examples:
      - desligue a transmissao NDP no boot e inicie o teste lora com transmissao a cada 15 segundos
      - at boot disable NDP and start the LoRa test transmitting every 15 seconds

    This intentionally mirrors the dedicated ``lora-tx-boot`` CLI behavior:
    it uses the already persisted RF profile and does not emit OP_LORA_CONFIG.
    The first packet is sent immediately when the BOOT VM starts and is TEST-01,
    followed by TEST-02, ... at the requested cadence.
    """
    if "lora" not in t:
        return None
    if not re.search(r"(?:teste|test)(?:\s+de)?\s+lora|lora\s+(?:teste|test)", t):
        return None
    if not re.search(r"inicie|iniciar|inicia|start|comece|comecar|começar|execute|executar|rode|rodar", t):
        return None

    period = _scoped_interval_s(t, r"lora|teste|test|transmissao|transmission|transmit|envie|send")
    if period is None:
        m = re.search(r"(?:a cada|every|cada)\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)\b", t)
        if m:
            period = _parse_seconds_fragment(m.group(1) + m.group(2))
    if period is None or period <= 0:
        raise UnsupportedSemantics("LoRa boot test needs a cadence, e.g. 'a cada 15 segundos'")

    # A boot test is only autonomous if persisted as BOOT.  Natural wording
    # containing explicit boot semantics already selects SCHED_BOOT; refuse to
    # silently promote a MANUAL request here.
    if schedule != SCHED_BOOT:
        return None

    # Optional quoted base, otherwise use the same canonical base as the
    # dedicated diagnostic command. Keep room for '-NN'.
    q = re.search(r'["\']([^"\']+)["\']', source)
    base = q.group(1) if q else "test"
    prefix = base + "-"
    raw = prefix.encode("utf-8")
    if not raw or len(raw) > 48:
        raise TextCompileError("LoRa test payload base is too long")

    a = Assembler(); ir = []
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)

    # Deliberately no LORA CONFIG here: use the persisted RF profile exactly as
    # lora-tx-boot does. R0=count, R1=constant one.
    a.movi(0, 1); a.movi(1, 1)
    ir += ["MOV R0 = 1", "MOV R1 = 1", "LABEL LOOP"]
    a.label("loop")
    ir.append(f'FORMAT BUFFER "{prefix}" + R0 AS U32_02')
    a.emit(OP_FORMAT_REG, len(raw)); a.code += raw; a.emit(0, FMT_U32_02)
    ir.append("LORA SEND BUFFER"); a.emit(OP_LORA_SEND_BUF)
    ir.append("ADD R0 = R0 + R1"); a.emit(OP_ADD, 0, 0, 1)
    ir.append(f"WAIT {period}s"); a.wait_s(period)
    ir += ["JMP LOOP", "END"]; a.jmp("loop"); a.emit(OP_END)

    return CompileResult(source, "lora-test-boot-counter", schedule, ir, a.finish(), [
        "Uses the persisted LoRa RF profile; the VM does not overwrite frequency/SF/BW/CR/power/sync/preamble.",
        f"First packet is {base}-01 immediately after BOOT VM start; subsequent packets are spaced by {period}s.",
        "Counter is volatile and restarts at 01 after reset/power loss.",
    ])

def _compile_lora_counter_stream(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Periodic LoRa message containing a monotonically increasing counter.

    Natural examples:
      - a cada 20 segundos envia uma mensagem lora com um contador
      - every 20 seconds send a LoRa message with a counter
      - cada 20 segundos envia por LoRa un mensaje con contador

    Counter is volatile unless persistence is explicitly requested. The first
    transmission occurs after one interval and carries COUNT=1.
    """
    if not ("lora" in t and re.search(r"contador|counter|contado\b", t) and re.search(r"envie|envia|enviar|send|transmita|manda", t)):
        return None
    # Persistence is intentionally a separate semantic contract; do not silently
    # turn a plain counter into flash writes.
    if re.search(r"persistente|persistent|continue do valor anterior|mesmo depois de desligar", t):
        return None
    period = _scoped_interval_s(t, r"lora|mensagem|mensaje|message|envie|envia|send|contador|counter|contado")
    if period is None:
        # Generic leading cadence: "a cada 20 segundos envie ..."
        m=re.search(r"(?:a cada|every|cada)\s*(?:(\d+)\s*)?(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)\b", t)
        if m: period=_parse_seconds_fragment((m.group(1) or "1")+m.group(2))
    if period is None or period <= 0:
        raise UnsupportedSemantics("periodic LoRa counter needs a cadence, e.g. 'a cada 20 segundos'")
    prefix = "COUNT="
    # Respect a quoted prefix when supplied, e.g. mensagem "CONTADOR=" com contador.
    q=re.search(r'["\']([^"\']*)["\']', source)
    if q and len(q.group(1).encode("utf-8")) <= 48:
        prefix=q.group(1)
    raw=prefix.encode("utf-8")
    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE, BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)
    # R0=count, R1=constant one. First packet is emitted after one interval as COUNT=1.
    a.movi(0,0); a.movi(1,1)
    ir += ["MOV R0 = 0", "MOV R1 = 1", "LABEL LOOP"]
    a.label("loop")
    ir.append(f"WAIT {period}s"); a.wait_s(period)
    ir.append("ADD R0 = R0 + R1"); a.emit(OP_ADD,0,0,1)
    ir.append(f'FORMAT BUFFER "{prefix}" + R0 AS U32')
    a.emit(OP_FORMAT_REG,len(raw)); a.code += raw; a.emit(0,FMT_U32)
    ir.append("LORA SEND BUFFER"); a.emit(OP_LORA_SEND_BUF)
    ir += ["JMP LOOP", "END"]; a.jmp("loop"); a.emit(OP_END)
    return CompileResult(source,"lora-periodic-counter",schedule,ir,a.finish(),[
        "Counter is volatile and resets to 0 after reset/power loss unless persistence is explicitly requested.",
        f"First LoRa transmission occurs after {period}s and carries {prefix}1.",
    ])


def _vib_learning_seconds(t: str) -> int | None:
    # Canonical English plus legacy PT/ES compatibility. Accept both
    # "learning 2 hours" and natural English "2-hour learning period".
    patterns = [
        r"(?:aprendizado|aprendizagem|learning|aprendizaje)(?:\s+period)?(?:\s+de|\s+por|\s+for|\s+of)?\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)\b",
        r"(\d+)\s*[- ]?(seconds?|minutes?|hours?)\s+learning(?:\s+period)?\b",
    ]
    for pat in patterns:
        m=re.search(pat,t)
        if m: return _parse_seconds_fragment(m.group(1)+m.group(2))
    return None

def _vib_initial_seconds(t: str) -> int | None:
    # Canonical meaning: installation/arming delay before VIB_AUTO starts learning.
    m=re.search(r"(?:tempo inicial|atraso inicial|initial time|initial delay|tiempo inicial|retardo inicial)(?:\s+de|\s+of)?\s*(\d+)\s*(segundos?|seconds?|s|minutos?|minutes?|m|horas?|hours?|h)\b",t)
    return _parse_seconds_fragment(m.group(1)+m.group(2)) if m else None


def _compile_vib_config_alarm_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Compose VIB_AUTO configuration/start with a later ALARM -> LoRa action.

    This lowerer intentionally runs before the simpler vib-auto-config-start
    lowerer so a leading setup clause cannot consume and silently discard a
    following event/action clause.
    """
    wants_vib=bool(re.search(r"vib[_ ]?auto|vibracao|vibración|vibration|detec(?:cao|ção) de vibracao|deteccion de vibracion|vibration detection",t))
    wants_start=bool(re.search(r"inicie|iniciar|ative|habilite|start|enable|inicia|habilita",t))
    learning=_vib_learning_seconds(t); initial=_vib_initial_seconds(t)
    has_alarm=bool(re.search(r"anomalia|anomaly|alarme|alarm", t))
    has_lora=("lora" in t and bool(re.search(r"envie|enviar|send|transmita|message|mensagem|mensaje", t)))
    if not (wants_vib and wants_start and (learning is not None or initial is not None) and has_alarm and has_lora):
        return None
    if learning is None: learning=3600
    if initial is None: initial=60
    if not 60 <= learning <= 604800:
        raise TextCompileError("VIB_AUTO learning time must be 60..604800 seconds")
    if not 0 <= initial <= 604800:
        raise TextCompileError("VIB_AUTO initial/arming delay must be 0..604800 seconds")

    quoted = re.search(r'(?:mensagem|message|mensaje|alerta|alert)\s*[\:\=]?\s*["\']([^"\']+)["\']', source, re.I)
    payload = quoted.group(1) if quoted else "VIB_ALERT"
    raw=payload.encode("utf-8")
    if not 1 <= len(raw) <= 32:
        raise TextCompileError("LoRa alert payload must be 1..32 UTF-8 bytes")

    a=Assembler(); ir=[]
    ir.append(f"VIB_AUTO CONFIG learning={learning}s initial={initial}s")
    a.emit(OP_VIB_AUTO_CONFIG_TIME); a.code += struct.pack("<II",learning,initial)
    _emit_lora_config_if_requested(a,ir,t)
    ir.append("VIB_AUTO START"); a.emit(OP_VIB_AUTO_START,0)
    a.label("monitor")
    ir += ["LABEL MONITOR", "WAIT_EVENT VIB_AUTO.ALARM"]
    a.emit(OP_WAIT_EVENT,EVENT_VIB_ALARM,6,7)
    ir.append(f'LORA SEND "{payload}" x1 every 0s')
    a.emit(OP_LORA_SEND_BYTES,len(raw)); a.code += raw
    ir += ["RETURN TO VIB_AUTO MONITOR", "JMP MONITOR", "END"]
    a.jmp("monitor"); a.emit(OP_END)
    warnings=[
        "'initial time/initial delay' is interpreted as VIB_AUTO arming_delay_s before learning begins.",
        "VIB_AUTO configuration is persisted by the firmware configuration store.",
        "VIB_AUTO remains enabled; after the LoRa alert the VM returns to waiting for the next ALARM event.",
    ]
    if not quoted:
        warnings.append('No explicit LoRa payload was supplied; using documented canonical payload "VIB_ALERT".')
    return CompileResult(source,"vib-auto-config-alarm-lora",schedule,ir,a.finish(),warnings)

def _compile_vib_auto_config_start(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    wants_vib=bool(re.search(r"vib[_ ]?auto|vibracao|vibración|vibration|detec(?:cao|ção) de vibracao|deteccion de vibracion|vibration detection",t))
    wants_start=bool(re.search(r"inicie|iniciar|ative|habilite|start|enable|inicia|habilita",t))
    learning=_vib_learning_seconds(t); initial=_vib_initial_seconds(t)
    if not (wants_vib and wants_start and (learning is not None or initial is not None)):
        return None
    if learning is None: learning=3600
    if initial is None: initial=60
    if not 60 <= learning <= 604800:
        raise TextCompileError("VIB_AUTO learning time must be 60..604800 seconds")
    if not 0 <= initial <= 604800:
        raise TextCompileError("VIB_AUTO initial/arming delay must be 0..604800 seconds")
    a=Assembler(); ir=[]
    ir.append(f"VIB_AUTO CONFIG learning={learning}s initial={initial}s")
    a.emit(OP_VIB_AUTO_CONFIG_TIME); a.code += struct.pack("<II",learning,initial)
    ir.append("VIB_AUTO START"); a.emit(OP_VIB_AUTO_START,0)
    ir.append("END"); a.emit(OP_END)
    return CompileResult(source,"vib-auto-config-start",schedule,ir,a.finish(),[
        "'initial time/initial delay' is interpreted as VIB_AUTO arming_delay_s before learning begins.",
        "VIB_AUTO configuration is persisted by the firmware configuration store."
    ])


def _compile_vib_alarm_lora_alert(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Lower natural machine-anomaly -> LoRa alert using validated primitives.

    Example: "quando a máquina apresentar uma anomalia envie um alerta por lora".
    If no payload text is specified, the documented canonical payload VIB_ALERT
    is used. LoRa radio parameters use the normal platform defaults unless the
    sentence supplies explicit values.
    """
    if not (_vib_alarm_requested(t) and "lora" in t and re.search(r"envie|enviar|send|transmita|alerta|alert", t)):
        return None
    # A more specific dynamic battery flow owns requests that include battery.
    if re.search(r"bateria|battery", t):
        return None
    quoted = re.search(r'(?:mensagem|message|alerta|alert)\s*[\:\=]?\s*["\']([^"\']+)["\']', source, re.I)
    payload = quoted.group(1) if quoted else "VIB_ALERT"
    raw = payload.encode("utf-8")
    if not 1 <= len(raw) <= 32:
        raise TextCompileError("LoRa alert payload must be 1..32 UTF-8 bytes")
    count, every = _lora_repeat(t)
    if count > 1 and every <= 0:
        raise UnsupportedSemantics("repeated LoRa alerts need an interval, e.g. '3 transmissões com intervalo de 5 segundos'")
    a=Assembler(); ir=[]
    if _ndp_off_requested(t):
        ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)
    ir.append("VIB_AUTO START"); a.emit(OP_VIB_AUTO_START,0)
    a.label("monitor"); ir += ["LABEL MONITOR", "WAIT_EVENT VIB_AUTO.ALARM"]
    a.emit(OP_WAIT_EVENT,EVENT_VIB_ALARM,6,7)
    if count > 1:
        a.movi(4,count); a.movi(5,1); a.label("send_loop")
    ir.append(f'LORA SEND "{payload}" x{count} every {every}s')
    a.emit(OP_LORA_SEND_BYTES,len(raw)); a.code += raw
    if count > 1:
        a.emit(OP_SUB,4,4,5); a.jz(4,"resume")
        if every: a.wait_s(every)
        a.jmp("send_loop")
    a.label("resume")
    ir += ["RETURN TO VIB_AUTO MONITOR", "JMP MONITOR", "END"]
    a.jmp("monitor"); a.emit(OP_END)
    warnings=["VIB_AUTO remains enabled and the VM returns to waiting for the next ALARM event."]
    if not quoted:
        warnings.append('No explicit LoRa payload was supplied; using documented canonical payload "VIB_ALERT".')
    return CompileResult(source,"vib-auto-alarm-lora-alert",schedule,ir,a.finish(),warnings)


def _compile_vib_alarm_battery_lora(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    if not (re.search(r"vib[_ ]?auto",t) and re.search(r"anomalia|alarm|alarme",t) and re.search(r"bateria|battery",t) and "lora" in t):
        return None
    count,every=_lora_repeat(t)
    prefix=_quoted_prefix_before_variable(source,r"bateria(?: atual)?|battery(?: current)?") or "VIB ALERT BAT="
    raw=prefix.encode('utf-8')
    if len(raw)>48: raise TextCompileError("dynamic payload prefix must be <=48 UTF-8 bytes")
    a=Assembler(); ir=[]
    if _ndp_off_requested(t): ir.append("BLE NDP OFF"); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)
    _emit_lora_config_if_requested(a,ir,t)
    ir.append("VIB_AUTO START"); a.emit(OP_VIB_AUTO_START,0)
    a.label('monitor'); ir += ["LABEL MONITOR", "WAIT_EVENT VIB_AUTO.ALARM"]
    a.emit(OP_WAIT_EVENT,EVENT_VIB_ALARM,6,7)
    ir += ["BAT READ -> R0", f'FORMAT BUFFER "{prefix}" + R0 as VOLTS']
    a.emit(OP_BAT_READ,0); a.emit(OP_FORMAT_REG,len(raw)); a.code += raw; a.emit(0,FMT_CENTIVOLTS)
    if count>1:
        a.movi(4,count); a.movi(5,1); a.label('send_loop')
    ir.append(f"LORA SEND BUFFER x{count} every {every}s")
    a.emit(OP_LORA_SEND_BUF)
    if count>1:
        a.emit(OP_SUB,4,4,5); a.jz(4,'resume')
        if every: a.wait_s(every)
        a.jmp('send_loop')
    a.label('resume'); ir += ["RETURN TO VIB_AUTO MONITOR", "JMP MONITOR", "END"]
    a.jmp('monitor'); a.emit(OP_END)
    return CompileResult(source,'vib-auto-alarm-battery-lora',schedule,ir,a.finish(),[
        "VIB_AUTO remains enabled; after the alert burst the VM returns to waiting for the next ALARM event."
    ])



def _has_action(plan: ActionPlan, verb: str, target: str | None = None) -> bool:
    return any(a.verb == verb and (target is None or a.target == target) for a in plan.actions)

def _compile_action_plan(source: str, canonical: str, plan: ActionPlan, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Lower orthogonal semantic actions before intent-family dispatch.

    R3.8.16b1i starts the migration away from first-matching-family semantics.
    Only combinations whose data-flow is fully defined are emitted; everything
    else falls through to the existing deterministic lowerers.
    """
    acts=plan.actions
    if not acts:
        return None
    a=Assembler(); ir=[]; warnings=[]

    if _has_action(plan,'DISABLE','NDP'):
        ir.append('BLE NDP OFF'); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)

    # RECEIVE LoRa -> received BUFFER -> dynamic BLE advertiser.
    # OP_LORA_RX_START(1) yields one packet in the shared VM buffer.
    # OP_BLE_ADV_BUF publishes that buffer, then the VM rearms LoRa RX.
    rx_lora=any(x.verb=='RECEIVE' and x.source=='LORA' and x.destination=='BUFFER' for x in acts)
    display_beacon=any(x.verb=='DISPLAY' and x.source=='BUFFER' and x.destination=='BEACON' for x in acts)
    start_beacon=_has_action(plan,'START','BEACON')
    forward_serial=any(x.verb=='FORWARD' and x.source=='BUFFER' and x.destination=='SERIAL' for x in acts)

    if rx_lora and display_beacon:
        _emit_lora_config_if_requested(a,ir,canonical)
        # Advertiser owns BLE from this point; NDP was already explicitly OFF if requested.
        mb = re.search(r"(?:every|interval(?: of)?)\s*(\d+)\s*(seconds?|minutes?|hours?)\b", canonical, re.I)
        beacon_s = _parse_seconds_fragment(mb.group(1) + mb.group(2)) if mb else 2
        beacon_ms = beacon_s * 1000
        if not 20 <= beacon_ms <= 10240:
            raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")
        ir += ['BLE ADVERTISER', f'BLE ADV INTERVAL={beacon_ms}ms']
        a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER)
        a.emit(OP_BLE_ADV_CONFIG); a.u16(beacon_ms); a.emit(4 & 0xff,0)
        a.label('rx_loop')
        ir += ['LABEL RX_LOOP','LORA RX -> BUFFER','DEBUG BUFFER','BLE ADV BUFFER','JMP RX_LOOP','END']
        a.emit(OP_LORA_RX_START,1)
        a.emit(OP_DEBUG_BUFFER)
        a.emit(OP_BLE_ADV_BUF)
        a.jmp('rx_loop'); a.emit(OP_END)
        warnings += [
            'The beacon payload is the raw received LoRa packet in the VM buffer; it is not replaced by a fixed BLE name.',
            f'BLE advertising interval is {beacon_ms} ms.' if mb else 'BLE advertising interval defaults to 2000 ms because the prompt did not specify one.',
            'After each LoRa packet, BLE advertising data is refreshed and LoRa RX is rearmed.'
        ]
        return CompileResult(source,'actions:lora-rx-to-beacon',schedule,ir,a.finish(),warnings)

    if rx_lora and forward_serial:
        baud=_serial_baud(canonical)
        _emit_lora_config_if_requested(a,ir,canonical)
        ir.append(f'SERIAL CONFIG TX=D15 RX=D16 {baud} 8N1')
        a.emit(OP_SERIAL_CONFIG,15,16); a.u32(baud)
        a.label('rx_loop'); ir += ['LABEL RX_LOOP','LORA RX NEXT PACKET -> BUFFER','SERIAL WRITE BUFFER','JMP RX_LOOP','END']
        a.emit(OP_LORA_RX_START,1); a.emit(OP_SERIAL_WRITE_BUF); a.jmp('rx_loop'); a.emit(OP_END)
        return CompileResult(source,'lora-rx-serial-bridge',schedule,ir,a.finish(),[
            'LoRa receive direction is explicit; no LoRa SEND opcode is emitted.'
        ])

    # A standalone DISABLE action is complete by itself, but only when the
    # original/canonical sentence contains no second domain action that the
    # action parser failed to classify.  Otherwise fall through to the
    # deterministic intent lowerers (e.g. NDP OFF + LoRa boot test).
    has_other_domain = bool(re.search(
        r'\blora\b|tracking|beacon|temperature|battery|vib|hall|reed|gpio|serial|uart|motion',
        canonical, re.I))
    if len(acts)==1 and _has_action(plan,'DISABLE','NDP') and not has_other_domain:
        ir.append('END'); a.emit(OP_END)
        return CompileResult(source,'ndp-role-off',schedule,ir,a.finish(),warnings)

    return None

def _minimum_power_goal_requested(source: str, canonical: str) -> bool:
    """Recognize a board-baseline power goal without stealing mixed programs."""
    t=_fold(source + " " + canonical)
    # Global shutdown must be explicit. Keep this clause-local so a sentence
    # like "disable NDP ... then transmit LoRa" is not misclassified.
    global_off=bool(re.search(
        r'(?:deslig\w*|desativ\w*|pare|parar|disable|turn off|stop)'
        r'[^.;,]{0,45}\b(?:tudo|todos|todas|everything|all)\b', t))
    all_tx=bool(re.search(
        r'(?:deslig\w*|desativ\w*|pare|parar|disable|turn off|stop)'
        r'[^.;,]{0,35}\b(?:todas?|all)\b[^.;,]{0,20}\b(?:transmiss|transmission|radios?)', t))
    explicit_baseline=bool(re.search(
        r'(consumo\s+(?:minimo|minimo|mínimo)|menor\s+consumo|lowest\s+(?:power|current|consumption)|'
        r'minimum\s+(?:power|current|consumption)|baseline\s+(?:power|current)|low(?:est)?\s+power)', t))
    instrument=bool(re.search(r'\b(ppk\d*|multimetro|multímetro|multimeter|ammeter)\b', t))
    measurement=bool(re.search(r'\b(test\w*|teste|medir|medicao|medição|measure|measurement)\b', t))
    shutdown=bool(re.search(r'\b(deslig\w*|desativ\w*|disable|turn off|stop|pare|parar)\b', t))
    p021_only=bool(re.search(r'(?:somente|apenas|only)[^.;,]{0,20}p0[.]?21|(?:deixe|mantenha|keep)[^.;,]{0,35}p0[.]?21[^.;,]{0,20}(?:somente|apenas|only)', t))
    return global_off or all_tx or explicit_baseline or (instrument and measurement) or p021_only



def _serial_gateway_low_power_requested(t: str) -> bool:
    return bool(re.search(
        r'economiz\w*[^.;,]{0,30}(?:energia|energy)|(?:maximo|máximo|maximum)[^.;,]{0,20}(?:economia|energy|energia)|'
        r'baixo consumo|low power|lowest power|minimum power|menor consumo|consumo minimo|consumo mínimo|modo economico|modo econômico|economy mode|economic mode', t, re.I))

def _serial_rx_frame(t: str) -> tuple[int,int,str]:
    """Return (mode,param,pseudo). Defaults to transparent idle-gap framing."""
    # Fixed length has priority when explicitly scoped to serial/UART.
    m=re.search(r'(?:serial|uart)[^.;,]{0,45}?(?:pacote|packet|frame|bloco|block)?[^.;,]{0,15}?(\d+)\s*bytes?\b', t, re.I)
    if not m:
        m=re.search(r'(?:receba|receive|read|escute|listen)[^.;,]{0,35}?(\d+)\s*bytes?[^.;,]{0,25}(?:serial|uart)', t, re.I)
    if m:
        n=int(m.group(1))
        if not 1 <= n <= 64: raise TextCompileError('SERIAL fixed frame length must be 1..64 bytes')
        return 2,n,f'SERIAL RX LENGTH={n} -> BUFFER'
    if (re.search(r'\blinhas?\b', t, re.I) or re.search(r'complete line|serial line|até newline|ate newline|until newline', t, re.I)):
        return 1,0,'SERIAL RX LINE -> BUFFER'
    # Explicit idle/silence gap.
    m=re.search(r'(?:silencio|silêncio|idle|sem receber|without receiving|gap)[^.;,]{0,25}?(\d+)\s*(ms|milissegundos?|milliseconds?)', t, re.I)
    gap=int(m.group(1)) if m else 20
    if not 1 <= gap <= 60000: raise TextCompileError('SERIAL idle framing must be 1..60000 ms')
    return 0,gap,f'SERIAL RX IDLE={gap}ms -> BUFFER'

def _compile_serial_rx_router(source: str, canonical: str, force_boot: bool, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.18c generic SERIAL source -> LoRa/Beacon/Application route.

    A low-power qualifier is a semantic policy, not a request to disable the
    SERIAL source itself: unrelated resources are stopped first, then only the
    explicitly required SERIAL/radio path is enabled.
    """
    t=_fold(source + ' ' + canonical)
    tc=_fold(canonical)
    if not (re.search(r'\b(serial|serie|uart)\b', t) and re.search(r'receb|recib|receive|escut|listen|read|leia|leer', t)):
        return None
    # Direction is part of the semantic contract. Do not steal the already
    # validated LoRa-RX -> SERIAL bridge just because both domains occur.
    if re.search(r'(?:receive|listen\w*)[^.;,]{0,20}\blora\b|\blora\b\s+messages?\b', tc, re.I):
        return None
    # Require SERIAL to be the receiving/source side, not merely a destination.
    serial_source=bool(re.search(r'(?:receive|listen\w*|read|receb\w*|recib\w*|leer|lea|leia|escut\w*)[^.;,]{0,35}\b(?:serial|serie|uart)\b|\b(?:serial|serie|uart)\b[^.;,]{0,45}(?:receive|listen\w*|read|receb\w*|recib\w*|leer|lea|leia|escut\w*)', t, re.I))
    if not serial_source:
        serial_source=bool(re.search(r'\b(?:line|linha|linea|línea|buffer|frame|packet|pacote)\b',t,re.I) and re.search(r'\b(?:receb\w*|recib\w*|receive|read|leer|lea|leia)\b',t,re.I))
    if not serial_source: return None
    to_lora=bool(re.search(r'\blora\b', t) and re.search(r'envie|enviar|send|forward|retransm|encaminh|repasse|mande', t))
    to_beacon=bool(re.search(r'beacon|advertis|anunc', t) and re.search(r'envie|send|forward|mostre|display|publique|publish|retransm|encaminh', t))
    to_app=bool(re.search(r'\bndp\b|home assistant|application|aplicacao|aplicação|aplicacion|aplicación|\bapp\b', t) and re.search(r'envie|enviar|send|forward|notifi|encaminh|repasse|mande|envia', t))
    if not (to_lora or to_beacon or to_app): return None

    low_power=_serial_gateway_low_power_requested(t)
    schedule=SCHED_BOOT if (force_boot or _is_boot(t) or low_power) else SCHED_MANUAL
    baud=_serial_baud(t)
    mode,param,rx_ir=_serial_rx_frame(t)
    a=Assembler(); ir=[]; warnings=[]

    if low_power:
        ir.append('SYSTEM MINIMUM POWER'); a.emit(OP_SYSTEM_MIN_POWER)
        warnings.append('ECONOMY serial-gateway policy: unrelated resources are stopped; P0.21 remains available.')
        warnings.append('IMPORTANT: economy SERIAL uses GPIO wake with UARTE OFF while idle. The sender MUST transmit a sacrificial wake preamble byte (recommended 0x55), wait at least 5 ms at 57600 baud, then send the real payload. The wake byte is not forwarded.')
        ir.append(f'SERIAL CONFIG ECONOMY TX=D15 RX=D16 {baud} 8N1')
        a.emit(OP_SERIAL_CONFIG_ECO,15,16); a.u32(baud)
    else:
        ir.append(f'SERIAL CONFIG TX=D15 RX=D16 {baud} 8N1')
        a.emit(OP_SERIAL_CONFIG,15,16); a.u32(baud)

    # Preserve the persisted R3.8.18 RF profile unless the sentence explicitly changes it.
    explicit_rf=bool(re.search(r'\b(?:freq|frequency|frequencia|frequência|sf\s*\d|spreading|bw\s*\d|bandwidth|tx power|potencia|potência|cr4/|coding rate)\b', t))
    if to_lora and explicit_rf:
        _emit_lora_config_if_requested(a,ir,t)
    elif to_lora:
        warnings.append('LoRa uses the persisted RF profile; the LLCC68 wakes only for TX and returns to sleep after TX_DONE.')

    if to_beacon:
        ir += ['BLE ADVERTISER','BLE ADV INTERVAL=2000ms']
        a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER)
        a.emit(OP_BLE_ADV_CONFIG); a.u16(2000); a.emit(4 & 0xff,0)
        warnings.append('Beacon publishes the latest serial frame; VM buffer must be <=24 bytes for BLE advertising.')
    if to_app:
        # Application/NDP raw notifications need the Application peripheral plane.
        a.emit(OP_BLE_APP_ROLE,2); ir.append('BLE NDP ON')
        warnings.append('Application/NDP raw serial forwarding requires an active connected client and frames <=20 bytes.')

    a.label('serial_loop'); ir += ['LABEL SERIAL_LOOP',rx_ir]
    a.emit(OP_SERIAL_RX_BUF,mode); a.u16(param)
    prefix=_explicit_payload_prefix(source)
    if prefix:
        raw_prefix=prefix.encode('utf-8')
        if not 1 <= len(raw_prefix) <= 48: raise TextCompileError('serial received-buffer prefix must be 1..48 UTF-8 bytes')
        ir.append(f'BUFFER PREPEND "{prefix}"')
        a.emit(OP_BUFFER_PREPEND,len(raw_prefix)); a.code += raw_prefix
    if to_lora: ir.append('LORA SEND BUFFER'); a.emit(OP_LORA_SEND_BUF)
    if to_beacon: ir.append('BLE ADV BUFFER'); a.emit(OP_BLE_ADV_BUF)
    if to_app: ir.append('APP SEND BUFFER'); a.emit(OP_APP_SEND_BUF)
    ir += ['JMP SERIAL_LOOP','END']; a.jmp('serial_loop'); a.emit(OP_END)

    if mode==0: warnings.append(f'SERIAL framing defaults to an idle gap of {param} ms: bytes are accumulated until the line is quiet, then one VM buffer is forwarded.')
    elif mode==1: warnings.append('SERIAL framing is line based and completes on LF (\n).')
    else: warnings.append(f'SERIAL framing uses fixed {param}-byte frames.')
    return CompileResult(source,'serial-rx-low-power-router' if low_power else 'serial-rx-router',schedule,ir,a.finish(),warnings)

def _compile_minimum_power_goal(source: str, canonical: str, force_boot: bool=False) -> CompileResult | None:
    if not _minimum_power_goal_requested(source, canonical):
        return None
    t=_fold(source + " " + canonical)
    # P0.21 is an invariant and need not be stated. Reject other explicit KEEP
    # requests until resource masks are introduced rather than silently breaking them.
    keep=re.search(r'(?:mantenha|manter|deixe|keep|except|exceto|menos)\s+(?:somente\s+|apenas\s+|only\s+)?([^,;]+)', t)
    if keep:
        what=keep.group(1)
        if not (re.search(r'p0[.]?21|botao|botão|button|program', what) or re.search(r'p0[.]?21', t)):
            raise UnsupportedSemantics(f"minimum-power preserve exception is not supported yet: {what.strip()!r}")
    a=Assembler(); a.emit(OP_SYSTEM_MIN_POWER); a.emit(OP_END)
    # A measurement baseline must survive reset without a NUS connection, so
    # MINIMUM_POWER_TEST defaults to BOOT even when the user omits 'no boot'.
    ir=['SYSTEM MINIMUM POWER','END']
    return CompileResult(source,'minimum-power-test',SCHED_BOOT,ir,a.finish(),[
        'Minimum-power test defaults to BOOT so current can be measured after reset with NUS disconnected.',
        'P0.21 programming/wake remains enabled by firmware policy.'
    ])


def semantic_diagnose(source: str, force_boot: bool=False) -> dict[str, Any]:
    ast=parse_semantic_ast(source,force_boot)
    result="MISSING_CAPABILITY" if ast.missing_capabilities else "SEMANTIC_OK"
    return {"result":result,"ast":ast.to_dict()}



def _fall_accel_params(t: str) -> tuple[int,int,int,int,bool]:
    """Extract conservative FALL configuration, defaulting to the frozen IR defaults."""
    odr=10; fs=2; threshold=120; duration=100; low_power=True
    m=re.search(r'\b(1|10|25|50|100|200|400)\s*hz\b',t)
    if m: odr=int(m.group(1))
    m=re.search(r'(?:escala(?: de)?|scale(?: of)?|full[_ -]?scale(?: of)?)\s*(2|4|8|16)\s*g\b',t)
    if m: fs=int(m.group(1))
    m=re.search(r'(?:threshold|limiar|umbral)(?:\s+de)?\s*(\d+)\s*mg\b',t)
    if m: threshold=int(m.group(1))
    m=re.search(r'(?:duracao|duração|duration)(?:\s+de)?\s*(\d+)\s*ms\b',t)
    if m: duration=int(m.group(1))
    if re.search(r'baixo consumo|low power|bajo consumo',t): low_power=True
    return odr,fs,threshold,duration,low_power


def _fall_accel_line(t: str, mode: str='FALL') -> str:
    odr,fs,th,dur,lp=_fall_accel_params(t)
    return f'ACCEL CONFIG mode={mode} odr={odr} fs={fs} threshold={th} duration={dur} low_power={1 if lp else 0}'


def _compile_fall_event_cfg(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2f3 generic event/FALL CFG lowerer.

    Handles a FALL event source composed with one of these orthogonal bodies:
      * literal BLE + LoRa alert,
      * tracking stop + BLE alert,
      * event counter + threshold + BLE trigger,
      * GPIO-change gate -> FALL -> temp+battery -> LoRa -> accel off,
      * FALL -> temp+battery -> LoRa + BLE buffer.

    The emitted program is pseudo-IR compiled by the frozen deterministic pseudo compiler.
    """
    if not re.search(r'\bfall\b|queda',t):
        return None
    # Avoid stealing VIB_AUTO semantics; c2f3 is accelerator-event only.
    if re.search(r'vib[_ ]?auto',t):
        return None

    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}']
    accel=_fall_accel_line(t,'FALL')
    has_ble=bool(re.search(r'\bble\b|bluetooth|anunc|advertis|beacon',t))
    has_lora='lora' in t
    has_temp=bool(re.search(r'temperatura|temperature',t))
    has_bat=bool(re.search(r'bateria|battery|bater[ií]a',t))
    has_tracking=bool(re.search(r'tracking|rastreamento|seguimiento',t))
    has_counter=bool(re.search(r'contador|counter',t))
    has_gpio=bool(re.search(r'gpio|pino\s*\d+|pin\s*\d+',t))
    has_change=bool(re.search(r'mudanca|mudança|change|cambio',t))

    # GPIO-change gate followed by FALL and a sensor report.
    if has_gpio and has_change and has_temp and has_bat and has_lora:
        pin=_gpio_pin(t)
        if pin is None: return None
        lines += [
            f'GPIO P0.{pin} READ -> R0',
            'LABEL gpio_monitor',
            f'GPIO P0.{pin} READ -> R1',
            'CMP R2 = R1 EQ R0',
            'IF R2 == 0 GOTO gpio_changed',
            'JMP gpio_monitor',
            'LABEL gpio_changed',
            'MOV R0 = R1',
            accel,
            'WAIT_EVENT FALL',
            'TEMP READ -> R3',
            'BAT READ -> R4',
            'FORMAT BUFFER "T=" + R3 AS CELSIUS + " B=" + R4 AS VOLTS',
            'LORA SEND BUFFER',
            _fall_accel_line(t,'OFF'),
            'JMP gpio_monitor',
            'END',
        ]
        r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
        r.intent='event-fall-gpio-change-sensors-lora'
        return r

    # Event counter/threshold trigger. Generic for FALL + counter + equality threshold.
    if has_counter:
        m=re.search(r'(?:chegue|chegar|atingir|atinja|llegue|reach(?:es)?|reaches?|igual(?: a)?|==)\s*(?:a\s*)?(\d+)',t)
        threshold=int(m.group(1)) if m else 3
        literal='TRIGGER'
        qm=re.search(r"['\"]([^'\"]{1,32})['\"]",source)
        if qm: literal=qm.group(1)
        lines += [
            'MOV R0 = 0','MOV R1 = 1',f'MOV R2 = {threshold}',accel,
            'LABEL fall_loop','WAIT_EVENT FALL','ADD R0 = R0 + R1',
            'CMP R3 = R0 EQ R2','IF R3 == 0 GOTO fall_loop',
        ]
        if has_ble:
            lines += ['BLE ADVERTISER',f'BLE NAME "{literal}"','BLE ADV INTERVAL=2000ms','BLE ADV START']
        if has_lora:
            lines += [f'LORA SEND "{literal}"']
        lines += ['MOV R0 = 0','JMP fall_loop','END']
        r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
        r.intent='event-fall-counter-threshold'
        return r

    # Tracking starts before event; stop + BLE alert when FALL occurs, then re-arm FALL.
    if has_tracking and re.search(r'pare|parar|stop|detenga|detener',t):
        rot_s=1800
        m=re.search(r'(?:rotacao|rotação|rotation|rotacion)\s*(?:de)?\s*(\d+)\s*(minutos?|minutes?|mins?|min|m|segundos?|seconds?|s|horas?|hours?|h)',t)
        if m: rot_s=_parse_seconds_fragment(m.group(1)+m.group(2)) or rot_s
        literal='FALL'
        lines += [f'TRACKING ROTATION {rot_s}s','TRACKING START',accel,
                  'LABEL fall_loop','WAIT_EVENT FALL','TRACKING STOP']
        if has_ble:
            lines += ['BLE ADVERTISER',f'BLE NAME "{literal}"','BLE ADV INTERVAL=2000ms','BLE ADV START']
        lines += ['JMP fall_loop','END']
        r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
        r.intent='event-fall-tracking-stop'
        return r

    # Multi-sensor event body: FALL -> temperature+battery -> LoRa and optional BLE buffer.
    if has_temp and has_bat and (has_lora or has_ble):
        interval_ms=2000
        m=re.search(r'(?:intervalo(?: de)?|interval(?: of)?|cada|every)\s*(\d+)\s*(segundos?|seconds?|s|ms|milissegundos?|milliseconds?)',t)
        if m:
            n=int(m.group(1)); unit=m.group(2)
            interval_ms=n if unit.startswith('ms') or 'mili' in unit else n*1000
        lines += [accel,'LABEL fall_loop','WAIT_EVENT FALL','TEMP READ -> R0','BAT READ -> R1',
                  'FORMAT BUFFER "T=" + R0 AS CELSIUS + " B=" + R1 AS VOLTS']
        if has_lora: lines += ['LORA SEND BUFFER']
        if has_ble:
            lines += ['BLE ADVERTISER',f'BLE ADV INTERVAL={interval_ms}ms','BLE ADV START','BLE ADV BUFFER']
        lines += ['JMP fall_loop','END']
        r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
        r.intent='event-fall-multisensor-alert'
        return r

    # Literal alert on event; use a mutable buffer for BLE and literal LoRa send.
    if has_ble and has_lora:
        literal='FALL'
        qm=re.search(r"['\"]([^'\"]{1,32})['\"]",source)
        if qm: literal=qm.group(1)
        lines += [accel,'BLE ADVERTISER','BLE ADV INTERVAL=2000ms','BLE ADV START',
                  'LABEL fall_loop','WAIT_EVENT FALL',f'BUFFER SET "{literal}"','BLE ADV BUFFER',
                  f'LORA SEND "{literal}"','JMP fall_loop','END']
        r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
        r.intent='event-fall-literal-alert'
        return r
    return None


def _compile_vib_auto_event_cfg(source: str, t: str, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """R3.8.20c2f4 generic VIB_AUTO event CFG lowerer.

    A named *machine monitoring/watching* profile is treated as an explicit
    profile selection, mapped deterministically to learning=3600s and
    initial=60s.  Outside that named profile, VIB_AUTO CONFIG still requires
    explicit learning/initial values; there is no global silent default.

    Generic body:
      CONFIG -> START -> WAIT ALARM -> optional LoRa/BLE actions -> loop.
    """
    if not re.search(r'vib[_ ]?auto', t):
        return None
    has_alarm=bool(re.search(r'machine alarm|alarm event|alarme(?: da maquina| de maquina)?|evento de alarme|alarma(?: de maquina)?|evento de alarma|anomalia|anomaly',t))
    wants_start=bool(re.search(r'\bstart\b|inicie|iniciar|ative|habilite|inicia|habilita',t))
    if not (has_alarm and wants_start):
        return None
    learning=_vib_learning_seconds(t)
    initial=_vib_initial_seconds(t)
    profile=bool(re.search(r'machine (?:monitoring|watching)|monitoramento (?:da )?maquina|monitoramento de maquina|monitoreo (?:de )?maquina',t))
    if learning is None or initial is None:
        if profile:
            learning = 3600 if learning is None else learning
            initial = 60 if initial is None else initial
        else:
            return None
    if not 60 <= learning <= 604800:
        raise TextCompileError("VIB_AUTO learning time must be 60..604800 seconds")
    if not 0 <= initial <= 604800:
        raise TextCompileError("VIB_AUTO initial/arming delay must be 0..604800 seconds")

    has_lora='lora' in t and bool(re.search(r'send|transmit|envie|enviar|transmita|manda|mandar',t))
    has_ble=bool(re.search(r'\bble\b|bluetooth|advertis|anunc',t))
    if not (has_lora or has_ble):
        return None
    qm=re.search(r"['\"]([^'\"]{1,32})['\"]",source)
    literal=qm.group(1) if qm else 'VIB'
    from nrfclaw_pseudo import compile_pseudo
    lines=[f'PROGRAM {"BOOT" if schedule==SCHED_BOOT else "MANUAL"}',
           f'VIB_AUTO CONFIG learning={learning}s initial={initial}s',
           'VIB_AUTO START']
    if has_ble:
        lines += ['BLE ADVERTISER',f'BLE NAME "{literal}"','BLE ADV INTERVAL=2000ms']
    lines += ['LABEL vib_alarm_loop','WAIT_EVENT VIB_AUTO.ALARM']
    if has_lora:
        lines += [f'LORA SEND "{literal}"']
    if has_ble:
        lines += ['BLE ADV START']
    lines += ['JMP vib_alarm_loop','END']
    r=compile_pseudo('\n'.join(lines),force_boot=(schedule==SCHED_BOOT))
    r.intent='vib-auto-event-cfg'
    r.warnings.append('Named machine-monitoring profile maps to learning=3600s and initial=60s when those values are omitted.')
    return r

def _certify_standalone_result(source: str, result: CompileResult, meta: dict[str, Any]) -> tuple[CompileResult, dict[str, Any]]:
    """R3.8.20c2g3a-r1: certify actions plus authenticated schedule metadata.

    AT/WEEKLY are external schedules. Existing standalone EVERY lowerers retain
    their validated WAIT/JMP CFG for regression compatibility; Semantic IR and
    agent paths may emit Schedule ABI EVERY directly.
    """
    sched=detect_schedule_intent(source)
    if sched and sched.get('mode') in {'AT','WEEKLY','BOOT'}:
        lowered=normalize_schedule(sched)
        result.schedule_mode=lowered['mode']; result.schedule=lowered
        meta['schedule_semantic']=sched; meta['schedule_abi']=lowered
    elif result.schedule is None:
        result.schedule=normalize_schedule({'mode':'BOOT' if result.schedule_mode==SCHED_BOOT else 'MANUAL'})
    cert=certify_standalone(source,result.ir,result.schedule_mode,getattr(result,'schedule',None))
    meta["standalone_certification"]=cert.to_dict()
    if not cert.passed:
        raise UnsupportedSemantics(
            "STANDALONE_SEMANTIC_CERTIFICATION_FAILED: " + "; ".join(cert.failures)
        )
    meta["semantic_certified"]=True
    return result,meta

def compile_hybrid(source: str, force_boot: bool = False) -> tuple[CompileResult, dict[str, Any]]:
    if not source or not source.strip(): raise TextCompileError("empty prompt")
    lang=canonicalize_to_english(source)
    canonical_source=lang.canonical_english
    cfg=load_config()
    ranked=semantic_matches(canonical_source,3)
    meta={"engine":"hybrid","version":SEMANTIC_ENGINE_VERSION,
          "language":lang.language,"canonical_english":canonical_source,
          "matches":[{"score":round(s,3),"family":e.get("family"),"text":e.get("text")} for s,e in ranked]}
    ast=parse_semantic_ast(source, force_boot=force_boot)
    meta["ast"]=ast.to_dict()
    serial_result=_compile_serial_rx_router(source, canonical_source, force_boot, cfg)
    if serial_result is not None:
        meta["route"]="semantic-goal"; meta["language_route"]="goal-resolver"; meta["family"]=serial_result.intent
        return _certify_standalone_result(source,serial_result,meta)
    power_result=_compile_minimum_power_goal(source, canonical_source, force_boot=force_boot)
    if power_result is not None:
        meta["route"]="semantic-goal"; meta["language_route"]="goal-resolver"; meta["family"]=power_result.intent
        meta["semantic_goal"]={"goal":"MINIMUM_POWER_TEST","schedule":"BOOT","preserve":["P0.21"]}
        return _certify_standalone_result(source,power_result,meta)
    action_plan=parse_action_plan(canonical_source)
    meta["action_plan"]=action_plan.to_dict()
    action_schedule=SCHED_BOOT if (force_boot or _is_boot(_fold(canonical_source))) else SCHED_MANUAL
    action_result=_compile_action_plan(source,canonical_source,action_plan,action_schedule,cfg)
    if action_result is not None:
        meta["route"]="action-graph"; meta["language_route"]="canonical-en"; meta["family"]=action_result.intent
        return _certify_standalone_result(source,action_result,meta)
    lowerers=(_compile_landing_motion_tracking,_compile_landing_motion_beacon,_compile_landing_vibration_ha,_compile_temperature_ble_beacon,_compile_vib_auto_lora_beacon,_compile_vib_auto_event_cfg,_compile_fall_event_cfg,_compile_gpio_change_counter_threshold,_compile_hall_counter_ble_lora,_compile_serial_parse_compare_gpio,_compile_gpio_compare_app_else_wait,_compile_battery_to_beacon,_compile_lora_rx_to_beacon,_compile_beacon_temp_lora_tracking,_compile_lora_test_boot,_compile_lora_counter_stream,_compile_minpower_hall_battery_lora,_compile_temp_battery_periodic_lora,_compile_gpio_high_temp_battery_lora,_compile_lora_temperature_stream,
              _compile_vib_config_alarm_lora,_compile_vib_auto_config_start,_compile_vib_alarm_battery_lora,
              _compile_vib_alarm_lora_alert,_compile_lora_battery_stream,
              _compile_lora_rx_serial_bridge,_compile_ndp_motion_ha,
              _compile_temperature_ble_tracking,_compile_tracking_battery_lora,
              _compile_temp_gpio,_compile_hall_wait,_compile_ndp_role_only,_compile_basic_battery_lora, _compile_direct_lora,_compile_direct_gpio)

    # Compatibility-first for already validated PT/ES grammar, then canonical
    # English retry. This prevents a generic English lowerer from stealing a
    # richer legacy behavior while the lowerers are being migrated to English.
    attempts=[("native-compat", source)]
    if _fold(canonical_source) != _fold(source):
        attempts.append(("canonical-en", canonical_source))
    last_error=None
    for route_name,candidate in attempts:
        t=_fold(candidate)
        schedule=SCHED_BOOT if (force_boot or _is_boot(t)) else SCHED_MANUAL
        candidate_error=None
        for fn in lowerers:
            try:
                r=fn(candidate,t,schedule,cfg)
            except TextCompileError as exc:
                candidate_error=exc
                break
            if r is not None:
                r.source=source
                meta["route"]="compositional" if route_name=="native-compat" else "compositional-english-retry"
                meta["language_route"]=route_name
                meta["family"]=r.intent
                return _certify_standalone_result(source,r,meta)
        if candidate_error is not None:
            last_error=candidate_error
            continue
        try:
            r=legacy_compile_text(candidate,force_boot=force_boot)
            r.source=source
            meta["route"]="legacy-b8" if route_name=="native-compat" else "legacy-b8-english-retry"
            meta["language_route"]=route_name
            meta["family"]=r.intent
            return _certify_standalone_result(source,r,meta)
        except TextCompileError as exc:
            last_error=exc

    if ranked and ranked[0][0] >= 0.74 and ranked[0][1].get("canonical"):
        canonical=str(ranked[0][1]["canonical"])
        try:
            r=legacy_compile_text(canonical,force_boot=force_boot)
            r.source=source
            r.warnings.append(f"semantic matcher used English trained example score={ranked[0][0]:.2f}")
            meta["route"]="semantic-example"; meta["language_route"]="canonical-en"
            meta["family"]=r.intent; meta["canonical"]=canonical
            return _certify_standalone_result(source,r,meta)
        except TextCompileError:
            pass
    missing = ", ".join(ast.missing_capabilities) if ast.missing_capabilities else "generic VM lowerer for parsed AST"
    raise UnsupportedSemantics(
        "SEMANTIC AST parsed, but deterministic lowering is not available yet. "
        f"Missing/lowerer: {missing}. Parsed capabilities: {', '.join(ast.capabilities) or 'none'}. "
        f"Canonical English retry: {canonical_source!r}. Last compiler error: {last_error}"
    )

# ---- R3.8.16b1 hierarchical semantic diagnostics / coverage ----
def _semantic_clause_split(source: str) -> list[str]:
    s=re.sub(r'\s+',' ',source.strip())
    base=[]
    for part in re.split(r'(?<=[.;])\s+|\s*;\s*',s):
        part=part.strip(' .;')
        if not part: continue
        # Preserve scopes by splitting before strong control markers, not ordinary conjunctions.
        pieces=re.split(r'\s+(?=(?:quando|when|cuando|se\s|if\s|si\s|sen[aã]o|else\b|caso contr[aá]rio|otherwise|abaixo de|below|acima de|above|entre\s|between\s|ao desligar|ao parar|ao ligar|depois de reset)\b)',part,flags=re.I)
        for x in pieces:
            x=x.strip(' ,')
            if not x: continue
            if _fold(x) in {'e','and','y','then','entao','então'}: continue
            base.append(x)
    return base


def _clause_features(clause: str) -> tuple[list[str], list[str], list[str]]:
    """Return (recognized, missing_caps, unresolved_reasons)."""
    t=_fold(clause); rec=[]; miss=[]; unr=[]
    pats=[
        ('BLE_NDP',r'\bndp\b|bluetooth|home assistant|\bha\b'),
        ('BEACON',r'beacon|anunc|advertis'),
        ('VIB_AUTO',r'vib[_ ]?auto|vibracao|vibração'),
        ('MOTION',r'motion|movimento|movimentacao|movimentação'),
        ('TEMPERATURE',r'temperatura|temperature|temperatura'),
        ('BATTERY',r'bateria|battery|bater[ií]a'),
        ('TRACKING',r'tracking|rastreamento|seguimiento'),
        ('LORA',r'\blora\b'),
        ('SERIAL',r'serial|uart'),
        ('HALL',r'\bhall\b|reed'),
        ('GPIO',r'gpio|pino\s*\d+|pin\s*\d+'),
        ('STATE',r'persistente|persistent|estado|state|contador|counter'),
        ('TIMING',r'a cada|every|cada\s+\d+|intervalo|durante|for\s+\d+'),
        ('CONDITION',r'\bse\b|\bif\b|\bsi\b|abaixo de|acima de|below|above|entre|between'),
        ('EVENT',r'\bquando\b|\bwhen\b|\bcuando\b|aguarde|wait'),
    ]
    for name,pat in pats:
        if re.search(pat,t): rec.append(name)
    # Explicit runtime gaps.
    if re.search(r'hora de inicio|hora de início|quanto tempo|duracao|duração|elapsed|uptime',t): miss.append('MONOTONIC_TIME')
    if re.search(r'entr(?:e|ar) em sleep|system off|deep sleep|dormir',t): miss.append('POWER_SLEEP')
    if re.search(r'quando receber (?:a mensagem|o pacote)|se (?:a mensagem|o pacote)|mensagem for|come[cç]ar com',t): miss.append('BUFFER_COMPARE')
    if re.search(r'direcao|direção|frente|reverso',t) and re.search(r'hall\s*1',t) and re.search(r'hall\s*2',t): miss.append('HALL_QUADRATURE_DIRECTION_VALIDATION')
    # Known generic-lowerer gaps in b1 standalone parser. These are not hardware gaps.
    if re.search(r'caso contr[aá]rio|sen[aã]o|otherwise|\belse\b',t): unr.append('IF/ELSE scope requires generic AST lowerer')
    if re.search(r'incremente|incrementa|decremente|decrementa|increment|decrement',t): unr.append('arithmetic/state update relation not fully lowered')
    if re.search(r'a cada\s+\d+\s+(?:acionamentos|pulsos|eventos|changes)',t): unr.append('event-count trigger/modulo relation not fully lowered')
    if re.search(r'monte uma (?:unica )?mensagem|mensagem composta|contendo .*[,=].*[,=]',t): unr.append('multi-variable payload composition not fully lowered')
    if re.search(r'anuncie|advertis',t) and re.search(r'mais (?:a |o )?(?:variavel|temperatura|bateria|valor)|\+\s*(?:variavel|temperature|battery)',t): unr.append('dynamic advertising data-flow needs structured lowering')
    if re.search(r'warning',t) and re.search(r'alarm|alarme',t): unr.append('multiple event handlers must preserve distinct bodies')
    if re.search(r'abaixo de .*abaixo de|acima de .*abaixo de|entre .*acima de',t): unr.append('multiple threshold branches/hysteresis require structured lowering')
    if re.search(r'depois de reset|ap[oó]s reset|after reset',t): unr.append('persistent restart branch requires structured lowering')
    if re.search(r'linha completa|complete line|aguarde uma linha',t): unr.append('serial receive framing/line semantics require structured lowering')
    if not rec: unr.append('no recognized semantic operation')
    return rec,sorted(set(miss)),unr



def _has_action(plan: ActionPlan, verb: str, target: str | None = None) -> bool:
    return any(a.verb == verb and (target is None or a.target == target) for a in plan.actions)

def _compile_action_plan(source: str, canonical: str, plan: ActionPlan, schedule: int, cfg: dict[str, Any]) -> CompileResult | None:
    """Lower orthogonal semantic actions before intent-family dispatch.

    R3.8.16b1i starts the migration away from first-matching-family semantics.
    Only combinations whose data-flow is fully defined are emitted; everything
    else falls through to the existing deterministic lowerers.
    """
    acts=plan.actions
    if not acts:
        return None
    a=Assembler(); ir=[]; warnings=[]

    if _has_action(plan,'DISABLE','NDP'):
        ir.append('BLE NDP OFF'); a.emit(OP_BLE_APP_ROLE,BLE_ROLE_OFF)

    # RECEIVE LoRa -> received BUFFER -> dynamic BLE advertiser.
    # OP_LORA_RX_START(1) yields one packet in the shared VM buffer.
    # OP_BLE_ADV_BUF publishes that buffer, then the VM rearms LoRa RX.
    rx_lora=any(x.verb=='RECEIVE' and x.source=='LORA' and x.destination=='BUFFER' for x in acts)
    display_beacon=any(x.verb=='DISPLAY' and x.source=='BUFFER' and x.destination=='BEACON' for x in acts)
    start_beacon=_has_action(plan,'START','BEACON')
    forward_serial=any(x.verb=='FORWARD' and x.source=='BUFFER' and x.destination=='SERIAL' for x in acts)

    if rx_lora and display_beacon:
        _emit_lora_config_if_requested(a,ir,canonical)
        # Advertiser owns BLE from this point; NDP was already explicitly OFF if requested.
        mb = re.search(r"(?:every|interval(?: of)?)\s*(\d+)\s*(seconds?|minutes?|hours?)\b", canonical, re.I)
        beacon_s = _parse_seconds_fragment(mb.group(1) + mb.group(2)) if mb else 2
        beacon_ms = beacon_s * 1000
        if not 20 <= beacon_ms <= 10240:
            raise TextCompileError("Beacon interval must be 20..10240 ms on the current backend")
        ir += ['BLE ADVERTISER', f'BLE ADV INTERVAL={beacon_ms}ms']
        a.emit(OP_BLE_APP_ROLE,BLE_ROLE_ADVERTISER)
        a.emit(OP_BLE_ADV_CONFIG); a.u16(beacon_ms); a.emit(4 & 0xff,0)
        a.label('rx_loop')
        ir += ['LABEL RX_LOOP','LORA RX -> BUFFER','DEBUG BUFFER','BLE ADV BUFFER','JMP RX_LOOP','END']
        a.emit(OP_LORA_RX_START,1)
        a.emit(OP_DEBUG_BUFFER)
        a.emit(OP_BLE_ADV_BUF)
        a.jmp('rx_loop'); a.emit(OP_END)
        warnings += [
            'The beacon payload is the raw received LoRa packet in the VM buffer; it is not replaced by a fixed BLE name.',
            f'BLE advertising interval is {beacon_ms} ms.' if mb else 'BLE advertising interval defaults to 2000 ms because the prompt did not specify one.',
            'After each LoRa packet, BLE advertising data is refreshed and LoRa RX is rearmed.'
        ]
        return CompileResult(source,'actions:lora-rx-to-beacon',schedule,ir,a.finish(),warnings)

    if rx_lora and forward_serial:
        baud=_serial_baud(canonical)
        _emit_lora_config_if_requested(a,ir,canonical)
        ir.append(f'SERIAL CONFIG TX=D15 RX=D16 {baud} 8N1')
        a.emit(OP_SERIAL_CONFIG,15,16); a.u32(baud)
        a.label('rx_loop'); ir += ['LABEL RX_LOOP','LORA RX NEXT PACKET -> BUFFER','SERIAL WRITE BUFFER','JMP RX_LOOP','END']
        a.emit(OP_LORA_RX_START,1); a.emit(OP_SERIAL_WRITE_BUF); a.jmp('rx_loop'); a.emit(OP_END)
        return CompileResult(source,'lora-rx-serial-bridge',schedule,ir,a.finish(),[
            'LoRa receive direction is explicit; no LoRa SEND opcode is emitted.'
        ])

    # A standalone DISABLE action is complete by itself, but only when the
    # original/canonical sentence contains no second domain action that the
    # action parser failed to classify.  Otherwise fall through to the
    # deterministic intent lowerers (e.g. NDP OFF + LoRa boot test).
    has_other_domain = bool(re.search(
        r'\blora\b|tracking|beacon|temperature|battery|vib|hall|reed|gpio|serial|uart|motion',
        canonical, re.I))
    if len(acts)==1 and _has_action(plan,'DISABLE','NDP') and not has_other_domain:
        ir.append('END'); a.emit(OP_END)
        return CompileResult(source,'ndp-role-off',schedule,ir,a.finish(),warnings)

    return None

def semantic_diagnose(source: str, force_boot: bool=False) -> dict[str, Any]:
    ast=parse_semantic_ast(source,force_boot)
    clauses=[]; resolved=0; all_missing=set(ast.missing_capabilities); unresolved=[]
    for i,c in enumerate(_semantic_clause_split(source),1):
        rec,miss,unr=_clause_features(c); all_missing.update(miss)
        status='RESOLVED' if rec and not miss and not unr else ('MISSING_CAPABILITY' if miss else 'INCOMPLETE')
        if status=='RESOLVED': resolved+=1
        if unr: unresolved.append(c)
        clauses.append({'id':i,'text':c,'recognized':rec,'status':status,'missing_capabilities':miss,'issues':unr})
    total=len(clauses); coverage=(resolved/total if total else 0.0)
    if all_missing: result='MISSING_CAPABILITY'
    elif coverage < 1.0: result='SEMANTIC_INCOMPLETE'
    else: result='SEMANTIC_OK'
    d=ast.to_dict(); d['clauses']=clauses; d['coverage']=round(coverage,3); d['unresolved_clauses']=unresolved; d['missing_capabilities']=sorted(all_missing)
    return {'result':result,'coverage':round(coverage,3),'ast':d}
