#!/usr/bin/env python3
"""Offline domain language front-end for nRFClaw.

This is deliberately NOT a general machine translator.  It performs a
conservative, deterministic semantic normalization of supported nRFClaw domain
phrases into canonical English so the standalone engine has one internal
language.  Unknown wording is preserved instead of guessed.
"""
from __future__ import annotations

from dataclasses import dataclass
import re
import unicodedata

# R3.8.20c2e5: English is the canonical/default language for standalone
# diagnostics and internal semantic normalization. PT/ES remain accepted inputs.
DEFAULT_LANGUAGE = "en"


@dataclass(frozen=True)
class LanguageNormalization:
    original: str
    language: str
    canonical_english: str
    changed: bool


def _ascii_fold(text: str) -> str:
    s = unicodedata.normalize("NFKD", text)
    return "".join(c for c in s if not unicodedata.combining(c))


def detect_language(text: str) -> str:
    """Small offline heuristic: en/pt/es/unknown.

    It is only used for diagnostics; normalization is phrase-driven and does
    not require the detector to be perfect.
    """
    t = " " + _ascii_fold(text).lower() + " "
    scores = {"en": 0, "pt": 0, "es": 0}
    for w in (" the ", " when ", " every ", " seconds ", " start ", " send ", " with ", " learning ", " above ", " below "):
        scores["en"] += t.count(w)
    for w in (" quando ", " cada ", " segundos ", " inicie ", " envie ", " desligue ", " receba ", " retransmita ", " com ", " aprendizado ", " acima ", " abaixo ", " mensagem ", " serial "):
        scores["pt"] += t.count(w)
    for w in (" cuando ", " cada ", " segundos ", " inicia ", " envie ", " con ", " aprendizaje ", " encima ", " debajo ", " mensaje "):
        scores["es"] += t.count(w)
    best = max(scores, key=scores.get)
    return best if scores[best] else DEFAULT_LANGUAGE


def _sub(pattern: str, repl: str, text: str) -> str:
    return re.sub(pattern, repl, text, flags=re.I)


def _normalize_typographic_quotes(text: str) -> str:
    """Normalize Unicode smart-quote delimiters used by browsers/mobile keyboards.

    This is syntax normalization, not semantic translation. It lets quoted
    beacon names/payloads typed as ‘OPEN1’ or “OPEN1” follow the same
    deterministic parser path as ASCII quotes.
    """
    return text.translate(str.maketrans({
        "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
        "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"',
    }))


def canonicalize_to_english(text: str) -> LanguageNormalization:
    """Translate supported nRFClaw semantics to canonical English offline.

    Quoted payload/name strings are protected and restored byte-for-byte.
    The translation is intentionally conservative and domain-specific.
    """
    original = text
    text = _normalize_typographic_quotes(text)
    language = detect_language(text)

    # Protect quoted user data from translation/folding.
    protected: list[str] = []
    def protect(m: re.Match[str]) -> str:
        protected.append(m.group(0))
        return f" __NRFCLAW_Q{len(protected)-1}__ "
    s = re.sub(r'"[^"\\]*(?:\\.[^"\\]*)*"|\'[^\'\\]*(?:\\.[^\'\\]*)*\'', protect, text)
    s = _ascii_fold(s).lower()
    s = re.sub(r"\s+", " ", s).strip()

    # Structural/time phrases first.
    rules = [
        (r"\bno boot\b|\bao iniciar\b|\bquando iniciar\b|\bquando ligar\b|\bna inicializacao\b|\bal arrancar\b|\bal iniciar\b", "at boot"),
        (r"\ba cada\b|\bcada\b", "every"),
        (r"\bde dois em dois\b", "every 2"),
        (r"\bde tres em tres\b", "every 3"),
        (r"\bde cinco em cinco\b", "every 5"),
        (r"\bsegundo(?:s)?\b|\bsegundo(?:s)?\b", "seconds"),
        (r"\bminuto(?:s)?\b", "minutes"),
        (r"\bhora(?:s)?\b", "hours"),
        (r"\bgrau(?:s)?\b", "degrees"),
    ]
    for p, r in rules:
        s = _sub(p, r, s)

    # Domain noun phrases.
    phrases = [
        (r"\bservico de deteccao de vibracao\b|\bservicio de deteccion de vibracion\b", "vibration detection service"),
        (r"\bdeteccao de vibracao\b|\bdeteccion de vibracion\b", "vibration detection"),
        (r"\baprendizado\b|\baprendizagem\b|\baprendizaje\b", "learning"),
        (r"\btempo inicial\b|\btiempo inicial\b|\batraso inicial\b|\bretardo inicial\b", "initial time"),
        (r"\bmensagens\b|\bmensagem\b|\bmensajes\b|\bmensaje\b", "message"),
        (r"\bcontador\b|\bcontado\b|\bcontador(?:es)?\b", "counter"),
        (r"\bbateria\b|\bbateria\b", "battery"),
        (r"\btemperatura\b", "temperature"),
        (r"\bmovimento\b|\bmovimentacao\b|\bmovimiento\b", "motion"),
        (r"\brastreamento\b|\brastreo\b", "tracking"),
        (r"\bmaquina\b|\bmaquina\b", "machine"),
        (r"\banomalia\b", "anomaly"),
        (r"\balerta\b", "alert"),
        (r"\bpino\b", "pin"),
        (r"\bporta serial\b|\bconexao serial\b|\bconexão serial\b|\bserial connection\b", "serial"),
        (r"\btransmissao do beacon\b|\btransmissao beacon\b", "beacon transmission"),
        (r"\bvalor recebido\b|\bdado recebido\b|\bdados recebidos\b", "received value"),
    ]
    for p, r in phrases:
        s = _sub(p, r, s)

    # Verbs/control words. Keep them narrow to avoid rewriting payloads (already protected).
    words = [
        (r"\binicie\b|\biniciar\b|\binicia\b|\bcomece\b|\bcomience\b|\bative\b|\bativar\b|\bhabilite\b|\bhabilita\b", "start"),
        (r"\breceba\b|\breceber\b|\brecebe\b|\bescute\b|\bescutar\b|\bfique recebendo\b|\bficar recebendo\b", "receive"),
        (r"\bretransmita\b|\bretransmitir\b|\brepasse\b|\bencaminhe\b", "forward"),
        (r"\benvie\b|\benvia\b|\benviar\b|\btransmita\b|\bmande\b|\bmanda\b", "send"),
        (r"\bleia\b|\bler\b|\blea\b", "read"),
        (r"\bdesligue\b|\bdesative\b|\bdesabilite\b|\bapague\b", "disable"),
        (r"\bpare\b|\bparar\b|\bdetenga\b", "stop"),
        (r"\bquando\b|\bcuando\b", "when"),
        (r"\bse\b|\bsi\b", "if"),
        (r"\bsenao\b|\bcaso contrario\b", "else"),
        (r"\bacima de\b|\bmaior que\b|\bsuperior a\b|\bpor encima de\b", "above"),
        (r"\babaixo de\b|\bmenor que\b|\binferior a\b|\bpor debajo de\b", "below"),
        (r"\bcom\b|\bcon\b", "with"),
        (r"\bpor lora\b|\bvia lora\b", "over lora"),
        (r"\binformando\b", "containing"),
        (r"\bexibindo\b|\bexiba\b|\bmostrar\b|\bmostre\b", "display"),
        (r"\bchegar\b|\bchegue\b|\bchega\b", "arrive"),
        (r"\bmais\b|\bmas\b", "plus"),
        (r"\be depois\b|\by despues\b", "then"),
        (r"\be\b|\by\b", "and"),
        (r"\bo\b|\ba\b|\bel\b|\bla\b", "the"),
        (r"\bum\b|\buma\b|\bun\b|\buna\b", "a"),
    ]
    for p, r in words:
        s = _sub(p, r, s)

    # Normalize common English variants into the engine's canonical shape.
    s = _sub(r"\b(\d+)\s*[- ]hour\s+learning\s+period\b", r"learning \1 hours", s)
    s = _sub(r"\blearning\s+period\s+(?:of|for)\s+(\d+)\s*(seconds?|minutes?|hours?)\b", r"learning \1 \2", s)
    s = _sub(r"\binitial\s+time\s+of\s+(\d+)\s*(seconds?|minutes?|hours?)\b", r"initial time \1 \2", s)
    s = _sub(r"\binitial\s+delay\s+of\s+(\d+)\s*(seconds?|minutes?|hours?)\b", r"initial delay \1 \2", s)
    s = _sub(r"\blearning\s+(?:of|for)\s+(\d+)\s*(seconds?|minutes?|hours?)\b", r"learning \1 \2", s)

    # Event/passive-voice normalization used by composed event handlers.
    # Do this after noun/verb translation so PT/ES variants converge to one
    # canonical English event phrase.
    s = _sub(r"\bwhen\s+for\s+detectado\s+(?:the|a)\s+anomaly\b", "when an anomaly is detected", s)
    s = _sub(r"\bwhen\s+(?:for\s+)?detectada\s+(?:the|a)\s+anomaly\b", "when an anomaly is detected", s)
    s = _sub(r"\bwhen\s+se\s+detecte\s+(?:the|a)\s+anomaly\b", "when an anomaly is detected", s)
    s = _sub(r"\bwhen\s+(?:an|a)\s+anomaly\s+is\s+detected\b", "when an anomaly is detected", s)

    # Portuguese/Spanish linker artifacts after noun translation.
    s = _sub(r"\blearning\s+de\s+(\d+)\b", r"learning \1", s)
    s = _sub(r"\binitial time\s+de\s+(\d+)\b", r"initial time \1", s)
    s = _sub(r"\bwith the learning\b", "with learning", s)
    s = _sub(r"\bsend over lora a message\b", "send a lora message", s)
    s = _sub(r"\bmessage\s+lora\b", "lora message", s)
    s = _sub(r"\bsend\s+over lora\s+(?:un|um|uma)\s+message\b", "send a lora message", s)
    s = _sub(r"\breceive\s+(?:the\s+)?(?:lora\s+)?messages?\s+(?:lora|over lora)\b", "receive lora messages", s)
    s = _sub(r"\breceive\s+(?:the\s+)?messages?\s+over lora\b", "receive lora messages", s)
    s = _sub(r"\bforward\s+(?:the\s+)?(?:messages?|data)?\s*(?:to|on|over|via|in)?\s*(?:the\s+)?serial(?: connection)?\b", "forward received data to serial", s)
    s = _sub(r"\bsend\s+(?:un|um|uma)\s+lora message\b", "send a lora message", s)
    s = _sub(r"\bwith\s+(?:un|um|uma)\s+counter\b", "with a counter", s)
    s = _sub(r"\bstart\s+o\s+vibration\b", "start vibration", s)
    s = _sub(r"\bstart\s+el\s+vibration\b", "start vibration", s)
    s = _sub(r"\breceive\s+as\s+lora\s+message\b", "receive lora messages", s)
    s = _sub(r"\breceive\s+as\s+message\s+lora\b", "receive lora messages", s)
    s = _sub(r"\breceive\s+(?:the\s+)?message\s+lora\b", "receive lora messages", s)
    s = _sub(r"\bforward\s+na\s+serial\b", "forward received data to serial", s)
    s = _sub(r"\bforward\s+no\s+serial\b", "forward received data to serial", s)
    s = _sub(r"\bforward\s+(?:them|it)\s+to\s+the\s+serial\b", "forward received data to serial", s)
    s = _sub(r"\breceive\s+(?:the\s+)?lora\s+message(?:s)?\b", "receive lora messages", s)
    s = _sub(r"\bwhen\s+(?:the\s+)?message\s+arrive\b", "when the message arrives", s)
    s = _sub(r"\bstart\s+(?:the\s+)?beacon transmission\b", "start beacon", s)
    s = _sub(r"\bbeacon\s+display\s+(?:the\s+)?received value\b", "beacon display received value", s)

    s = re.sub(r"\s+", " ", s).strip()

    # Restore quoted data exactly as supplied.
    for i, q in enumerate(protected):
        s = s.replace(f"__nrfclaw_q{i}__", q)
        s = s.replace(f"__NRFCLAW_Q{i}__", q)

    return LanguageNormalization(original, language, s, s != original.strip())
