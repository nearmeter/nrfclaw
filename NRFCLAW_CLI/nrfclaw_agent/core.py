#!/usr/bin/env python3
"""nRFClaw R3.8.16b1a external/local AI semantic agent.

The LLM is deliberately NOT a bytecode generator. It may only produce a
versioned semantic plan. The existing deterministic compiler remains the sole
component allowed to generate VM bytecode.
"""
from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.request
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

from nrfclaw_text_compiler import CompileResult, TextCompileError
from nrfclaw_hybrid_semantic import compile_hybrid

SCHEMA_VERSION = 1
KB_VERSION = 1
VM_ABI = 1
DEFAULT_CONFIG = Path.home() / ".config" / "nrfclaw" / "agent.json"
KNOWLEDGE_DIR = Path(__file__).resolve().parent / "knowledge"


class AgentError(RuntimeError):
    pass

_LAST_PROVIDER_META: Dict[str, Any] = {}

def _set_provider_meta(obj: Dict[str, Any], stage: str = "") -> None:
    global _LAST_PROVIDER_META
    usage=obj.get("usage") if isinstance(obj,dict) else None
    _LAST_PROVIDER_META={"transport_stage":stage}
    if isinstance(usage,dict):
        for k in ("prompt_tokens","completion_tokens","total_tokens","cost"):
            if k in usage: _LAST_PROVIDER_META[k]=usage[k]

def last_provider_meta() -> Dict[str, Any]:
    return dict(_LAST_PROVIDER_META)


@dataclass
class AgentConfig:
    enabled: bool = False
    provider: str = "ollama"
    model: str = "qwen2.5:3b"
    base_url: str = "http://127.0.0.1:11434"
    api_key_env: str = ""
    timeout_s: int = 45


def provider_catalog() -> Dict[str, Dict[str, str]]:
    return {
        "ollama": {
            "default_model": "qwen2.5:3b",
            "default_base_url": "http://127.0.0.1:11434",
            "api_key_env": "",
        },
        "openrouter": {
            "default_model": "openrouter/free",
            "default_base_url": "https://openrouter.ai/api/v1",
            "api_key_env": "OPENROUTER_API_KEY",
        },
        "gemini": {
            "default_model": "gemini-2.0-flash-lite",
            "default_base_url": "https://generativelanguage.googleapis.com/v1beta",
            "api_key_env": "GEMINI_API_KEY",
        },
    }


def load_config(path: Path = DEFAULT_CONFIG) -> AgentConfig:
    if not path.exists():
        return AgentConfig()
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        return AgentConfig(**{k: raw[k] for k in asdict(AgentConfig()).keys() if k in raw})
    except Exception as exc:
        raise AgentError(f"cannot read agent config {path}: {exc}") from exc


def save_config(cfg: AgentConfig, path: Path = DEFAULT_CONFIG) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(asdict(cfg), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def disable_agent(path: Path = DEFAULT_CONFIG) -> AgentConfig:
    cfg = load_config(path)
    cfg.enabled = False
    save_config(cfg, path)
    return cfg


def configure_agent(provider: str, model: Optional[str] = None,
                    base_url: Optional[str] = None,
                    api_key_env: Optional[str] = None,
                    timeout_s: Optional[int] = None,
                    path: Path = DEFAULT_CONFIG) -> AgentConfig:
    catalog = provider_catalog()
    if provider not in catalog:
        raise AgentError(f"unknown provider '{provider}'")
    p = catalog[provider]
    cfg = AgentConfig(
        enabled=True,
        provider=provider,
        model=model or p["default_model"],
        base_url=(base_url or p["default_base_url"]).rstrip("/"),
        api_key_env=api_key_env if api_key_env is not None else p["api_key_env"],
        timeout_s=timeout_s or 45,
    )
    save_config(cfg, path)
    return cfg


def _load_knowledge() -> str:
    chunks = []
    for path in sorted(KNOWLEDGE_DIR.glob("*.md")):
        chunks.append(f"\n===== {path.name} =====\n{path.read_text(encoding='utf-8').strip()}\n")
    if not chunks:
        raise AgentError(f"knowledge base is empty: {KNOWLEDGE_DIR}")
    return "".join(chunks)


def build_system_prompt() -> str:
    return f"""You are the nRFClaw semantic planner.
KB_VERSION={KB_VERSION}; VM_ABI={VM_ABI}; SEMANTIC_SCHEMA={SCHEMA_VERSION}.
Interpret the user's request using ONLY capabilities documented below.
Never invent opcodes, register values, capabilities, sensors, or timing semantics.
Never output VM bytecode. Return exactly one JSON object and no markdown.
If the request cannot be represented by the schema/capabilities, return
{{\"schema_version\":1,\"supported\":false,\"error\":\"clear reason\"}}.
If information is omitted, use documented defaults only.

{_load_knowledge()}
"""


def _request_json(url: str, payload: Dict[str, Any], headers: Dict[str, str], timeout_s: int) -> Dict[str, Any]:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json", **headers}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:1000]
        raise AgentError(f"agent HTTP {exc.code}: {detail}") from exc
    except Exception as exc:
        raise AgentError(f"agent request failed: {exc}") from exc


def _call_ollama(cfg: AgentConfig, system: str, user: str) -> str:
    obj = _request_json(
        cfg.base_url.rstrip("/") + "/api/chat",
        {"model": cfg.model, "stream": False, "format": "json",
         "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}]},
        {}, cfg.timeout_s)
    try:
        return obj["message"]["content"]
    except Exception as exc:
        raise AgentError("invalid Ollama response") from exc


def _normalize_chat_content(content: Any) -> str:
    """Normalize OpenAI/OpenRouter-compatible message content to text.

    Most providers return a string, but some routed models return an array of
    typed text parts. Supporting both prevents a valid response from being
    rejected before semantic validation.
    """
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts=[]
        for item in content:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict):
                txt=item.get("text") or item.get("content")
                if isinstance(txt, str): parts.append(txt)
        if parts: return "\n".join(parts)
    raise AgentError("agent message content is empty or non-text")


def _semantic_ir_response_schema(allowed_ops: Optional[list[str]] = None) -> Dict[str, Any]:
    """Provider-side structural guardrail; local validate_ir remains authoritative.

    The action object deliberately allows the union of known Semantic IR field
    names. Operation-specific required/range rules stay in validate_ir(). This
    keeps the provider schema portable while preventing wrapper/meta fields.
    """
    ops = [
        "system.minimum_power","label","jump","jump_if_zero","register.copy",
        "register.set","register.add","register.sub","register.compare","wait",
        "gpio.read","gpio.write","accel.config","wait.event","battery.read",
        "temperature.read","hall.wait","ble.ndp","ble.advertiser","ble.name",
        "ble.adv_start","ble.advertise_buffer","lora.config","lora.receive",
        "lora.send_literal","lora.send_buffer","serial.config","serial.receive",
        "serial.write_buffer","app.send_buffer","debug.buffer","buffer.prepend",
        "buffer.parse_int","buffer.parse_fixed","buffer.format_register",
        "buffer.format_fixed","buffer.format_2reg","vib_auto.config","vib_auto.start",
        "vib_auto.stop","tracking.config","tracking.rotation","tracking.start",
        "tracking.stop","ha.event","loop"
    ]
    if allowed_ops:
        selected=set(allowed_ops)
        ops=[op for op in ops if op in selected]
        if not ops:
            ops=["label","jump"]
    integer = {"type":"integer"}
    number = {"type":"number"}
    string = {"type":"string"}
    boolean = {"type":"boolean"}
    action_props = {
        "op":{"type":"string","enum":ops}, "name":string, "label":string,
        "register":integer, "dst":integer, "src":integer, "a":integer, "b":integer,
        "value":{"type":["string","integer"]}, "condition":{"type":"string"}, "seconds":integer, "pin":integer,
        "mode":{"type":"string"}, "odr_hz":integer, "full_scale_g":integer,
        "threshold_mg":integer, "duration_ms":integer, "low_power":boolean,
        "event":{"type":"string"}, "enabled":boolean, "interval_ms":integer,
        "frequency_mhz":number, "power_dbm":integer, "sf":integer, "bw_khz":integer,
        "cr":integer, "target":{"type":"string"}, "tx_pin":integer, "rx_pin":integer,
        "baud":integer, "economy":boolean, "scale":integer, "prefix":string,
        "format":string, "decimals":integer, "suffix":string, "prefix1":string,
        "reg1":integer, "format1":string, "prefix2":string, "reg2":integer,
        "format2":string, "learning_s":integer, "initial_s":integer, "tx_dbm":integer,
        "cap":integer, "operation":integer,
        "actions":{"type":"array","items":{"$ref":"#/$defs/action"}}
    }
    action={"type":"object","properties":action_props,"required":["op"],"additionalProperties":False}
    return {
        "$defs":{"action":action},
        "type":"object",
        "properties":{
            "ir_version":{"type":"integer","enum":[1]},
            "status":{"type":"string","enum":["COMPILE","MISSING_CAPABILITY","AMBIGUOUS","RESOURCE_CONFLICT"]},
            "language":{"type":"string","enum":["pt","en","es","other"]},
            "program":{"type":["object","null"],"properties":{
                "mode":{"type":"string","enum":["MANUAL","BOOT","AT","EVERY","WEEKLY"]},
                "schedule":{"type":["object","null"],"properties":{
                    "time":{"type":"string"},"date":{"type":"string"},"timezone":{"type":"string","enum":["LOCAL","UTC"]},
                    "epoch_utc":integer,"interval_s":integer,"anchor_epoch_utc":integer,
                    "days":{"type":"array","items":{"type":"string","enum":["MON","TUE","WED","THU","FRI","SAT","SUN"]}}
                },"additionalProperties":False},
                "actions":{"type":"array","items":{"$ref":"#/$defs/action"}}
            },"required":["mode","actions"],"additionalProperties":False},
            "unresolved_clauses":{"type":"array","items":{"type":"string"}},
            "missing_capabilities":{"type":"array","items":{"type":"string"}},
            "notes":{"type":"array","items":{"type":"string"}}
        },
        "required":["ir_version","status","language","program","unresolved_clauses","missing_capabilities","notes"],
        "additionalProperties":False
    }


def _call_openrouter(cfg: AgentConfig, system: str, user: str,
                     response_kind: str = "semantic_ir",
                     semantic_ops: Optional[list[str]] = None) -> str:
    key = os.getenv(cfg.api_key_env or "OPENROUTER_API_KEY")
    if not key:
        raise AgentError(f"missing API key environment variable: {cfg.api_key_env or 'OPENROUTER_API_KEY'}")
    # R3.8.20c2: native structured output + low-latency routing. Local validation
    # is still authoritative. Minimal reasoning is appropriate for schema mapping.
    payload={
        "model": cfg.model,
        "temperature": 0,
        "reasoning": {"effort":"minimal", "exclude": True},
        "response_format": {
            "type": "json_schema",
            "json_schema": {"name":"nrfclaw_semantic_ir", "strict": True,
                            "schema": _semantic_ir_response_schema(semantic_ops)}
        },
        "provider": {"require_parameters": True, "sort": "latency"},
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
    }
    headers={"Authorization": f"Bearer {key}", "HTTP-Referer": "https://www.blusense.com.br/", "X-Title": "nRFClaw CLI"}
    url=cfg.base_url.rstrip("/") + "/chat/completions"

    if response_kind not in ("semantic_ir", "json_object"):
        raise AgentError(f"unsupported response_kind {response_kind!r}")

    # Generic JSON/QA requests must not be constrained by the Semantic IR schema.
    if response_kind == "json_object":
        payload_json = dict(payload)
        payload_json.pop("reasoning", None)
        payload_json["response_format"] = {"type": "json_object"}
        payload_json["provider"] = {"sort": "latency"}
        obj = _request_json(url, payload_json, headers, cfg.timeout_s)
        _set_provider_meta(obj, "json_object")
        try:
            return _normalize_chat_content(obj["choices"][0]["message"]["content"])
        except Exception as exc:
            if isinstance(exc, AgentError):
                raise
            raise AgentError("invalid OpenRouter response") from exc

    try:
        # Stage 1: strongest contract. Only endpoints that explicitly support the
        # requested structured-output/reasoning parameters are eligible.
        obj = _request_json(url, payload, headers, cfg.timeout_s)
        stage="json_schema"

        # Some provider/model combinations advertise json_schema support and
        # accept the request, but return an empty JSON object even though the
        # schema requires fields. Treat that as a transport/schema capability
        # failure and retry using generic JSON mode. Local validate_ir remains
        # authoritative; no Semantic IR fields are invented or coerced here.
        try:
            _schema_content = _normalize_chat_content(
                obj["choices"][0]["message"]["content"]
            )
            _schema_obj = json.loads(_schema_content)
        except Exception:
            _schema_obj = None

        if isinstance(_schema_obj, dict) and not _schema_obj:
            payload2 = dict(payload)
            payload2.pop("reasoning", None)
            payload2["response_format"] = {"type": "json_object"}
            payload2["provider"] = {"sort": "latency"}
            obj = _request_json(url, payload2, headers, cfg.timeout_s)
            stage = "json_object_after_empty_schema"

    except AgentError as exc1:
        if "HTTP 400" not in str(exc1) and "HTTP 404" not in str(exc1):
            raise
        # Stage 2: compatibility JSON mode. IMPORTANT: do not keep
        # require_parameters=True here. R3.8.20c2 accidentally did that, so the
        # router repeated the same parameter-capability filter and could return
        # another 404 before the model was called.
        payload2=dict(payload)
        payload2.pop("reasoning", None)
        payload2["response_format"]={"type":"json_object"}
        payload2["provider"]={"sort":"latency"}
        try:
            obj = _request_json(url, payload2, headers, cfg.timeout_s)
            stage="json_object"
        except AgentError as exc2:
            if "HTTP 400" not in str(exc2) and "HTTP 404" not in str(exc2):
                raise
            # Stage 3: maximum compatibility. The strong system prompt plus the
            # local Semantic IR validator remain authoritative even when the
            # selected endpoint exposes neither native schema nor JSON mode.
            payload3=dict(payload2)
            payload3.pop("response_format", None)
            payload3.pop("provider", None)
            obj = _request_json(url, payload3, headers, cfg.timeout_s)
            stage="plain"
    _set_provider_meta(obj, stage)
    try:
        return _normalize_chat_content(obj["choices"][0]["message"]["content"])
    except Exception as exc:
        if isinstance(exc, AgentError): raise
        raise AgentError("invalid OpenRouter response") from exc


def _call_gemini(cfg: AgentConfig, system: str, user: str) -> str:
    env = cfg.api_key_env or "GEMINI_API_KEY"
    key = os.getenv(env)
    if not key:
        raise AgentError(f"missing API key environment variable: {env}")
    url = f"{cfg.base_url.rstrip('/')}/models/{cfg.model}:generateContent?key={key}"
    obj = _request_json(url, {
        "systemInstruction": {"parts": [{"text": system}]},
        "contents": [{"role": "user", "parts": [{"text": user}]}],
        "generationConfig": {"temperature": 0, "responseMimeType": "application/json"},
    }, {}, cfg.timeout_s)
    try:
        return obj["candidates"][0]["content"]["parts"][0]["text"]
    except Exception as exc:
        raise AgentError("invalid Gemini response") from exc


def call_provider(cfg: AgentConfig, system: str, user: str,
                  response_kind: str = "semantic_ir",
                  semantic_ops: Optional[list[str]] = None) -> str:
    if cfg.provider == "ollama":
        return _call_ollama(cfg, system, user)
    if cfg.provider == "openrouter":
        return _call_openrouter(cfg, system, user, response_kind=response_kind, semantic_ops=semantic_ops)
    if cfg.provider == "gemini":
        return _call_gemini(cfg, system, user)
    raise AgentError(f"unsupported provider '{cfg.provider}'")


def _extract_json(text: str) -> Dict[str, Any]:
    s = text.strip()
    if s.startswith("```"):
        s = re.sub(r"^```(?:json)?\s*", "", s)
        s = re.sub(r"\s*```$", "", s)
    try:
        obj = json.loads(s)
    except json.JSONDecodeError:
        start, end = s.find("{"), s.rfind("}")
        if start < 0 or end <= start:
            raise AgentError("agent did not return JSON")
        try:
            obj = json.loads(s[start:end + 1])
        except json.JSONDecodeError as exc:
            raise AgentError(f"agent returned invalid JSON: {exc}") from exc
    if not isinstance(obj, dict):
        raise AgentError("agent JSON root must be an object")
    return obj


def _int_range(name: str, value: Any, lo: int, hi: int) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or int(value) != value:
        raise AgentError(f"{name} must be an integer")
    value = int(value)
    if value < lo or value > hi:
        raise AgentError(f"{name} must be {lo}..{hi}")
    return value


def validate_plan(plan: Dict[str, Any]) -> Dict[str, Any]:
    allowed_top = {"schema_version", "supported", "error", "schedule", "ndp_off", "intent",
                   "tracking", "motion", "temperature", "beacon"}
    extra = set(plan) - allowed_top
    if extra:
        raise AgentError(f"unknown semantic fields: {', '.join(sorted(extra))}")
    if plan.get("schema_version") != SCHEMA_VERSION:
        raise AgentError(f"semantic schema mismatch: expected {SCHEMA_VERSION}")
    if plan.get("supported") is False:
        raise AgentError("agent cannot represent request safely: " + str(plan.get("error") or "unsupported"))
    if plan.get("supported") not in (True, None):
        raise AgentError("supported must be boolean")
    schedule = plan.get("schedule", "MANUAL")
    if schedule not in ("BOOT", "MANUAL"):
        raise AgentError("schedule must be BOOT or MANUAL")
    plan["schedule"] = schedule
    if not isinstance(plan.get("ndp_off", False), bool):
        raise AgentError("ndp_off must be boolean")
    intent = plan.get("intent")
    if intent not in ("tracking", "motion_tracking", "temperature_flow", "beacon"):
        raise AgentError(f"unsupported semantic intent: {intent!r}")

    tr = plan.get("tracking")
    if tr is not None:
        if not isinstance(tr, dict): raise AgentError("tracking must be an object")
        tr["interval_ms"] = _int_range("tracking.interval_ms", tr.get("interval_ms", 1000), 1000, 10000)
        if tr["interval_ms"] % 1000:
            raise AgentError("current text compiler supports whole-second tracking intervals")
        tr["tx_power_dbm"] = _int_range("tracking.tx_power_dbm", tr.get("tx_power_dbm", 4), -40, 8)
        if tr["tx_power_dbm"] != 4:
            raise AgentError("current deterministic compiler supports tracking TX power +4 dBm only")

    mo = plan.get("motion")
    if mo is not None:
        if not isinstance(mo, dict): raise AgentError("motion must be an object")
        mo["threshold_mg"] = _int_range("motion.threshold_mg", mo.get("threshold_mg", 250), 16, 2000)
        mo["duration_ms"] = _int_range("motion.duration_ms", mo.get("duration_ms", 100), 100, 25000)
        if mo["threshold_mg"] != 250 or mo["duration_ms"] != 100:
            raise AgentError("current text compiler exposes motion defaults 250mg/100ms only")

    temp = plan.get("temperature")
    if temp is not None:
        if not isinstance(temp, dict): raise AgentError("temperature must be an object")
        if temp.get("operator") not in ("lt", "gt"):
            raise AgentError("temperature.operator must be lt or gt")
        if not isinstance(temp.get("threshold_c"), (int, float)):
            raise AgentError("temperature.threshold_c must be numeric")
        interval = temp.get("interval_ms")
        if interval is not None:
            temp["interval_ms"] = _int_range("temperature.interval_ms", interval, 1000, 86400000)
            if temp["interval_ms"] % 1000:
                raise AgentError("current text compiler supports whole-second temperature intervals")

    beacon = plan.get("beacon")
    if beacon is not None:
        if not isinstance(beacon, dict): raise AgentError("beacon must be an object")
        name = beacon.get("name", "nRFClaw")
        if not isinstance(name, str) or not (1 <= len(name.encode("utf-8")) <= 24):
            raise AgentError("beacon.name must be 1..24 UTF-8 bytes")
        beacon["name"] = name
        beacon["interval_ms"] = _int_range("beacon.interval_ms", beacon.get("interval_ms", 2000), 1000, 10000)
        if beacon["interval_ms"] % 1000:
            raise AgentError("current text compiler supports whole-second beacon intervals")
        beacon["wait_s"] = _int_range("beacon.wait_s", beacon.get("wait_s", 0), 0, 86400)
        beacon["tx_power_dbm"] = _int_range("beacon.tx_power_dbm", beacon.get("tx_power_dbm", 4), -40, 8)
    return plan


def plan_to_canonical_prompt(plan: Dict[str, Any]) -> str:
    plan = validate_plan(plan)
    p = []
    if plan["schedule"] == "BOOT": p.append("at boot")
    if plan.get("ndp_off"): p.append("disable ndp")
    intent = plan["intent"]
    tr = plan.get("tracking") or {"interval_ms": 1000}

    if intent == "tracking":
        p.append(f"start tracking every {tr['interval_ms'] // 1000} seconds")
    elif intent == "motion_tracking":
        p.append("enable motion and when motion is detected")
        p.append(f"start tracking every {tr['interval_ms'] // 1000} seconds")
    elif intent == "temperature_flow":
        te = plan.get("temperature")
        if not te: raise AgentError("temperature_flow requires temperature")
        if te.get("interval_ms") is None:
            p.append("read temperature")
        else:
            p.append(f"read temperature every {te['interval_ms'] // 1000} seconds")
        op = "below" if te["operator"] == "lt" else "above"
        p.append(f"if temperature is {op} {te['threshold_c']:g} degrees")
        if plan.get("motion") is not None:
            p.append("enable motion and when motion is detected")
        if plan.get("tracking") is not None:
            p.append(f"start tracking every {tr['interval_ms'] // 1000} seconds")
    elif intent == "beacon":
        b = plan.get("beacon")
        if not b: raise AgentError("beacon intent requires beacon")
        if b.get("wait_s", 0): p.append(f"wait {b['wait_s']} seconds")
        p.append(f"advertise {b['name']} every {b['interval_ms'] // 1000} seconds")
    return "; ".join(p)


def interpret_with_agent(source: str, cfg: AgentConfig, provider_call=None) -> Tuple[Dict[str, Any], str]:
    if not source.strip(): raise AgentError("empty prompt")
    system = build_system_prompt()
    call = provider_call or call_provider
    raw = call(cfg, system, source)
    plan = validate_plan(_extract_json(raw))
    canonical = plan_to_canonical_prompt(plan)
    return plan, canonical


def compile_with_agent(source: str, cfg: AgentConfig, force_boot: bool = False,
                       provider_call=None) -> Tuple[CompileResult, Dict[str, Any], str]:
    plan, canonical = interpret_with_agent(source, cfg, provider_call=provider_call)
    if force_boot and plan["schedule"] != "BOOT":
        plan["schedule"] = "BOOT"
        canonical = plan_to_canonical_prompt(plan)
    try:
        result, _meta = compile_hybrid(canonical, force_boot=force_boot)
    except TextCompileError as exc:
        raise AgentError(f"deterministic compiler rejected agent plan: {exc}") from exc
    return result, plan, canonical
