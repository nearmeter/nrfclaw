#!/usr/bin/env python3
"""
nRFClaw Stage-4 BLE/NUS client.

Preserves all useful Stage-3 commands (battery, notify-string,
test-battery-lora) and adds Stage-4 Flash commit verification.

Requires:
    python3 -m pip install bleak
"""

import argparse
import base64
import csv
import json
import zlib
import hashlib
import hmac
import os
import asyncio
import struct
import sys
import time
from pathlib import Path
from urllib.parse import urlencode

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    BleakClient = None
    BleakScanner = None

from nrfclaw_text_compiler import (
    compile_text as compile_natural_text,
    format_ir as format_natural_ir,
    bytecode_hex as natural_bytecode_hex,
    TextCompileError,
    UnsupportedSemantics,
)
from nrfclaw_schedule_semantics import normalize_schedule, schedule_mode_name, format_schedule_abi
from nrfclaw_hybrid_semantic import (
    compile_hybrid,
    load_config as load_semantic_config,
    update_config as update_semantic_config,
    teach_example as semantic_teach_example,
    DEFAULT_CONFIG as SEMANTIC_CONFIG_PATH,
    USER_EXAMPLES as SEMANTIC_USER_EXAMPLES,
    SEMANTIC_ENGINE_VERSION,
    semantic_diagnose,
)
from nrfclaw_product_qa import answer_question, KB_PATH as PRODUCT_KB_PATH
from nrfclaw_pseudo import compile_pseudo, pseudo_from_result, GRAMMAR as PSEUDO_GRAMMAR
from nrfclaw_language import canonicalize_to_english
from nrfclaw_semantic_ir import (
    IR_VERSION as SEMANTIC_IR_VERSION,
    VM_ABI as SEMANTIC_IR_VM_ABI,
    schema_contract as semantic_ir_schema_contract,
    validate_ir as validate_semantic_ir,
    compile_ir as compile_semantic_ir,
    SemanticIRError,
    SemanticIRMissingCapability,
)
from nrfclaw_agent.router import (
    external_available as agent_external_available,
    plan_program as agent_plan_program,
    plan_ir as agent_plan_ir,
    plan_ir_with_metadata as agent_plan_ir_with_metadata,
    answer_external as agent_answer_external,
    knowledge_sha256 as agent_knowledge_sha256,
)
from nrfclaw_agent.core import (
    AgentConfig,
    AgentError,
    load_config as load_agent_config,
    configure_agent,
    disable_agent,
    provider_catalog,
    compile_with_agent,
    DEFAULT_CONFIG as AGENT_CONFIG_PATH,
    KB_VERSION as AGENT_KB_VERSION,
    SCHEMA_VERSION as AGENT_SCHEMA_VERSION,
    VM_ABI as AGENT_VM_ABI,
)



TRACKING_KDF_LABEL = b"nRFClaw-OH1"
TRACKING_SEED_SIZE = 32
TRACKING_PRIVATE_KEY_SIZE = 28
TRACKING_MAX_EXPORT_KEYS = 250
TRACKING_KEYGEN_URL = "https://keygen.nrfclaw.cloud/"


def derive_tracking_keypair(seed: bytes, index: int):
    """Derive exactly the same secp224r1 keypair used by nrfclaw_tracking.c.

    Firmware uses SHA256(label || seed || index_be32 || retry), copies the
    first 28 digest bytes into micro-ecc built with native-little-endian VLI,
    then advertises the public X coordinate serialized big-endian.
    """
    if len(seed) != TRACKING_SEED_SIZE:
        raise ValueError("tracking seed must be exactly 32 bytes")
    if index < 0 or index > 0xFFFFFFFF:
        raise ValueError("tracking key index must be 0..4294967295")

    try:
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError as exc:
        raise RuntimeError(
            "tracking-export requires Python package 'cryptography'; "
            "install with: python3 -m pip install cryptography"
        ) from exc

    prefix = TRACKING_KDF_LABEL + seed + index.to_bytes(4, "big")
    for retry in range(256):
        digest = hashlib.sha256(prefix + bytes([retry])).digest()
        native_private = digest[:TRACKING_PRIVATE_KEY_SIZE]
        # micro-ecc receives these bytes as a native-little-endian scalar.
        scalar = int.from_bytes(native_private, "little")
        if scalar == 0:
            continue
        try:
            key = ec.derive_private_key(scalar, ec.SECP224R1())
        except ValueError:
            continue
        x = key.public_key().public_numbers().x
        adv_key = x.to_bytes(TRACKING_PRIVATE_KEY_SIZE, "big")
        # Macless/OpenHaystack expects the private scalar in standard
        # big-endian representation, not micro-ecc's native byte layout.
        private_key = scalar.to_bytes(TRACKING_PRIVATE_KEY_SIZE, "big")
        return private_key, adv_key, retry

    raise RuntimeError(f"unable to derive valid P-224 key for index {index}")


def tracking_mac_from_adv_key(adv_key: bytes) -> str:
    if len(adv_key) != 28:
        raise ValueError("advertisement key must be 28 bytes")
    # Firmware sets addr[5..0] from key[0..5], with top two bits forced to 1.
    display = bytes([adv_key[0] | 0xC0]) + adv_key[1:6]
    return ":".join(f"{b:02X}" for b in display)


def build_tracking_provision_url(
    seed: bytes,
    device_name: str,
    current_index: int,
    rotation_seconds: int,
    base_url: str = TRACKING_KEYGEN_URL,
) -> str:
    """Build a client-side nRFClaw keygen provisioning URL.

    Sensitive provisioning values live in the URL fragment, so normal HTTP
    requests to the keygen host do not include the tracking seed. The QR/URL
    itself remains secret because anyone who obtains the seed can derive the
    rotating private-key sequence.
    """
    if len(seed) != TRACKING_SEED_SIZE:
        raise ValueError("tracking seed must be exactly 32 bytes")
    if current_index < 0 or current_index > 0xFFFFFFFF:
        raise ValueError("tracking key index must be 0..4294967295")
    if rotation_seconds < 1 or rotation_seconds > 0xFFFFFFFF:
        raise ValueError("tracking rotation must be 1..4294967295 seconds")
    device_name = str(device_name).strip()
    if not device_name:
        raise ValueError("device name must not be empty")

    base_url = str(base_url).strip()
    if not base_url:
        raise ValueError("keygen base URL must not be empty")
    # Never let a caller-supplied query/fragment carry provisioning data.
    base_url = base_url.split("#", 1)[0].split("?", 1)[0]
    if not base_url.endswith("/"):
        base_url += "/"

    fragment = urlencode(
        {
            "v": 1,
            "device": device_name,
            "seed": seed.hex(),
            "index": current_index,
            "rotation": rotation_seconds,
        }
    )
    return f"{base_url}#{fragment}"


def build_tracking_provision_qr_matrix(url: str):
    """Return a QR matrix using the bundled pure-Python encoder.

    This path intentionally has no external runtime dependency. PNG export is
    kept separate and may use the optional third-party ``qrcode`` package.
    """
    import nrfclaw_qrcode

    qr = nrfclaw_qrcode.QRCode(
        version=None,
        error_correction=nrfclaw_qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)
    return qr.get_matrix()


def render_tracking_provision_qr_unicode(url: str, ansi: bool = True) -> str:
    """Render a compact two-module-per-row QR for a terminal.

    ANSI mode fixes foreground/background to black/white, so scanning does not
    depend on whether the user's terminal theme is light or dark. Each Unicode
    character represents two vertical QR modules.
    """
    matrix = build_tracking_provision_qr_matrix(url)
    width = len(matrix[0]) if matrix else 0
    if width == 0:
        return ""

    # A blank row is appended when the QR height is odd. The QR quiet-zone is
    # already part of the matrix, so this does not reduce the required border.
    rows = [list(row) for row in matrix]
    if len(rows) % 2:
        rows.append([False] * width)

    lines = []
    if ansi:
        # top,bottom -> ANSI foreground/background plus upper-half block.
        # Spaces are used when both halves have the same colour.
        for y in range(0, len(rows), 2):
            top = rows[y]
            bottom = rows[y + 1]
            parts = []
            current = None
            for t, b in zip(top, bottom):
                if t and b:
                    state, glyph = "40", " "       # black / black
                elif (not t) and (not b):
                    state, glyph = "47", " "       # white / white
                elif t and not b:
                    state, glyph = "30;47", "▀"    # black / white
                else:
                    state, glyph = "37;40", "▀"    # white / black
                if state != current:
                    parts.append(f"\x1b[{state}m")
                    current = state
                parts.append(glyph)
            parts.append("\x1b[0m")
            lines.append("".join(parts))
    else:
        # Plain Unicode assumes a light terminal background. It is primarily
        # useful for logs/tests; ANSI mode is the production terminal default.
        glyphs = {
            (False, False): " ",
            (True, True): "█",
            (True, False): "▀",
            (False, True): "▄",
        }
        for y in range(0, len(rows), 2):
            lines.append(
                "".join(glyphs[(t, b)] for t, b in zip(rows[y], rows[y + 1]))
            )

    return "\n".join(lines)


def write_tracking_provision_qr(url: str, output_path) -> Path:
    """Write provisioning URL as a QR PNG using optional external qrcode."""
    try:
        import qrcode
    except ImportError as exc:
        raise RuntimeError(
            "PNG QR generation requires optional package 'qrcode'; "
            "install with: python3 -m pip install 'qrcode[pil]'"
        ) from exc

    path = Path(output_path)
    path.parent.mkdir(parents=True, exist_ok=True)
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=8,
        border=4,
    )
    qr.add_data(url)
    qr.make(fit=True)
    image = qr.make_image(fill_color="black", back_color="white")
    image.save(path)
    return path


def build_macless_devices_json(name: str, key_rows):
    """Create the Macless-Haystack devices.json object for rotating keys."""
    if not key_rows:
        raise ValueError("at least one tracking key is required")

    # Macless-Haystack's generator treats the last key as the primary key and
    # all prior keys as additionalKeys. Keep the same convention.
    primary = key_rows[-1]
    additional = key_rows[:-1]
    return [
        {
            "id": name,
            "colorComponents": [0, 1, 0, 1],
            "name": name,
            "privateKey": primary["private_b64"],
            "hashedAdvKey": primary["hashed_adv_b64"],
            "icon": "",
            "isActive": True,
            "additionalKeys": [r["private_b64"] for r in additional],
            "additionalHashedAdvKeys": [
                r["hashed_adv_b64"] for r in additional
            ],
        }
    ]

def parse_duration_seconds(value: str) -> int:
    s = str(value).strip().lower()
    mult = 1
    if s.endswith("s"):
        s = s[:-1]
    elif s.endswith("m"):
        mult, s = 60, s[:-1]
    elif s.endswith("h"):
        mult, s = 3600, s[:-1]
    elif s.endswith("d"):
        mult, s = 86400, s[:-1]
    v = int(s) * mult
    if v < 1 or v > 604800:
        raise argparse.ArgumentTypeError("duration must be between 1s and 7d")
    return v

def format_duration(seconds: int) -> str:
    if seconds % 86400 == 0:
        return f"{seconds // 86400}d"
    if seconds % 3600 == 0:
        return f"{seconds // 3600}h"
    if seconds % 60 == 0:
        return f"{seconds // 60}m"
    return f"{seconds}s"


NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Pack 02 Application / Home Assistant plane
APP_SERVICE = "4e52a800-0000-10a7-d24b-6a3244619a47"
APP_RX = "4e52a801-0000-10a7-d24b-6a3244619a47"
APP_TX = "4e52a802-0000-10a7-d24b-6a3244619a47"


# NDP Stage 10.2.3a
NDP_INFO             = 0x20
NDP_CAPS             = 0x21
NDP_STATUS           = 0x22
NDP_BOARD_INFO       = 0x23
NDP_BOARD_MEMORY     = 0x24
NDP_BOARD_RESET      = 0x25
NDP_BOARD_BOOTLOADER = 0x26
NDP_SENSOR_READ      = 0x34
NDP_RADIO_GET        = 0x38
NDP_RADIO_SET        = 0x39
NDP_HALL_CONFIG      = 0x3A

# Pack 02-r2 NDP security / sensors
NDP_AUTH_BEGIN        = 0x40
NDP_AUTH_FINISH       = 0x41
NDP_AUTH_LOGOUT       = 0x42
NDP_ACCESS_STATUS     = 0x43
NDP_ACCEL_CONFIG      = 0x44
NDP_VIB_INFO          = 0x45
NDP_VIB_READ          = 0x46
NDP_ACCEL_PROBE       = 0x47
NDP_VIB_MODEL_SET      = 0x48  # EXPERIMENTAL
NDP_VIB_HEALTH         = 0x49  # EXPERIMENTAL
NDP_VIB_AUTO_START     = 0x4A  # EXPERIMENTAL
NDP_VIB_AUTO_STOP      = 0x4B  # EXPERIMENTAL
NDP_VIB_AUTO_STATUS    = 0x4C  # EXPERIMENTAL
NDP_VIB_AUTO_CONFIG    = 0x4D  # EXPERIMENTAL
NDP_VIB_AUTO_BASELINE  = 0x4E  # EXPERIMENTAL
NDP_VIB_AUTO_RESET     = 0x4F  # EXPERIMENTAL
NDP_KEY_GENERATE        = 0x50  # physical NUS only
NDP_KEY_GET             = 0x51  # physical NUS only
NDP_KEY_STATUS          = 0x52  # physical NUS only
NDP_BLE_BOOT_CONTROL    = 0x53  # r3.8.10 physical NUS only
NDP_BLE_BEACON_CONTROL  = 0x54  # r3.8.14 physical NUS only
NDP_LORA_DIAG_SEND       = 0x55  # r3.8.16b1l physical NUS only
NDP_LORA_DIAG_RX         = 0x56  # r3.8.16b1l physical NUS only
NDP_RADIO_GET_EXT         = 0x57  # r3.8.18 extended persisted radio profile
NDP_RADIO_SET_EXT         = 0x58  # r3.8.18 extended persisted radio profile
NDP_NINALINK_LAB          = 0x59  # B4.1 physical NUS lab uplink
NDP_NINALINK_BRIDGE       = 0x5A  # B4.2/B4.3 validated bridge RX
NDP_NINALINK_LINK         = 0x5B  # B4.3 node ACK/downlink gate

NDP_SENSOR_BATTERY       = 1
NDP_SENSOR_HALL          = 2
NDP_SENSOR_ACCEL_XYZ     = 3
NDP_SENSOR_DS18B20       = 4
NDP_SENSOR_ACCEL_METRICS = 5


NINALINK_MAGIC = 0x4E
NINALINK_VERSION = 1
NINALINK_TYPE_NAMES = {
    0x01: "HELLO", 0x02: "CAPS_REQUEST", 0x03: "CAPS_RESPONSE",
    0x10: "CAP_REPORT", 0x11: "CAP_EVENT", 0x20: "ACK", 0x21: "NACK",
}
NINALINK_VALUE_SIZES = {0x01:1,0x02:1,0x03:1,0x04:2,0x05:2,0x06:4,0x07:4,0x08:1}
NINALINK_CAPS = {
    0x0001: ("battery_voltage", -3, "V"),
    0x0002: ("battery_percent", 0, "%"),
    0x0100: ("temperature", -2, "C"),
    0x0101: ("humidity", -2, "%"),
    0x0102: ("illuminance", 0, "lux"),
    0x0103: ("pressure", 0, "Pa"),
    0x0104: ("co2", 0, "ppm"),
    0x0105: ("tvoc", 0, "ppb"),
    0x0106: ("leak", 0, ""),
    0x0200: ("motion", 0, ""),
    0x0201: ("tap", 0, ""),
    0x0202: ("fall", 0, ""),
    0x0203: ("acceleration_x", 0, "mg"),
    0x0204: ("acceleration_y", 0, "mg"),
    0x0205: ("acceleration_z", 0, "mg"),
    0x0206: ("vibration_rms", 0, "mg"),
    0x0207: ("vibration_peak", 0, "mg"),
    0x0208: ("vibration_peak_to_peak", 0, "mg"),
    0x0209: ("vibration_frequency", -3, "Hz"),
    0x020A: ("vibration_alarm", 0, ""),
    0x0300: ("digital_input", 0, ""),
    0x0301: ("hall_state", 0, ""),
    0x0302: ("counter", 0, "count"),
    0x0303: ("quadrature_position", 0, "count"),
    0x0304: ("pulse_frequency", -3, "Hz"),
    0x0400: ("presence", 0, ""),
    0x0401: ("tracking_active", 0, ""),
    0x0503: ("energy", -3, "Wh"),
}

def ninalink_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for octet in data:
        crc ^= octet << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc

def decode_ninalink(data: bytes) -> dict:
    if len(data) < 15: raise ValueError("frame shorter than 15 bytes")
    if len(data) > 64: raise ValueError("frame longer than 64 bytes")
    if data[0] != NINALINK_MAGIC: raise ValueError(f"bad magic 0x{data[0]:02X}")
    if data[1] != NINALINK_VERSION: raise ValueError(f"unsupported version {data[1]}")
    payload_len = data[6]
    if len(data) != 15 + payload_len:
        raise ValueError(f"length mismatch physical={len(data)} payload={payload_len}")
    got_crc = int.from_bytes(data[-2:], "little")
    want_crc = ninalink_crc16(data[:-2])
    if got_crc != want_crc:
        raise ValueError(f"CRC mismatch got=0x{got_crc:04X} expected=0x{want_crc:04X}")
    result = {
        "flags": data[2], "type": data[3],
        "network_id": int.from_bytes(data[4:6], "little"),
        "node_id": int.from_bytes(data[7:11], "little"),
        "sequence": int.from_bytes(data[11:13], "little"),
        "payload": data[13:-2], "entries": [],
    }
    if result["type"] in (0x10, 0x11):
        p = result["payload"]
        if not p: raise ValueError("value message has empty payload")
        count, off, entries = p[0], 1, []
        for _ in range(count):
            if off + 4 > len(p): raise ValueError("truncated value-entry header")
            cap = int.from_bytes(p[off:off+2], "little")
            ch, vt = p[off+2], p[off+3]
            size = NINALINK_VALUE_SIZES.get(vt)
            if size is None: raise ValueError(f"unknown value type 0x{vt:02X}")
            if off + 4 + size > len(p): raise ValueError("truncated value")
            rawb = p[off+4:off+4+size]
            if vt == 0x01:
                if rawb[0] not in (0,1): raise ValueError("invalid BOOL")
                raw = bool(rawb[0])
            elif vt in (0x03,0x05,0x07):
                raw = int.from_bytes(rawb, "little", signed=True)
            else:
                raw = int.from_bytes(rawb, "little")
            entries.append({"capability_id":cap,"channel":ch,"value_type":vt,"raw":raw})
            off += 4 + size
        if off != len(p): raise ValueError("trailing bytes after value entries")
        result["entries"] = entries
    return result

def format_ninalink_value(entry: dict) -> str:
    cap = entry["capability_id"]
    name, scale, unit = NINALINK_CAPS.get(cap, (f"cap_0x{cap:04X}",0,""))
    raw = entry["raw"]
    if isinstance(raw, bool):
        value_text = "true" if raw else "false"
    elif scale == 0:
        value_text = str(raw)
    else:
        value = raw * (10 ** scale)
        value_text = f"{value:.{-scale}f}"
    if unit: value_text += f" {unit}"
    return f"{name}[{entry['channel']}] = {value_text} (raw={raw})"

NDP_ACCESS_PUBLIC     = 0
NDP_ACCESS_CONTROL    = 1
NDP_ACCESS_PROVISION  = 2

ACCEL_MODES = {
    "off": 0,
    "motion": 1,
    "tap": 2,
    "fall": 3,
    "walk": 4,
    "vibration": 5,
}

HALL_MODE_DISABLED      = 0
HALL_MODE_SINGLE        = 1
HALL_MODE_QUADRATURE    = 2

NDP_STATUS_NAMES = {
    0: "OK",
    1: "BAD_LENGTH",
    2: "BAD_ARG",
    3: "UNSUPPORTED",
    4: "BUSY",
    5: "UNAUTHORIZED",
    6: "FORBIDDEN",
    7: "AUTH_FAILED",
}

NDP_BOARD_TYPE_NAMES = {
    1: "NINASENSE",
    2: "PCA10040",
    3: "HALFMOON",
}

NDP_SOFTDEVICE_NAMES = {
    1: "S132",
}

RESETREAS_FLAGS = {
    0: "RESETPIN",
    1: "DOG",
    2: "SREQ",
    3: "LOCKUP",
    16: "OFF",
    17: "LPCOMP",
    18: "DIF",
    19: "NFC",
    20: "VBUS",
}



CMD_HELLO         = 0x01
CMD_TIME_SYNC     = 0x02
CMD_PROGRAM_BEGIN = 0x10
CMD_PROGRAM_DATA  = 0x11
CMD_PROGRAM_END   = 0x12
CMD_PROGRAM_RUN   = 0x13
CMD_PROGRAM_STOP  = 0x14
CMD_STATUS        = 0x15
CMD_PING          = 0x16
CMD_PROGRAM_AUTH = 0x17
CMD_PROGRAM_SCHEDULE = 0x18
CMD_RESET = 0x19
CMD_EVENT_INJECT = 0x1A
CMD_CAPABILITIES = 0x1B
CMD_FACTORY_RESET = 0x1C
CMD_TRACKING_IDENTITY = 0x1D
CMD_TRACKING_INFO     = 0x1E
CMD_TRACKING_DEBUG    = 0x1F
CMD_DFU_ENTER         = 0x20

DFU_CMD_INFO=0x01
DFU_CMD_BEGIN=0x02
DFU_CMD_DATA=0x03
DFU_CMD_END=0x04
DFU_CMD_ABORT=0x05
DFU_CMD_BOOT=0x06
DFU_APP_START=0x00026000
DFU_APP_END=0x0006C000
DFU_MAX_IMAGE=DFU_APP_END-DFU_APP_START


STATUS_NAMES = {
    0x00: "OK",
    0x01: "BAD_LENGTH",
    0x02: "BAD_STATE",
    0x03: "BAD_CRC",
    0x04: "INVALID_PROGRAM",
    0x05: "RANGE/OFFSET",
    0x06: "BUSY",
    0x07: "NOT_LOADED",
    0x08: "INTERNAL",
    0x09: "AUTH_REQUIRED",
    0x0A: "AUTH_FAILED",
}


VM_STATES = {
    0: "STOPPED",
    1: "READY",
    2: "WAIT_RTC",
    3: "WAIT_HALL",
    4: "WAIT_BATTERY",
    5: "WAIT_LORA",
    6: "WAIT_EVENT",
    7: "DONE",
    8: "ERROR",
    9: "WAIT_SERIAL_TX",
    10: "WAIT_DS18B20",
    11: "WAIT_LORA_RX",
    12: "WAIT_SERIAL_RX",
}


FLASH_STATES = {
    0: "EMPTY",
    1: "READY",
    2: "SAVING",
    3: "ERROR",
}


CAP_NAMES = {1:"GPIO",2:"BATTERY",3:"DS18B20",4:"ACCEL",5:"LORA",6:"BLE_APP",7:"RTC",8:"HALL",9:"STATE",10:"SERIAL",11:"TRACKING",12:"VIB_HEALTH",13:"VIB_AUTO"}

VM_EVENT_FRAME   = 0xE0
VM_NOTIFY_U32    = 0x01
VM_NOTIFY_STRING = 0x02
VM_FLAG_START    = 0x01
VM_FLAG_END      = 0x02


# Bytecode v1
OP_END           = 0x00
OP_MOVI          = 0x08
OP_JMP           = 0x0B
OP_WAIT_EVENT    = 0x12
OP_WAIT_S        = 0x11
OP_BAT_READ      = 0x30
OP_DEBUG_BAT     = 0x31
OP_NOTIFY_U32    = 0x40
OP_NOTIFY_STR    = 0x41
OP_LORA_SEND_BAT = 0x60
OP_WAIT_EVENT = 0x12
OP_GPIO_WRITE_IMM = 0x50


OP_TRACKING_SET_KEY = 0x77
OP_TRACKING_CONFIG  = 0x78
OP_TRACKING_START   = 0x79
OP_TRACKING_STOP    = 0x7A
OP_TRACKING_STATUS  = 0x7B
OP_TRACKING_ROTATION_SET = 0x7C

def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF

    for b in data:
        crc ^= b << 8

        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF

    return crc


DEFAULT_STAGE5_KEY_HEX = "6e5246436c61772d5354414745352d4445562d4b45592d303030303030303100"


def stage5_key(key_hex: str | None = None) -> bytes:
    raw = (
        key_hex
        or os.environ.get("NRFCLAW_KEY")
        or DEFAULT_STAGE5_KEY_HEX
    )

    try:
        key = bytes.fromhex(raw)
    except ValueError as exc:
        raise ValueError("Stage-5 key must be hexadecimal") from exc

    if len(key) != 32:
        raise ValueError("Stage-5 HMAC key must be exactly 32 bytes")

    return key



DEFAULT_CONTROL_KEY = b"nRFClaw-CONTROL-DEV-KEY-00000001"


def control_key(raw: str | None = None) -> bytes:
    """
    Pack 02 development CONTROL key.

    Priority:
      --control-key
      NRFCLAW_CONTROL_KEY
      built-in DEVELOPMENT key

    A value prefixed with "hex:" is decoded as hexadecimal. Otherwise the
    value is interpreted as UTF-8. Production deployments must provision a
    device/fleet-specific secret instead of using the development default.
    """
    value = raw or os.environ.get("NRFCLAW_CONTROL_KEY")

    if value is None:
        return DEFAULT_CONTROL_KEY

    if value.startswith("hex:"):
        try:
            key = bytes.fromhex(value[4:])
        except ValueError as exc:
            raise ValueError("invalid hexadecimal CONTROL key") from exc
    else:
        key = value.encode("utf-8")

    if not key:
        raise ValueError("CONTROL key cannot be empty")

    return key


def ndp_access_key(raw: str | None = None) -> bytes | None:
    """Optional Application/NDP access key.

    If the device has never been provisioned with ``ndp-key-generate``, the
    Application/NDP plane remains open and no key is needed. Once a key exists
    in device flash, provide it via --ndp-key or NRFCLAW_NDP_KEY.
    """
    value = raw or os.environ.get("NRFCLAW_NDP_KEY")
    if value is None:
        return None
    if value.startswith("hex:"):
        try:
            key = bytes.fromhex(value[4:])
        except ValueError as exc:
            raise ValueError("invalid hexadecimal NDP key") from exc
    else:
        key = value.encode("utf-8")
    if len(key) != 32:
        raise ValueError("NDP key must be exactly 32 bytes (256 bits)")
    return key


def encode_schedule(schedule: dict) -> bytes:
    return struct.pack(
        "<BBHII",
        schedule["mode"],
        schedule.get("dow_mask", 0),
        0,
        schedule.get("arg0", 0),
        schedule.get("arg1", 0),
    )


def bytecode_hmac(code: bytes, schedule: dict, key: bytes) -> bytes:
    domain = b"nRFClaw-bytecode-v6\x00"
    msg = (
        domain
        + struct.pack("<H", len(code))
        + encode_schedule(schedule)
        + code
    )
    return hmac.new(key, msg, hashlib.sha256).digest()



SCHED_MANUAL = 0
SCHED_AT = 1
SCHED_EVERY = 2
SCHED_WEEKLY = 3
SCHED_BOOT = 4

SCHED_STATES = {
    0: "DISABLED",
    1: "WAIT_TIME",
    2: "ARMED",
    3: "RUNNING",
    4: "EXPIRED",
    5: "ERROR",
    6: "SUSPENDED",
}

DOW = {
    "mon": 0, "monday": 0,
    "tue": 1, "tuesday": 1,
    "wed": 2, "wednesday": 2,
    "thu": 3, "thursday": 3,
    "fri": 4, "friday": 4,
    "sat": 5, "saturday": 5,
    "sun": 6, "sunday": 6,
}


def schedule_manual():
    return {"mode": SCHED_MANUAL, "dow_mask": 0, "arg0": 0, "arg1": 0}


def schedule_boot():
    return {"mode": SCHED_BOOT, "dow_mask": 0, "arg0": 0, "arg1": 0}


def schedule_at(epoch: int):
    return {"mode": SCHED_AT, "dow_mask": 0, "arg0": epoch, "arg1": 0}


def schedule_every(seconds: int, anchor: int | None = None):
    if seconds <= 0:
        raise ValueError("interval must be > 0")
    if anchor is None:
        anchor = int(time.time())
    return {"mode": SCHED_EVERY, "dow_mask": 0, "arg0": seconds, "arg1": anchor}


def schedule_weekly(days: str, hhmmss: str):
    mask = 0
    for token in days.split(","):
        key = token.strip().lower()
        if key not in DOW:
            raise ValueError(f"unknown weekday: {token}")
        mask |= 1 << DOW[key]

    parts = [int(x) for x in hhmmss.split(":")]
    if len(parts) == 2:
        parts.append(0)
    if len(parts) != 3:
        raise ValueError("time must be HH:MM or HH:MM:SS")

    hh, mm, ss = parts
    if not (0 <= hh <= 23 and 0 <= mm <= 59 and 0 <= ss <= 59):
        raise ValueError("invalid UTC time")

    return {"mode": SCHED_WEEKLY, "dow_mask": mask,
            "arg0": hh * 3600 + mm * 60 + ss, "arg1": 0}


# Stage 8 VM persistent-state opcodes.
OP_STATE_SET      = 0x42
OP_STATE_GET      = 0x43
OP_PERSIST_SAVE   = 0x44
OP_PERSIST_LOAD   = 0x45

# Stage 8 native/app opcodes used by test helpers.
OP_APP_GET_U32    = 0x56
OP_BLE_APP_ROLE   = 0x70
OP_BLE_ADV_CONFIG = 0x71
OP_BLE_ADV_START  = 0x72
OP_BLE_ADV_NAME   = 0x76
OP_APP_SEND_U32   = 0x57
OP_SERIAL_CONFIG  = 0x58
OP_SERIAL_DISABLE = 0x59
OP_SERIAL_WRITE   = 0x5A
OP_SERIAL_READ    = 0x5B
OP_SERIAL_RX_BUF  = 0x85
OP_APP_SEND_BUF   = 0x86

BLE_APP_ROLE_PERIPHERAL = 2

EVENT_APP_COMMAND = 18


def state_save_test(key: int, value: int) -> bytes:
    if not 0 <= key <= 15:
        raise ValueError("state key must be 0..15")

    value &= 0xFFFFFFFF

    # MOVI R0,value
    # STATE_SET key,R0
    # PERSIST_SAVE key,R0
    # END
    return bytes([
        OP_MOVI, 0x00,
        value & 0xFF,
        (value >> 8) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 24) & 0xFF,
        OP_STATE_SET, key, 0x00,
        OP_PERSIST_SAVE, key, 0x00,
        OP_END,
    ])


def state_load_notify_test(key: int, channel: int = 3) -> bytes:
    if not 0 <= key <= 15:
        raise ValueError("state key must be 0..15")

    # PERSIST_LOAD key,R0
    # NOTIFY_U32 channel,R0
    # END
    return bytes([
        OP_PERSIST_LOAD, key, 0x00,
        OP_NOTIFY_U32, channel, 0x00,
        OP_END,
    ])


def state_get_notify_test(key: int, channel: int = 4) -> bytes:
    if not 0 <= key <= 15:
        raise ValueError("state key must be 0..15")

    # STATE_GET key,R0
    # NOTIFY_U32 channel,R0
    # END
    return bytes([
        OP_STATE_GET, key, 0x00,
        OP_NOTIFY_U32, channel, 0x00,
        OP_END,
    ])


def app_echo_boot_test(interval_ms: int = 1000, payload: str = "nRFClaw8") -> bytes:
    if not 100 <= interval_ms <= 10000:
        raise ValueError("interval must be 100..10000 ms")

    raw = payload.encode("utf-8")

    if len(raw) > 16:
        raise ValueError("payload must be <=16 UTF-8 bytes")

    code = bytearray()

    code += bytes([
        OP_BLE_APP_ROLE,
        BLE_APP_ROLE_PERIPHERAL,
    ])

    code += bytes([
        OP_BLE_ADV_CONFIG,
        interval_ms & 0xFF,
        (interval_ms >> 8) & 0xFF,
        0x00,
        len(raw),
    ])
    code += raw

    code += bytes([
        OP_BLE_ADV_START,
    ])

    loop_pc = len(code)

    code += bytes([
        OP_WAIT_EVENT,
        EVENT_APP_COMMAND,
        0x00,
        0x01,
    ])

    code += bytes([
        OP_APP_GET_U32,
        0x00,
        0x02,
    ])

    code += bytes([
        OP_APP_SEND_U32,
        0x05,
        0x09,
        0x01,
        0x01,
        0x02,
    ])

    jmp_opcode_pc = len(code)
    pc_after_jmp = jmp_opcode_pc + 3
    rel = loop_pc - pc_after_jmp

    if not -32768 <= rel <= 32767:
        raise ValueError("internal echo loop jump out of range")

    code += bytes([
        OP_JMP,
        rel & 0xFF,
        (rel >> 8) & 0xFF,
    ])

    # The VM validator requires a physical END as the final byte of every
    # program, even when control flow loops forever and END is unreachable.
    code += bytes([
        OP_END,
    ])

    return bytes(code)


def beacon_boot_program(name: str = "Teste-Claw", interval_ms: int = 2000, tx_power_dbm: int = 4, wait_s: int = 5) -> bytes:
    raw = name.encode("utf-8")
    if not raw or len(raw) > 24:
        raise ValueError("beacon name must be 1..24 UTF-8 bytes")
    if not 20 <= interval_ms <= 10240:
        raise ValueError("interval must be 20..10240 ms")
    if not 0 <= wait_s <= 0xFFFFFFFF:
        raise ValueError("wait must fit uint32 seconds")
    return bytes([
        OP_WAIT_S, wait_s & 0xff, (wait_s >> 8) & 0xff, (wait_s >> 16) & 0xff, (wait_s >> 24) & 0xff,
        OP_BLE_APP_ROLE, 1,
        OP_BLE_ADV_CONFIG, interval_ms & 0xff, (interval_ms >> 8) & 0xff, tx_power_dbm & 0xff, 0,
        OP_BLE_ADV_NAME, len(raw),
    ]) + raw + bytes([OP_BLE_ADV_START, OP_END])


def serial_write_test(tx_pin: int, rx_pin: int, baud: int, text: str) -> bytes:
    raw = text.encode("utf-8")
    if not raw or len(raw) > 64:
        raise ValueError("serial text must be 1..64 UTF-8 bytes")
    if not (0 <= tx_pin <= 31 and 0 <= rx_pin <= 31 and tx_pin != rx_pin):
        raise ValueError("invalid serial TX/RX pins")
    code = bytearray([
        OP_SERIAL_CONFIG, tx_pin, rx_pin,
        baud & 0xFF, (baud >> 8) & 0xFF,
        (baud >> 16) & 0xFF, (baud >> 24) & 0xFF,
        OP_SERIAL_WRITE, len(raw),
    ])
    code += raw
    code += bytes([OP_SERIAL_DISABLE, OP_END])
    return bytes(code)


def tracking_test_program(
    key_hex: str,
    interval_ms: int = 1000,
    tx_power_dbm: int = 0,
) -> bytes:
    """
    Stage 8.2-A experimental tracking test bytecode.

    Encoding:
      TRACKING_SET_KEY <28 bytes>
      TRACKING_CONFIG interval:u16 tx_power:i8
      TRACKING_START
      END
    """
    key_hex = key_hex.strip().replace(":", "").replace(" ", "")

    try:
        key = bytes.fromhex(key_hex)
    except ValueError as exc:
        raise ValueError("tracking key must be hexadecimal") from exc

    if len(key) != 28:
        raise ValueError(
            "tracking key must be exactly 28 bytes / 56 hex characters"
        )

    if not 100 <= interval_ms <= 10000:
        raise ValueError(
            "tracking interval must be 100..10000 ms"
        )

    if not -40 <= tx_power_dbm <= 8:
        raise ValueError(
            "tracking TX power must be -40..8 dBm"
        )

    code = bytearray()

    code += bytes([
        OP_TRACKING_SET_KEY,
    ])
    code += key

    code += bytes([
        OP_TRACKING_CONFIG,
        interval_ms & 0xFF,
        (interval_ms >> 8) & 0xFF,
        tx_power_dbm & 0xFF,
    ])

    code += bytes([
        OP_TRACKING_START,
        OP_END,
    ])

    return bytes(code)


def tracking_stop_program() -> bytes:
    return bytes([
        OP_TRACKING_STOP,
        OP_END,
    ])


def tracking_enable_program(
    interval_ms: int = 1000,
    tx_power_dbm: int = 4,
    rotation_seconds: int = 10800,
) -> bytes:
    """
    Stage 8.2 autonomous tracking.

    No external OpenHaystack public-key file is required.

    Bytecode:
      TRACKING_CONFIG interval:u16 tx_power:i8
      TRACKING_ROTATION_SET seconds:u32
      TRACKING_START
      END
    """
    if not 100 <= interval_ms <= 10000:
        raise ValueError(
            "tracking interval must be 100..10000 ms"
        )

    if rotation_seconds <= 0 or rotation_seconds > 0xFFFFFFFF:
        raise ValueError(
            "tracking rotation must be 1..4294967295 seconds"
        )

    # nRF52832/S132 accepts discrete RF values. We allow the signed byte
    # through here; the firmware/SoftDevice remains the final validator.
    if not -128 <= tx_power_dbm <= 127:
        raise ValueError(
            "tracking TX power must fit signed int8"
        )

    return bytes([
        OP_TRACKING_CONFIG,
        interval_ms & 0xFF,
        (interval_ms >> 8) & 0xFF,
        tx_power_dbm & 0xFF,

        OP_TRACKING_ROTATION_SET,
        rotation_seconds & 0xFF,
        (rotation_seconds >> 8) & 0xFF,
        (rotation_seconds >> 16) & 0xFF,
        (rotation_seconds >> 24) & 0xFF,

        OP_TRACKING_START,
        OP_END,
    ])


def battery_notify_test() -> bytes:
    # BAT_READ R0
    # NOTIFY_U32 channel=1 R0
    # END
    return bytes([
        OP_BAT_READ, 0x00,
        OP_NOTIFY_U32, 0x01, 0x00,
        OP_END,
    ])


def string_notify_test(text: str, channel: int = 2) -> bytes:
    raw = text.encode("utf-8")

    if not raw or len(raw) > 64:
        raise ValueError("NOTIFY_STR supports 1..64 UTF-8 bytes")

    if not 0 <= channel <= 255:
        raise ValueError("channel must be 0..255")

    return (
        bytes([OP_NOTIFY_STR, channel, len(raw)])
        + raw
        + bytes([OP_END])
    )


def battery_lora_test(wait_s: int = 5) -> bytes:
    return bytes([
        OP_WAIT_S,
        wait_s & 0xFF,
        (wait_s >> 8) & 0xFF,
        (wait_s >> 16) & 0xFF,
        (wait_s >> 24) & 0xFF,

        OP_BAT_READ, 0x00,
        OP_DEBUG_BAT, 0x00,
        OP_LORA_SEND_BAT, 0x00,
        OP_END,
    ])


def event_gpio_test(event_type: int, pin: int, value: int) -> bytes:
    if not 0 <= event_type <= 255:
        raise ValueError("event type must be 0..255")
    if not 0 <= pin <= 31:
        raise ValueError("pin must be 0..31")
    return bytes([
        OP_WAIT_EVENT, event_type, 0x00, 0x01,
        OP_GPIO_WRITE_IMM, pin, 1 if value else 0,
        OP_END,
    ])


async def scan_bleak_devices(timeout: float):
    """Return [(device, adv)] from an unfiltered Bleak scan, supporting
    both newer and older Bleak releases used by the nRFClaw CLI.
    """
    try:
        found = await BleakScanner.discover(timeout=timeout, return_adv=True)
        rows = []
        for _key, item in found.items():
            if isinstance(item, tuple) and len(item) == 2:
                rows.append(item)
            else:
                rows.append((item, None))
        return rows
    except TypeError:
        devices = await BleakScanner.discover(timeout=timeout)
        return [(dev, None) for dev in devices]


async def print_bleak_scan(timeout: float, suffix: str = ""):
    rows = await scan_bleak_devices(timeout)
    suffix_norm = suffix.replace(":", "").replace("-", "").upper()
    print(f"=== BLEAK SCAN ({timeout:g}s) ===")
    print(f"Devices seen: {len(rows)}")
    for dev, adv in rows:
        address = (getattr(dev, "address", "") or "")
        adv_name = getattr(adv, "local_name", None) if adv is not None else None
        name = (adv_name or getattr(dev, "name", None) or "").strip()
        uuids = list((getattr(adv, "service_uuids", None) or []) if adv is not None else [])
        rssi = getattr(adv, "rssi", None) if adv is not None else getattr(dev, "rssi", None)
        compact = address.replace(":", "").replace("-", "").upper()
        mark = " <== TARGET" if suffix_norm and compact.endswith(suffix_norm) else ""
        print(f"{address or '-':17}  {name or '-':24} RSSI={rssi if rssi is not None else '-'}{mark}")
        if uuids:
            print("  UUIDs: " + ", ".join(uuids))
    if not rows:
        print("(Bleak received no advertisements.)")
    return rows


async def find_app_device(suffix: str, timeout: float, verbose: bool = False):
    """Find the normal Application/HA plane without relying on Bleak's
    find_device_by_filter callback path.

    Some BlueZ/Bleak combinations can expose a peripheral to bluetoothctl
    while find_device_by_filter() never receives a matching callback.  For
    the HA reference path we therefore perform an unfiltered discovery,
    inspect the complete result set, and select by board name or address
    suffix.  GATT discovery after connect remains the authoritative check
    that the selected peripheral is the Application plane.
    """
    suffix = suffix.replace(":", "").replace("-", "").upper()
    wanted = f"nRFClaw(NDP)-{suffix}"

    rows = await scan_bleak_devices(timeout)

    candidates = []
    visible = []
    for dev, adv in rows:

        address = (getattr(dev, "address", "") or "")
        address_compact = address.replace(":", "").replace("-", "").upper()
        adv_name = getattr(adv, "local_name", None) if adv is not None else None
        name = (adv_name or getattr(dev, "name", None) or "").strip()
        uuids = {
            u.lower()
            for u in ((getattr(adv, "service_uuids", None) or []) if adv is not None else [])
        }

        if name or address:
            visible.append((address, name, uuids))

        identity_match = (
            name.upper() == wanted.upper()
            or address_compact.endswith(suffix)
        )
        if not identity_match:
            continue

        # If BlueZ explicitly says this is NUS-only, do not steal the
        # programming plane.  Missing UUIDs are accepted and proven by GATT.
        if NUS_SERVICE.lower() in uuids and APP_SERVICE.lower() not in uuids:
            continue

        score = 0
        if address_compact.endswith(suffix):
            score += 4
        if name.upper() == wanted.upper():
            score += 4
        if APP_SERVICE.lower() in uuids:
            score += 8
        candidates.append((score, dev, adv))

    if candidates:
        candidates.sort(key=lambda x: x[0], reverse=True)
        return wanted, candidates[0][1]

    if verbose:
        print("Bleak scan saw these devices:")
        if not visible:
            print("  (none)")
        else:
            for address, name, uuids in visible:
                uuids_txt = ",".join(sorted(uuids)) if uuids else "-"
                print(f"  {address or '-':17} {name or '-':24} UUIDs={uuids_txt}")

    raise RuntimeError(
        f"{wanted} Application BLE not found in Bleak discovery. "
        "The OS may still show a cached device in bluetoothctl. "
        "Run 'ha scan' to compare what Bleak itself can see."
    )


class ApplicationNDPClient:
    """Reference NDP-SESSION client over the Pack-02 Application GATT plane.

    The Application/HA plane is open by default. If the owner provisions a
    device key from physical-button NUS, this client authenticates with that
    key before normal NDP operations. NUS itself remains keyless.
    """

    def __init__(self, dev, access_key: bytes | None = None):
        self.dev = dev
        self.access_key = access_key
        self.client = BleakClient(dev)
        self.response_queue = asyncio.Queue()
        self._seq = 0
        self._notify_started = False

    async def __aenter__(self):
        # IMPORTANT: if __aenter__ raises, Python does NOT call __aexit__.
        # Therefore every partial Application-BLE connection must be cleaned
        # up here, otherwise BlueZ can keep the nRFClaw connected and the
        # peripheral will no longer advertise for the next CLI attempt.
        stage = "connect"
        try:
            await self.client.connect()
            if not self.client.is_connected:
                raise RuntimeError("Bleak returned from connect() but is_connected is false")

            stage = "service discovery"
            # Accessing services forces/validates GATT discovery on supported
            # Bleak backends.  Do not use get_services(), which was removed in
            # newer Bleak versions.
            services = self.client.services
            service = services.get_service(APP_SERVICE)
            if service is None:
                raise RuntimeError(f"Application service {APP_SERVICE} not present after connect")
            rx = services.get_characteristic(APP_RX)
            tx = services.get_characteristic(APP_TX)
            if rx is None:
                raise RuntimeError(f"Application RX characteristic {APP_RX} not present")
            if tx is None:
                raise RuntimeError(f"Application TX characteristic {APP_TX} not present")

            stage = "enable TX notifications"
            await self.client.start_notify(APP_TX, self._notify)
            self._notify_started = True

            stage = "NDP access negotiation"
            await self.ensure_application_access()
            return self

        except BaseException as exc:
            # Best-effort cleanup is mandatory here because __aexit__ will not
            # run when __aenter__ fails.
            try:
                if self.client.is_connected:
                    await self.client.disconnect()
            except BaseException:
                pass
            await asyncio.sleep(0.20)
            detail = str(exc) or repr(exc)
            raise RuntimeError(
                f"Application BLE failed during {stage}: {detail}"
            ) from exc

    async def __aexit__(self, exc_type, exc, tb):
        if self._notify_started:
            try:
                await self.client.stop_notify(APP_TX)
            except BaseException:
                pass
            self._notify_started = False
        try:
            if self.client.is_connected:
                await self.client.disconnect()
        finally:
            # Give BlueZ/SoftDevice a short interval to process DISCONNECTED
            # and let the firmware restart Application advertising.
            await asyncio.sleep(0.20)

    def _notify(self, _sender, data: bytearray):
        self.response_queue.put_nowait(bytes(data))

    async def ndp_command_raw(self, opcode: int, payload: bytes = b"", timeout: float = 5.0):
        if len(payload) > 16:
            raise ValueError(
                "Application NDP payload exceeds current ATT MTU=23 / 20-byte frame limit"
            )
        self._seq = (self._seq + 1) & 0xFF
        seq = self._seq
        frame = bytes([0x00, opcode & 0xFF, seq, len(payload)]) + payload
        if len(frame) > 20:
            raise ValueError(f"Application NDP frame too large: {len(frame)} bytes")

        await self.client.write_gatt_char(APP_RX, frame, response=False)
        end = asyncio.get_running_loop().time() + timeout
        while True:
            remain = end - asyncio.get_running_loop().time()
            if remain <= 0:
                raise TimeoutError(
                    f"Timeout waiting Application NDP response opcode=0x{opcode:02X} seq={seq}"
                )
            try:
                data = await asyncio.wait_for(self.response_queue.get(), remain)
            except asyncio.TimeoutError as exc:
                raise TimeoutError(
                    f"Timeout waiting Application NDP response opcode=0x{opcode:02X} seq={seq}"
                ) from exc
            if len(data) < 5:
                continue
            _flags, op, rx_seq, plen = data[0], data[1], data[2], data[3]
            if op != opcode or rx_seq != seq:
                continue
            if len(data) != 4 + plen:
                raise RuntimeError(
                    f"Invalid Application NDP frame length: header={plen} actual={len(data)-4}"
                )
            body = data[4:]
            if not body:
                raise RuntimeError("Application NDP response missing status")
            return body[0], body[1:]

    async def ndp_command(self, opcode: int, payload: bytes = b"", timeout: float = 5.0):
        status, body = await self.ndp_command_raw(opcode, payload, timeout)
        if status != 0:
            raise RuntimeError(
                f"Application NDP 0x{opcode:02X}: "
                f"{NDP_STATUS_NAMES.get(status, hex(status))}"
            )
        return body

    async def ndp_access_status(self) -> int:
        p = await self.ndp_command(NDP_ACCESS_STATUS)
        if len(p) != 1:
            raise RuntimeError(f"Invalid Application ACCESS_STATUS length: {len(p)}")
        return p[0]

    async def ensure_application_access(self):
        # Probe one normal opcode. Open/unprovisioned devices answer directly;
        # protected devices return UNAUTHORIZED and require HMAC authentication.
        status, _ = await self.ndp_command_raw(NDP_INFO)
        if status == 0:
            return
        if status != 5:  # NDP_UNAUTHORIZED
            raise RuntimeError(
                f"Application NDP access probe failed: "
                f"{NDP_STATUS_NAMES.get(status, hex(status))}"
            )
        if self.access_key is None:
            raise RuntimeError(
                "This nRFClaw has NDP protection enabled. Supply the key with "
                "--ndp-key hex:<64-hex-digits> or NRFCLAW_NDP_KEY."
            )
        await self.ndp_auth_control(self.access_key)

    async def ndp_auth_control(self, key: bytes):
        level = await self.ndp_access_status()
        if level >= NDP_ACCESS_CONTROL:
            return
        p = await self.ndp_command(NDP_AUTH_BEGIN, bytes([NDP_ACCESS_CONTROL]))
        if len(p) != 13:
            raise RuntimeError(f"Invalid Application AUTH_BEGIN length: {len(p)}")
        challenge, session_id = p[:12], p[12]
        tag16 = hmac.new(
            key,
            challenge + bytes([NDP_ACCESS_CONTROL, session_id]),
            hashlib.sha256,
        ).digest()[:16]
        p = await self.ndp_command(NDP_AUTH_FINISH, tag16)
        if len(p) != 1 or p[0] < NDP_ACCESS_CONTROL:
            raise RuntimeError("Application NDP CONTROL authentication was not granted")

    async def info(self, quiet=False):
        p = await self.ndp_command(NDP_INFO)
        if len(p) < 3:
            raise RuntimeError("Short Application NDP INFO response")
        d = {"ndp_version": p[0], "vm_abi": p[1], "board_type": p[2]}
        if not quiet:
            print(f"NDP version:     {d['ndp_version']}")
            print(f"VM ABI:          {d['vm_abi']}")
            print(f"Board:           {NDP_BOARD_TYPE_NAMES.get(d['board_type'], d['board_type'])}")
        return d

    async def capabilities(self, quiet=False):
        p = await self.ndp_command(NDP_CAPS)
        if len(p) < 2:
            raise RuntimeError("Short Application NDP CAPS response")
        mask = int.from_bytes(p[:2], "little")
        d = {
            "mask": mask,
            "supported": {cap: bool(mask & (1 << (cap - 1))) for cap in CAP_NAMES},
        }
        # Compatibility for older callers of the R3.4 CLI.
        d["available"] = d["supported"]
        if not quiet:
            print(f"Capabilities:    0x{mask:04X}")
            for cap, name in CAP_NAMES.items():
                print(f"  {name:14s} {'YES' if d['supported'][cap] else 'no'}")
        return d

    async def board_reset(self, quiet=False):
        p = await self.ndp_command(NDP_BOARD_RESET)
        if len(p) < 4:
            raise RuntimeError(f"Short Application BOARD_RESET response: {len(p)} bytes")
        resetreas = int.from_bytes(p[:4], "little")
        d = {"resetreas": resetreas, "reasons": decode_resetreas(resetreas)}
        if not quiet:
            print(f"RESETREAS:       0x{resetreas:08X}")
            print(f"Reset reason:    {', '.join(d['reasons'])}")
        return d

    async def board_info(self, quiet=False):
        core = decode_board_info_core(await self.ndp_command(NDP_BOARD_INFO, b"\x00"))
        devid = await self.ndp_command(NDP_BOARD_INFO, b"\x01")
        git = await self.ndp_command(NDP_BOARD_INFO, b"\x02")
        if len(devid) != 8 or len(git) != 4:
            raise RuntimeError("Invalid Application BOARD_INFO response")
        d = dict(core)
        d["deviceid0"] = int.from_bytes(devid[0:4], "little")
        d["deviceid1"] = int.from_bytes(devid[4:8], "little")
        d["git32"] = int.from_bytes(git, "little")
        if not quiet:
            board_name = NDP_BOARD_TYPE_NAMES.get(d["board_type"], d["board_type"])
            sd_name = NDP_SOFTDEVICE_NAMES.get(d["softdevice_type"], d["softdevice_type"])
            pre = f"-pre{d['prerelease']}" if d["prerelease"] else ""
            print(f"Board:           {board_name} Rev {d['hw_major']}.{d['hw_minor']}")
            print(f"Firmware:        nRFClaw {d['fw_major']}.{d['fw_minor']}.{d['fw_patch']}{pre} build {d['build']}")
            print(f"Git32:           {d['git32']:08X}")
            print(f"NDP / VM ABI:    {d['ndp_version']} / {d['vm_abi']}")
            print(f"SoftDevice:      {sd_name} {d['sd_major']}.{d['sd_minor']}.{d['sd_patch']}")
            print(f"Device ID:       {d['deviceid1']:08X}{d['deviceid0']:08X}")
        return d

    async def status(self, quiet=False):
        p = await self.ndp_command(NDP_STATUS)
        if len(p) < 2:
            raise RuntimeError("Short Application NDP STATUS response")
        d = {"hall_mode": p[0], "ds18b20_present": bool(p[1])}
        if not quiet:
            hall = {0:"disabled",1:"single D11",2:"quadrature D11+D14"}.get(d["hall_mode"], str(d["hall_mode"]))
            print(f"Hall mode:       {hall}")
            print(f"DS18B20 present: {'yes' if d['ds18b20_present'] else 'no'}")
        return d

    async def active_capabilities(self, quiet=False):
        p = await self.ndp_command(NDP_STATUS, b"\x01")
        if len(p) < 2:
            raise RuntimeError("Short Application NDP STATUS(active) response")
        mask = int.from_bytes(p[:2], "little")
        d = {
            "mask": mask,
            "active": {cap: bool(mask & (1 << (cap - 1))) for cap in CAP_NAMES},
        }
        if not quiet:
            print(f"Active mask:     0x{mask:04X}")
            for cap, name in CAP_NAMES.items():
                print(f"  {name:14s} {'YES' if d['active'][cap] else 'no'}")
        return d

    async def sensor_read_raw(self, sensor_id: int, retries=1, delay=0.2):
        last = (None, b"")
        for attempt in range(retries):
            last = await self.ndp_command_raw(NDP_SENSOR_READ, bytes([sensor_id & 0xFF]))
            if last[0] != 4:
                return last
            if attempt + 1 < retries:
                await asyncio.sleep(delay)
        return last

    async def battery(self, quiet=False):
        status, p = await self.sensor_read_raw(NDP_SENSOR_BATTERY, retries=3, delay=0.15)
        if status != 0:
            raise RuntimeError(f"Battery: {NDP_STATUS_NAMES.get(status, status)}")
        if len(p) < 2:
            raise RuntimeError("Short battery response")
        cv = int.from_bytes(p[:2], "little")
        if not quiet:
            print(f"Battery:         {cv / 100.0:.2f} V ({cv} cV)")
        return cv

    async def device_info(self, ble_name=None):
        """Print one consolidated NDP-v1 device/conformance snapshot."""
        info = await self.info(quiet=True)
        board = await self.board_info(quiet=True)
        caps = await self.capabilities(quiet=True)
        reset = await self.board_reset(quiet=True)
        status = await self.status(quiet=True)
        active = await self.active_capabilities(quiet=True)

        board_name = NDP_BOARD_TYPE_NAMES.get(board["board_type"], f"BOARD-{board['board_type']}")
        sd_name = NDP_SOFTDEVICE_NAMES.get(board["softdevice_type"], str(board["softdevice_type"]))
        pre = f"-pre{board['prerelease']}" if board["prerelease"] else ""
        device_id = f"{board['deviceid1']:08X}{board['deviceid0']:08X}"

        print("=== nRFClaw NDP DEVICE ===")
        print()
        print(f"Device:          {ble_name or ('nRFClaw(NDP)-' + device_id[-6:])}")
        print(f"Device ID:       {device_id}")
        print()
        print(f"Board:           {board_name}")
        print(f"Hardware:        Rev {board['hw_major']}.{board['hw_minor']}")
        print(
            f"Firmware:        nRFClaw {board['fw_major']}.{board['fw_minor']}."
            f"{board['fw_patch']}{pre} build {board['build']}"
        )
        print(f"Git32:           {board['git32']:08X}")
        print(f"SoftDevice:      {sd_name} {board['sd_major']}.{board['sd_minor']}.{board['sd_patch']}")
        print()
        print(f"NDP version:     {info['ndp_version']}")
        print(f"VM ABI:          {info['vm_abi']}")
        print(f"Capabilities:    0x{caps['mask']:04X}")
        for cap, name in CAP_NAMES.items():
            print(f"  {name:14s} {'YES' if caps['supported'][cap] else 'no'}")

        print()
        print("Runtime:")
        hall = {0: "disabled", 1: "single", 2: "quadrature"}.get(status["hall_mode"], str(status["hall_mode"]))
        print(f"  Hall mode:      {hall}")
        print(f"  DS18B20:        {'present' if status['ds18b20_present'] else 'not present'}")
        print(f"  Hall:           {'active' if active['active'].get(8, False) else 'disabled'}")
        print(f"  Serial:         {'active' if active['active'].get(10, False) else 'disabled'}")
        print(f"  Tracking:       {'active' if active['active'].get(11, False) else 'stopped'}")
        print(f"  Vib Auto:       {'active' if active['active'].get(13, False) else 'disabled'}")
        print(f"  RESETREAS:      0x{reset['resetreas']:08X}")
        print(f"  Reset reason:   {', '.join(reset['reasons'])}")

        # Read sensor values only when the capability is currently available.
        if caps['supported'].get(2, False):
            try:
                cv = await self.battery(quiet=True)
                print(f"  Battery:        {cv / 100.0:.2f} V")
            except Exception as exc:
                print(f"  Battery:        unavailable ({exc})")

        if caps['supported'].get(3, False):
            try:
                st, p = await self.sensor_read_raw(NDP_SENSOR_DS18B20, retries=3, delay=0.8)
                if st == 0 and len(p) == 4:
                    mc = struct.unpack("<i", p)[0]
                    print(f"  Temperature:    {mc / 1000.0:.3f} C")
                else:
                    print(f"  Temperature:    unavailable ({NDP_STATUS_NAMES.get(st, st)})")
            except Exception as exc:
                print(f"  Temperature:    unavailable ({exc})")

        print()
        print("BLE:")
        print("  Interface:      NDP / Application GATT")
        print("  TX power:       not exposed by NDP v1")

        return {
            "info": info,
            "board": board,
            "capabilities": caps,
            "reset": reset,
            "status": status,
            "active_capabilities": active,
        }

    async def sensors(self, quiet=False):
        st = await self.status(quiet=True)
        result = {"hall_mode": st["hall_mode"], "ds18b20_present": st["ds18b20_present"]}
        result["battery_cv"] = await self.battery(quiet=True)

        status, p = await self.sensor_read_raw(NDP_SENSOR_ACCEL_XYZ)
        if status == 0 and len(p) == 6:
            result["accel"] = struct.unpack("<hhh", p)
        else:
            result["accel"] = None

        if st["ds18b20_present"]:
            status, p = await self.sensor_read_raw(NDP_SENSOR_DS18B20, retries=3, delay=0.8)
            result["temperature_mC"] = struct.unpack("<i", p)[0] if status == 0 and len(p) == 4 else None
        else:
            result["temperature_mC"] = None

        status, p = await self.sensor_read_raw(NDP_SENSOR_ACCEL_METRICS)
        result["vibration"] = (int.from_bytes(p[:2], "little"), int.from_bytes(p[2:4], "little")) if status == 0 and len(p) >= 4 else None

        if st["hall_mode"]:
            status, p = await self.sensor_read_raw(NDP_SENSOR_HALL)
            if status == 0 and len(p) >= 4:
                result["hall"] = int.from_bytes(p[:4], "little", signed=(st["hall_mode"] == HALL_MODE_QUADRATURE))
            else:
                result["hall"] = None
        else:
            result["hall"] = None

        if not quiet:
            print("=== HA / APPLICATION SENSORS ===")
            print(f"Battery:         {result['battery_cv']/100.0:.2f} V")
            if result["accel"] is None:
                print("Accelerometer:   unavailable")
            else:
                x,y,z = result["accel"]
                print(f"Accelerometer:   X={x} Y={y} Z={z} mg")
            if result["temperature_mC"] is None:
                print("DS18B20:         unavailable")
            else:
                print(f"DS18B20:         {result['temperature_mC']/1000.0:.3f} C")
            if result["vibration"] is None:
                print("Vibration:       unavailable")
            else:
                rms, peak = result["vibration"]
                print(f"Vibration:       RMS={rms} mg peak={peak} mg")
            if not st["hall_mode"]:
                print("Hall:            disabled")
            else:
                print(f"Hall:            {result['hall']}")
        return result


async def find_device(suffix: str, timeout: float):
    """Find the physical-button NUS programming plane with early exit.

    A full ``BleakScanner.discover(timeout=...)`` always waits for the complete
    scan window.  That is useful for diagnostics but wrong for interactive NUS
    commands: the board may already have been seen after a few advertisements,
    while the CLI needlessly burns the rest of the programming window before
    attempting the connection.

    Use a live detection callback and stop scanning as soon as a strong target
    is observed.  We deliberately do not use ``find_device_by_filter()`` since
    some BlueZ/Bleak combinations fail to deliver that filtered callback even
    though an unfiltered scan sees the same advertisement.

    A MAC suffix alone is not enough because the Application and NUS planes use
    the same BLE address.  Early success therefore requires the exact NUS name
    or the NUS service UUID in addition to the requested board identity.
    """
    suffix = suffix.replace(":", "").replace("-", "").upper()
    wanted = f"nRFClaw(NUS)-{suffix}"
    nus_uuid = NUS_SERVICE.lower()

    loop = asyncio.get_running_loop()
    found_event = asyncio.Event()
    best = {"score": -1, "dev": None}

    def consider(dev, adv):
        address = (getattr(dev, "address", "") or "")
        address_compact = address.replace(":", "").replace("-", "").upper()
        adv_name = getattr(adv, "local_name", None) if adv is not None else None
        name = (adv_name or getattr(dev, "name", None) or "").strip()
        uuids = {
            u.lower()
            for u in ((getattr(adv, "service_uuids", None) or []) if adv is not None else [])
        }

        name_match = name.upper() == wanted.upper()
        address_match = bool(suffix) and address_compact.endswith(suffix)
        nus_match = nus_uuid in uuids

        # Another nearby nRFClaw can advertise NUS too, so UUID alone cannot
        # select a board. Conversely MAC alone cannot distinguish NDP from NUS.
        if not (name_match or address_match):
            return
        if not (name_match or nus_match):
            return

        score = 0
        if nus_match:
            score += 16
        if name_match:
            score += 8
        if address_match:
            score += 4

        if score > best["score"]:
            best["score"] = score
            best["dev"] = dev

        # Exact name is authoritative.  NUS UUID + requested BLE suffix is
        # also strong enough and avoids waiting for a later scan-response name.
        if name_match or (nus_match and address_match):
            loop.call_soon_threadsafe(found_event.set)

    scanner = BleakScanner(consider)
    try:
        await scanner.start()
        try:
            await asyncio.wait_for(found_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
    finally:
        await scanner.stop()

    if best["dev"] is None:
        raise RuntimeError(
            f"{wanted} not found. Press P0.21 if the programming session is closed."
        )

    return wanted, best["dev"]



def decode_resetreas(value: int) -> list[str]:
    names = [name for bit, name in RESETREAS_FLAGS.items() if value & (1 << bit)]
    return names or ["POWER_ON/UNKNOWN"]


def decode_board_info_core(p: bytes) -> dict:
    if len(p) != 15:
        raise RuntimeError(f"Invalid BOARD_INFO core length: {len(p)}")
    return {
        "board_type": p[0], "hw_major": p[1], "hw_minor": p[2],
        "fw_major": p[3], "fw_minor": p[4], "fw_patch": p[5],
        "prerelease": p[6], "build": int.from_bytes(p[7:9], "little"),
        "ndp_version": p[9], "vm_abi": p[10], "softdevice_type": p[11],
        "sd_major": p[12], "sd_minor": p[13], "sd_patch": p[14],
    }


def decode_board_memory_physical(p: bytes) -> dict:
    if len(p) != 12:
        raise RuntimeError(f"Invalid BOARD_MEMORY physical length: {len(p)}")
    return {
        "flash_total": int.from_bytes(p[0:4], "little"),
        "ram_total": int.from_bytes(p[4:8], "little"),
        "flash_page_size": int.from_bytes(p[8:10], "little"),
        "flash_page_count": int.from_bytes(p[10:12], "little"),
    }


def decode_board_memory_layout(p: bytes) -> dict:
    if len(p) != 14:
        raise RuntimeError(f"Invalid BOARD_MEMORY layout length: {len(p)}")
    return {
        "data_start": int.from_bytes(p[0:4], "little"),
        "data_end": int.from_bytes(p[4:8], "little"),
        "reserved_free_start": int.from_bytes(p[8:12], "little"),
        "reserved_free_bytes": int.from_bytes(p[12:14], "little"),
    }


def decode_board_bootloader_payload(p: bytes) -> dict:
    if len(p) < 5:
        raise RuntimeError(f"Short BOARD_BOOTLOADER response: {len(p)} bytes")
    return {
        "present": bool(p[0]),
        "address": int.from_bytes(p[1:5], "little"),
    }


class NRFClawClient:
    def __init__(self, dev, auth_key: bytes):
        self.dev = dev
        self.auth_key = auth_key
        self.client = BleakClient(dev, timeout=20.0)
        self.response_queue = asyncio.Queue()
        self.event_queue = asyncio.Queue()
        self._string_parts = {}
        self._ndp_seq = 0

    async def __aenter__(self):
        await self.client.connect()
        await self.client.start_notify(
            NUS_TX,
            self._notify,
        )
        return self

    async def __aexit__(self, exc_type, exc, tb):
        try:
            await self.client.stop_notify(NUS_TX)
        except Exception:
            pass

        await self.client.disconnect()

    def _notify(self, _sender, data: bytearray):
        packet = bytes(data)

        # VM asynchronous event frame.
        if len(packet) >= 4 and packet[0] == VM_EVENT_FRAME:
            kind = packet[1]
            channel = packet[2]
            flags = packet[3]
            payload = packet[4:]

            if kind == VM_NOTIFY_U32:
                if len(payload) == 4:
                    value = int.from_bytes(
                        payload,
                        "little",
                        signed=False,
                    )

                    self.event_queue.put_nowait(
                        (kind, channel, value)
                    )
                return

            if kind == VM_NOTIFY_STRING:
                if flags & VM_FLAG_START:
                    self._string_parts[channel] = bytearray()

                buf = self._string_parts.setdefault(
                    channel,
                    bytearray(),
                )

                buf.extend(payload)

                if flags & VM_FLAG_END:
                    raw = bytes(buf)
                    self._string_parts.pop(
                        channel,
                        None,
                    )

                    self.event_queue.put_nowait(
                        (
                            kind,
                            channel,
                            raw.decode(
                                "utf-8",
                                errors="replace",
                            ),
                        )
                    )

                return

        # Normal command response.
        self.response_queue.put_nowait(packet)

    async def command(
        self,
        cmd: int,
        payload: bytes = b"",
        timeout: float = 5.0,
    ) -> bytes:
        packet = bytes([cmd]) + payload

        if len(packet) > 20:
            raise ValueError(
                f"NUS command packet too large: {len(packet)}"
            )

        await self.client.write_gatt_char(
            NUS_RX,
            packet,
            response=False,
        )

        wanted = cmd | 0x80
        end = asyncio.get_running_loop().time() + timeout

        while True:
            remain = end - asyncio.get_running_loop().time()

            if remain <= 0:
                raise TimeoutError(
                    f"Timeout waiting response for command 0x{cmd:02X}"
                )

            try:
                data = await asyncio.wait_for(
                    self.response_queue.get(),
                    remain,
                )
            except asyncio.TimeoutError as exc:
                raise TimeoutError(
                    f"Timeout waiting response for command 0x{cmd:02X}"
                ) from exc

            if len(data) >= 2 and data[0] == wanted:
                status = data[1]

                if status != 0:
                    raise RuntimeError(
                        f"Command 0x{cmd:02X}: "
                        f"{STATUS_NAMES.get(status, hex(status))}"
                    )

                return data[2:]

    async def wait_event(
        self,
        kind: int,
        channel: int,
        timeout: float = 10.0,
    ):
        end = asyncio.get_running_loop().time() + timeout

        while True:
            remain = end - asyncio.get_running_loop().time()

            if remain <= 0:
                raise TimeoutError(
                    f"Timeout waiting VM event "
                    f"kind={kind} channel={channel}"
                )

            try:
                event = await asyncio.wait_for(
                    self.event_queue.get(),
                    remain,
                )
            except asyncio.TimeoutError as exc:
                raise TimeoutError(
                    f"Timeout waiting VM event "
                    f"kind={kind} channel={channel}"
                ) from exc

            if event[0] == kind and event[1] == channel:
                return event[2]

            print("VM event:", event)

    async def ndp_command_raw(
        self,
        opcode: int,
        payload: bytes = b"",
        timeout: float = 5.0,
    ) -> tuple[int, bytes]:
        if len(payload) > 255:
            raise ValueError("NDP-SESSION payload too large")

        self._ndp_seq = (self._ndp_seq + 1) & 0xFF
        seq = self._ndp_seq
        frame = bytes([0x00, opcode & 0xFF, seq, len(payload)]) + payload

        # S132 is currently configured with ATT MTU=23, therefore the
        # write-without-response payload must fit in 20 bytes.
        if len(frame) > 20:
            raise ValueError(
                f"NDP frame too large for current NUS ATT MTU: {len(frame)} bytes "
                "(maximum 20)."
            )

        await self.client.write_gatt_char(
            NUS_RX,
            frame,
            response=False,
        )

        end = asyncio.get_running_loop().time() + timeout

        while True:
            remain = end - asyncio.get_running_loop().time()
            if remain <= 0:
                raise TimeoutError(
                    f"Timeout waiting NDP response opcode=0x{opcode:02X} seq={seq}"
                )

            try:
                data = await asyncio.wait_for(
                    self.response_queue.get(),
                    remain,
                )
            except asyncio.TimeoutError as exc:
                raise TimeoutError(
                    f"Timeout waiting NDP response opcode=0x{opcode:02X} seq={seq}"
                ) from exc

            if len(data) < 5:
                continue

            _flags, op, rx_seq, plen = data[0], data[1], data[2], data[3]
            if op != opcode or rx_seq != seq:
                continue

            if len(data) != 4 + plen:
                raise RuntimeError(
                    f"Invalid NDP frame length: header={plen} actual={len(data)-4}"
                )

            body = data[4:]
            if not body:
                raise RuntimeError("NDP response missing status")

            return body[0], body[1:]

    async def ndp_command(
        self,
        opcode: int,
        payload: bytes = b"",
        timeout: float = 5.0,
    ) -> bytes:
        status, body = await self.ndp_command_raw(
            opcode,
            payload,
            timeout,
        )

        if status != 0:
            raise RuntimeError(
                f"NDP 0x{opcode:02X}: "
                f"{NDP_STATUS_NAMES.get(status, hex(status))}"
            )

        return body

    async def ndp_access_status(self, quiet: bool = False) -> int:
        p = await self.ndp_command(NDP_ACCESS_STATUS)
        if len(p) != 1:
            raise RuntimeError(f"Invalid ACCESS_STATUS length: {len(p)}")
        level = p[0]
        if not quiet:
            name = {
                NDP_ACCESS_PUBLIC: "PUBLIC",
                NDP_ACCESS_CONTROL: "CONTROL",
                NDP_ACCESS_PROVISION: "PROVISION",
            }.get(level, str(level))
            print(f"NDP access:       {name}")
        return level

    async def ndp_auth_control(self, key: bytes | None = None):
        # R3.7: NUS is intentionally keyless. Physical P0.21 access is the
        # security boundary, so NDP-over-NUS privileged operations bypass
        # Application-plane authentication in firmware.
        return

    async def ndp_logout(self):
        await self.ndp_command(NDP_AUTH_LOGOUT)
        print("NDP access:       PUBLIC")

    async def ndp_key_status(self, quiet: bool = False) -> int:
        p = await self.ndp_command(NDP_KEY_STATUS)
        if len(p) != 1:
            raise RuntimeError(f"Invalid NDP KEY_STATUS length: {len(p)}")
        status = p[0]
        if not quiet:
            names = {0: "unconfigured (NDP open)", 1: "ready (NDP protected)",
                     2: "saving", 3: "flash error"}
            print(f"NDP key:          {names.get(status, status)}")
        return status

    async def ndp_key_get(self, quiet: bool = False) -> bytes:
        status = await self.ndp_key_status(quiet=True)
        if status == 0:
            raise RuntimeError("No NDP key has been generated on this device")
        if status == 2:
            raise RuntimeError("NDP key is still being saved; retry in a moment")
        if status == 3:
            raise RuntimeError("NDP key flash store is in ERROR state")
        parts = []
        for chunk in range(3):
            parts.append(await self.ndp_command(NDP_KEY_GET, bytes([chunk])))
        key = b"".join(parts)
        if len(key) != 32:
            raise RuntimeError(f"Invalid stored NDP key length: {len(key)}")
        if not quiet:
            print(f"NDP key:          hex:{key.hex()}")
        return key

    async def ndp_key_generate(self) -> bytes:
        p = await self.ndp_command(NDP_KEY_GENERATE)
        if len(p) != 1:
            raise RuntimeError(f"Invalid NDP KEY_GENERATE response length: {len(p)}")
        deadline = asyncio.get_running_loop().time() + 10.0
        while True:
            status = await self.ndp_key_status(quiet=True)
            if status == 1:
                break
            if status == 3:
                raise RuntimeError("NDP key flash save failed")
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("Timed out waiting for NDP key flash commit")
            await asyncio.sleep(0.20)
        key = await self.ndp_key_get(quiet=True)
        print("NDP protection:   enabled")
        print(f"NDP key:          hex:{key.hex()}")
        print("Store this key securely; Application/HA now requires it.")
        return key

    async def ndp_info(self, quiet: bool = False):
        p = await self.ndp_command(NDP_INFO)
        if len(p) < 3:
            raise RuntimeError("Short NDP INFO response")
        result = {
            "ndp_version": p[0],
            "vm_abi": p[1],
            "board_type": p[2],
        }
        if not quiet:
            print(f"NDP version:     {result['ndp_version']}")
            print(f"VM ABI:          {result['vm_abi']}")
            print(
                "Board:           "
                f"{NDP_BOARD_TYPE_NAMES.get(result['board_type'], result['board_type'])}"
            )
        return result

    async def ndp_status(self, quiet: bool = False):
        p = await self.ndp_command(NDP_STATUS)
        if len(p) < 2:
            raise RuntimeError("Short NDP STATUS response")
        result = {
            "hall_mode": p[0],
            "ds18b20_present": bool(p[1]),
        }
        if not quiet:
            hall = {0: "disabled", 1: "single D11", 2: "quadrature D11+D14"}.get(
                result["hall_mode"], str(result["hall_mode"])
            )
            print(f"Hall mode:       {hall}")
            print(
                f"DS18B20 present: {'yes' if result['ds18b20_present'] else 'no'}"
            )
        return result

    async def board_info(self, quiet: bool = False):
        core = decode_board_info_core(
            await self.ndp_command(NDP_BOARD_INFO, b"\x00")
        )
        devid = await self.ndp_command(NDP_BOARD_INFO, b"\x01")
        git = await self.ndp_command(NDP_BOARD_INFO, b"\x02")
        if len(devid) != 8:
            raise RuntimeError(f"Invalid BOARD_INFO Device ID length: {len(devid)}")
        if len(git) != 4:
            raise RuntimeError(f"Invalid BOARD_INFO git32 length: {len(git)}")
        d = dict(core)
        d["deviceid0"] = int.from_bytes(devid[0:4], "little")
        d["deviceid1"] = int.from_bytes(devid[4:8], "little")
        d["git32"] = int.from_bytes(git, "little")

        if not quiet:
            board_name = NDP_BOARD_TYPE_NAMES.get(d["board_type"], d["board_type"])
            sd_name = NDP_SOFTDEVICE_NAMES.get(d["softdevice_type"], d["softdevice_type"])
            pre = f"-pre{d['prerelease']}" if d["prerelease"] else ""
            print(f"Board:           {board_name} Rev {d['hw_major']}.{d['hw_minor']}")
            print(
                f"Firmware:        nRFClaw "
                f"{d['fw_major']}.{d['fw_minor']}.{d['fw_patch']}{pre} "
                f"build {d['build']}"
            )
            print(f"Git32:           {d['git32']:08X}")
            print(f"NDP / VM ABI:    {d['ndp_version']} / {d['vm_abi']}")
            print(
                f"SoftDevice:      {sd_name} "
                f"{d['sd_major']}.{d['sd_minor']}.{d['sd_patch']}"
            )
            print(f"Device ID:       {d['deviceid1']:08X}{d['deviceid0']:08X}")
        return d

    async def board_memory(self, quiet: bool = False):
        physical = decode_board_memory_physical(
            await self.ndp_command(NDP_BOARD_MEMORY, b"\x00")
        )
        layout = decode_board_memory_layout(
            await self.ndp_command(NDP_BOARD_MEMORY, b"\x01")
        )
        d = {**physical, **layout}
        if not quiet:
            print(f"Flash total:     {d['flash_total']} bytes ({d['flash_total']/1024:.0f} KiB)")
            print(f"RAM total:       {d['ram_total']} bytes ({d['ram_total']/1024:.0f} KiB)")
            print(f"Flash page:      {d['flash_page_size']} bytes")
            print(f"Flash pages:     {d['flash_page_count']}")
            print(f"nRFClaw data:    0x{d['data_start']:08X}..0x{d['data_end']:08X}")
            print(
                f"Reserved free:   0x{d['reserved_free_start']:08X} "
                f"({d['reserved_free_bytes']} bytes)"
            )
        return d

    async def board_reset(self, quiet: bool = False):
        p = await self.ndp_command(NDP_BOARD_RESET)
        if len(p) < 4:
            raise RuntimeError(f"Short BOARD_RESET response: {len(p)} bytes")
        resetreas = int.from_bytes(p[0:4], "little")
        result = {"resetreas": resetreas, "reasons": decode_resetreas(resetreas)}
        if not quiet:
            print(f"RESETREAS:       0x{resetreas:08X}")
            print(f"Reset reason:    {', '.join(result['reasons'])}")
        return result

    async def board_bootloader(self, quiet: bool = False):
        p = await self.ndp_command(NDP_BOARD_BOOTLOADER)
        d = decode_board_bootloader_payload(p)
        if not quiet:
            print(f"Bootloader:      {'present' if d['present'] else 'not detected'}")
            print(f"Bootloader addr: 0x{d['address']:08X}")
        return d

    async def sensor_read(self, sensor_id: int) -> bytes:
        return await self.ndp_command(
            NDP_SENSOR_READ,
            bytes([sensor_id & 0xFF]),
        )

    async def accel_probe(self):
        p = await self.ndp_command(NDP_ACCEL_PROBE)
        if len(p) != 4:
            raise RuntimeError(f"Invalid ACCEL_PROBE length: {len(p)}")
        present, address, whoami, mode = p
        print("=== ACCEL PROBE ===")
        print(f"Present:         {'yes' if present else 'no'}")
        print(f"I2C address:     0x{address:02X}")
        print(f"WHO_AM_I:        0x{whoami:02X} (expected 0x33)")
        print(f"Mode:            {mode}")
        return present, address, whoami, mode

    async def accel_read(self):
        p = await self.sensor_read(NDP_SENSOR_ACCEL_XYZ)
        if len(p) != 6:
            raise RuntimeError(f"Invalid ACCEL SENSOR_READ length: {len(p)}")

        x, y, z = struct.unpack("<hhh", p)
        print("=== ACCEL ===")
        print(f"X:               {x} mg")
        print(f"Y:               {y} mg")
        print(f"Z:               {z} mg")
        return x, y, z

    async def ds18b20_read(self, retries: int = 3):
        last_status = None

        for attempt in range(retries):
            status, p = await self.ndp_command_raw(
                NDP_SENSOR_READ,
                bytes([NDP_SENSOR_DS18B20]),
            )
            last_status = status

            if status == 0:
                if len(p) != 4:
                    raise RuntimeError(
                        f"Invalid DS18B20 SENSOR_READ length: {len(p)}"
                    )
                milli_c = struct.unpack("<i", p)[0]
                print("=== DS18B20 ===")
                print("Present:         yes")
                print(f"Temperature:     {milli_c / 1000.0:.3f} C")
                return milli_c

            if status != 4:  # BUSY
                raise RuntimeError(
                    f"DS18B20 SENSOR_READ: "
                    f"{NDP_STATUS_NAMES.get(status, hex(status))}"
                )

            if attempt + 1 < retries:
                await asyncio.sleep(0.8)

        raise RuntimeError(
            "DS18B20 conversion unavailable/busy after retries "
            f"(last status={NDP_STATUS_NAMES.get(last_status, last_status)})"
        )

    async def accel_config(
        self,
        mode: str,
        odr_hz: int,
        full_scale_g: int,
        threshold_mg: int,
        duration_ms: int,
        low_power: bool,
        key: bytes,
    ):
        if mode not in ACCEL_MODES:
            raise ValueError(f"unknown accel mode: {mode}")
        if odr_hz not in (1, 10, 25, 50, 100, 200, 400):
            raise ValueError("ODR must be one of: 1,10,25,50,100,200,400 Hz")
        if full_scale_g not in (2, 4, 8, 16):
            raise ValueError("scale must be one of: 2,4,8,16 g")
        if not 0 <= threshold_mg <= 0xFFFF:
            raise ValueError("threshold must be 0..65535 mg")
        if not 0 <= duration_ms <= 0xFFFF:
            raise ValueError("duration must be 0..65535 ms")

        await self.ndp_auth_control(key)

        payload = struct.pack(
            "<BHBHHB",
            ACCEL_MODES[mode],
            odr_hz,
            full_scale_g,
            threshold_mg,
            duration_ms,
            1 if low_power else 0,
        )
        await self.ndp_command(NDP_ACCEL_CONFIG, payload)

        print("=== ACCEL CONFIG ===")
        print(f"Mode:            {mode}")
        print(f"ODR:             {odr_hz} Hz")
        print(f"Scale:           +/-{full_scale_g} g")
        print(f"Threshold:       {threshold_mg} mg")
        print(f"Duration:        {duration_ms} ms")
        print(f"Low power:       {'yes' if low_power else 'no'}")

    async def vibration_read(self, csv_path: str | None = None):
        status, p = await self.ndp_command_raw(NDP_VIB_INFO)

        if status != 0:
            raise RuntimeError(
                "No vibration capture available: "
                f"{NDP_STATUS_NAMES.get(status, hex(status))}"
            )

        if len(p) != 15:
            raise RuntimeError(f"Invalid VIB_INFO length: {len(p)}")

        count, rate, rms, peak, p2p, zero_cross, sequence = struct.unpack(
            "<BHHHHHI",
            p,
        )

        print("=== VIBRATION ===")
        print(f"Capture seq:     {sequence}")
        print(f"Samples:         {count}")
        print(f"Sample rate:     {rate} Hz")
        print(f"RMS:             {rms} mg")
        print(f"Peak:            {peak} mg")
        print(f"Peak-to-peak:    {p2p} mg")
        print(f"Zero-cross est.: {zero_cross} Hz (not FFT)")

        rows = []

        for index in range(0, count, 2):
            requested = min(2, count - index)
            p = await self.ndp_command(
                NDP_VIB_READ,
                bytes([index, requested]),
            )

            if len(p) < 2:
                raise RuntimeError("Short VIB_READ response")

            returned_index = p[0]
            got = p[1]

            if returned_index != index:
                raise RuntimeError(
                    f"VIB_READ index mismatch: requested={index} "
                    f"returned={returned_index}"
                )

            expected = 2 + got * 6
            if len(p) != expected:
                raise RuntimeError(
                    f"Invalid VIB_READ payload length: got={len(p)} "
                    f"expected={expected}"
                )

            for j in range(got):
                x, y, z = struct.unpack_from("<hhh", p, 2 + j * 6)
                rows.append((index + j, x, y, z))

        if csv_path:
            with open(csv_path, "w", newline="") as fp:
                writer = csv.writer(fp)
                writer.writerow(["sample", "x_mg", "y_mg", "z_mg"])
                writer.writerows(rows)
            print(f"CSV:             {csv_path}")
        else:
            print("\n sample      x_mg      y_mg      z_mg")
            for sample, x, y, z in rows:
                print(f"{sample:7d} {x:9d} {y:9d} {z:9d}")

        return {
            "sequence": sequence,
            "count": count,
            "rate_hz": rate,
            "rms_mg": rms,
            "peak_mg": peak,
            "peak_to_peak_mg": p2p,
            "zero_cross_hz": zero_cross,
            "samples": rows,
        }

    async def hall_status(self):
        st = await self.ndp_status(quiet=True)
        info = await self.ndp_info(quiet=True)
        p = await self.ndp_command(NDP_STATUS, b"\x02")
        if len(p) != 12:
            raise RuntimeError(f"Invalid Hall diagnostics length: {len(p)}")
        mode, channel, h1, h2 = p[0:4]
        rc1 = int.from_bytes(p[4:8], "little")
        rc2 = int.from_bytes(p[8:12], "little")
        board = NDP_BOARD_TYPE_NAMES.get(info["board_type"], str(info["board_type"]))
        if mode == HALL_MODE_SINGLE:
            hall = f"single HALL{channel}"
        elif mode == HALL_MODE_QUADRATURE:
            hall = "quadrature HALL1+HALL2"
        else:
            hall = "disabled"
        print(f"Board:           {board}")
        print(f"Hall mode:       {hall}")
        print(f"HALL1:           P0.{h1:02d}")
        print(f"HALL2:           P0.{h2:02d} (quadrature or single channel 2)")
        print(f"HALL1 GPIOTE rc: 0x{rc1:08X}")
        print(f"HALL2 GPIOTE rc: 0x{rc2:08X}")
        st.update({"channel":channel,"hall1_pin":h1,"hall2_pin":h2,"hall1_rc":rc1,"hall2_rc":rc2})
        return st

    async def hall_config(
        self,
        mode: int,
        pullup: bool = False,
        count_edges: bool = False,
        emit_events: bool = False,
        channel: int = 1,
    ):
        payload = bytes([
            mode & 0xFF,
            1 if pullup else 0,
            1 if count_edges else 0,
            1 if emit_events else 0,
            channel & 0xFF,
        ])
        await self.ndp_command(NDP_HALL_CONFIG, payload)

        names = {
            HALL_MODE_DISABLED: "disabled",
            HALL_MODE_SINGLE: f"single HALL{channel}",
            HALL_MODE_QUADRATURE: "quadrature HALL1+HALL2",
        }
        print(f"Hall configured: {names.get(mode, mode)}")
        if mode != HALL_MODE_DISABLED:
            print(f"Pull-up:         {'on' if pullup else 'off'}")
            print(f"Count edges:     {'yes' if count_edges else 'no'}")
            print(f"Emit events:     {'yes' if emit_events else 'no'}")

    async def hall_disable(self):
        await self.hall_config(HALL_MODE_DISABLED)

    async def hall_read(self):
        st = await self.ndp_status(quiet=True)
        if st["hall_mode"] == HALL_MODE_DISABLED:
            raise RuntimeError("Hall is disabled; configure single or quadrature first")

        p = await self.sensor_read(NDP_SENSOR_HALL)
        if len(p) != 4:
            raise RuntimeError(f"Invalid HALL SENSOR_READ length: {len(p)}")

        if st["hall_mode"] == HALL_MODE_QUADRATURE:
            value = int.from_bytes(p, "little", signed=True)
            print(f"Hall position:   {value}")
        else:
            value = int.from_bytes(p, "little", signed=False)
            print(f"Hall count:      {value}")
        return value

    async def beacon_status(self):
        p = await self.ndp_command(NDP_BLE_BEACON_CONTROL, b"\x00")
        if len(p) != 4: raise RuntimeError(f"Invalid beacon status length: {len(p)}")
        role=p[0]; interval=int.from_bytes(p[1:3],"little"); tx=int.from_bytes(p[3:4],"little",signed=True)
        names={0:"off",1:"beacon/broadcaster",2:"peripheral",3:"central",4:"scanner"}
        print(f"BLE role:        {names.get(role,role)}")
        print(f"Interval:        {interval} ms")
        print(f"TX power:        {tx:+d} dBm")
        print(f"Connectable:     {'no' if role==1 else 'yes' if role==2 else '-'}")
        return role,interval,tx

    async def beacon_start(self, interval: int, tx_power: int):
        if not (20 <= interval <= 10240): raise RuntimeError("Beacon interval must be 20..10240 ms")
        if not (-40 <= tx_power <= 8): raise RuntimeError("Invalid BLE TX power")
        payload=bytes([1,interval&0xff,(interval>>8)&0xff,tx_power&0xff])
        await self.ndp_command(NDP_BLE_BEACON_CONTROL,payload)
        print("Beacon configured. It starts after this NUS session disconnects.")
        await self.beacon_status()

    async def beacon_stop(self):
        await self.ndp_command(NDP_BLE_BEACON_CONTROL,b"\x02")
        print("Beacon disabled")

    async def lora_get(self, quiet: bool = False):
        p = await self.ndp_command(NDP_RADIO_GET)
        if len(p) != 9:
            raise RuntimeError(f"Invalid RADIO_GET response length: {len(p)}")
        d = {
            "frequency_hz": int.from_bytes(p[0:4], "little"),
            "power_dbm": int.from_bytes(p[4:5], "little", signed=True),
            "sf": p[5],
            "bw_khz": int.from_bytes(p[6:8], "little"),
            "cr": p[8],
        }
        if not quiet:
            print(f"Frequency:       {d['frequency_hz']} Hz ({d['frequency_hz']/1e6:.3f} MHz)")
            print(f"TX power:        {d['power_dbm']} dBm")
            print(f"SF:              {d['sf']}")
            print(f"Bandwidth:       {d['bw_khz']} kHz")
            print(f"Coding rate:     4/{4 + d['cr']} (CR={d['cr']})")
        return d

    async def lora_get_ext(self, quiet: bool = False):
        p = await self.ndp_command(NDP_RADIO_GET_EXT)
        if len(p) not in (12, 15):
            raise RuntimeError(f"Invalid RADIO_GET_EXT response length: {len(p)}")
        d = {
            "frequency_hz": int.from_bytes(p[0:4], "little"),
            "power_dbm": int.from_bytes(p[4:5], "little", signed=True),
            "sf": p[5],
            "bw_khz": int.from_bytes(p[6:8], "little"),
            "cr": p[8],
            "sync_word": p[9],
            "preamble": int.from_bytes(p[10:12], "little"),
            "persist_busy": bool(p[12]) if len(p) >= 15 else False,
            "persist_generation": int.from_bytes(p[13:15], "little") if len(p) >= 15 else None,
        }
        if not quiet:
            print(f"Frequency:       {d['frequency_hz']} Hz ({d['frequency_hz']/1e6:.3f} MHz)")
            print(f"TX power:        {d['power_dbm']} dBm")
            print(f"SF:              {d['sf']}")
            print(f"Bandwidth:       {d['bw_khz']} kHz")
            print(f"Coding rate:     4/{4 + d['cr']} (CR={d['cr']})")
            print(f"Sync word:       0x{d['sync_word']:02X}")
            print(f"Preamble:        {d['preamble']} symbols")
            if d["persist_generation"] is None:
                print("Persistence:     enabled")
            else:
                state = "saving" if d["persist_busy"] else "committed"
                print(f"Persistence:     {state}, generation={d['persist_generation']}")
        return d

    async def lora_wait_persist(self, timeout: float = 5.0):
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            d = await self.lora_get_ext(quiet=True)
            if not d.get("persist_busy", False):
                return d
            if asyncio.get_running_loop().time() >= deadline:
                raise TimeoutError("LoRa profile is still waiting for persistent flash commit")
            await asyncio.sleep(0.05)

    async def lora_set_ext(
        self, frequency_hz: int, power_dbm: int, sf: int, bw_khz: int, cr: int,
        sync_word: int, preamble: int,
    ):
        if not 150_000_000 <= frequency_hz <= 960_000_000:
            raise ValueError("frequency must be 150000000..960000000 Hz")
        if not -9 <= power_dbm <= 22:
            raise ValueError("power must be -9..22 dBm")
        if not 5 <= sf <= 11:
            raise ValueError("SF must be 5..11")
        if bw_khz not in (125, 250, 500):
            raise ValueError("BW must be 125, 250 or 500 kHz")
        if not 1 <= cr <= 4:
            raise ValueError("CR must be 1..4 (LoRa 4/5..4/8)")
        if not 0 <= sync_word <= 0xFF:
            raise ValueError("sync word must be 0..255")
        if not 1 <= preamble <= 0xFFFF:
            raise ValueError("preamble must be 1..65535 symbols")
        payload = (
            struct.pack("<I", frequency_hz)
            + struct.pack("<b", power_dbm)
            + bytes([sf])
            + struct.pack("<H", bw_khz)
            + bytes([cr, sync_word])
            + struct.pack("<H", preamble)
        )
        await self.ndp_command(NDP_RADIO_SET_EXT, payload)
        d = await self.lora_wait_persist()
        print(f"Extended LoRa profile committed to flash (generation={d.get('persist_generation')}).")
        await self.lora_get_ext()

    async def lora_set(
        self,
        frequency_hz: int,
        power_dbm: int,
        sf: int,
        bw_khz: int,
        cr: int,
    ):
        if not 150_000_000 <= frequency_hz <= 960_000_000:
            raise ValueError("frequency must be 150000000..960000000 Hz")
        if not -9 <= power_dbm <= 22:
            raise ValueError("power must be -9..22 dBm")
        if not 5 <= sf <= 11:
            raise ValueError("SF must be 5..11")
        if bw_khz not in (125, 250, 500):
            raise ValueError("BW must be 125, 250 or 500 kHz")
        if not 1 <= cr <= 4:
            raise ValueError("CR must be 1..4 (LoRa 4/5..4/8)")

        payload = (
            struct.pack("<I", frequency_hz)
            + struct.pack("<b", power_dbm)
            + bytes([sf])
            + struct.pack("<H", bw_khz)
            + bytes([cr])
        )
        await self.ndp_command(NDP_RADIO_SET, payload)
        try:
            d = await self.lora_wait_persist()
            print(f"LoRa profile committed to flash (generation={d.get('persist_generation')}).")
        except RuntimeError:
            print("LoRa profile accepted (legacy firmware: persistence status unavailable).")
        await self.lora_get()

    async def lora_info(self):
        print("=== DIRECT LORA DIAGNOSTIC ===")
        try:
            d = await self.lora_get_ext(quiet=True)
        except RuntimeError:
            d = await self.lora_get(quiet=True)
        print(f"Frequency:       {d['frequency_hz']} Hz ({d['frequency_hz']/1e6:.3f} MHz)")
        print(f"TX power:        {d['power_dbm']:+d} dBm")
        print(f"SF:              {d['sf']}")
        print(f"Bandwidth:       {d['bw_khz']} kHz")
        print(f"Coding rate:     4/{4 + d['cr']}")
        if "sync_word" in d:
            print(f"Sync word:       0x{d['sync_word']:02X}")
            print(f"Preamble:        {d['preamble']} symbols")
        print("Transport:       physical NUS direct diagnostic")
        print("VM/compiler:     bypassed")

    async def lora_send(self, text: str, quiet: bool = False):
        data = text.encode("utf-8")
        if not data or len(data) > 48:
            raise ValueError("direct LoRa diagnostic payload must be 1..48 UTF-8 bytes")
        await self.ndp_command(NDP_LORA_DIAG_SEND, data)
        if not quiet:
            print(f"LoRa TX accepted: {len(data)} bytes")
            print("HEX:   " + " ".join(f"{b:02X}" for b in data))
            print("ASCII: " + data.decode("utf-8", errors="replace"))

    async def lora_send_continuous(self, base: str, every_s: float, count: int = 0):
        import time

        if every_s <= 0:
            raise ValueError("--every must be > 0 seconds")
        if count < 0:
            raise ValueError("--count must be >= 0 (0 = unlimited)")
        if not base:
            raise ValueError("payload base must not be empty")

        # Reserve at least '_01'.  The numeric field automatically grows after 99.
        if len((base + "_01").encode("utf-8")) > 48:
            raise ValueError("payload base is too long; base + '_01' must fit in 48 UTF-8 bytes")

        print(f"LoRa TX: continuous every {every_s:g}s")
        print(f"Payload sequence: {base}_01, {base}_02, {base}_03, ...")
        if count:
            print(f"Packet count: {count}")
        else:
            print("Packet count: unlimited (Ctrl-C or NUS disconnect to stop)")

        started = time.monotonic()
        seq = 1
        next_tx = started

        while count == 0 or seq <= count:
            if not self.client.is_connected:
                raise ConnectionError("NUS connection lost")

            now = time.monotonic()
            delay = next_tx - now
            if delay > 0:
                await asyncio.sleep(delay)

            if not self.client.is_connected:
                raise ConnectionError("NUS connection lost")

            suffix = f"{seq:02d}"
            payload = f"{base}_{suffix}"
            data = payload.encode("utf-8")
            if len(data) > 48:
                raise ValueError(
                    f"generated packet {seq} exceeds 48 UTF-8 bytes: {payload!r}"
                )

            await self.lora_send(payload, quiet=True)
            elapsed = time.monotonic() - started
            print(
                f"[{seq:04d} +{elapsed:8.2f}s] TX len={len(data)} "
                f"ASCII: {payload}"
            )

            seq += 1
            next_tx += every_s

        elapsed = time.monotonic() - started
        print(f"LoRa TX complete: {seq - 1} packet(s) in {elapsed:.2f}s")

    async def sensor_read_raw(self, sensor_id: int, retries=1, delay=0.2):
        """Raw SENSOR_READ helper for the physical NUS NDP client."""
        last = (None, b"")
        for attempt in range(retries):
            last = await self.ndp_command_raw(
                NDP_SENSOR_READ,
                bytes([sensor_id & 0xFF]),
            )
            if last[0] != 4:  # NDP_BUSY
                return last
            if attempt + 1 < retries:
                await asyncio.sleep(delay)
        return last

    def _decode_ninalink_lab_status(self, p: bytes) -> dict:
        if len(p) != 12:
            raise RuntimeError(f"Invalid NINALINK_LAB status length: {len(p)}")
        return {
            "active": bool(p[0]),
            "period_s": int.from_bytes(p[1:3], "little"),
            "next_sequence": int.from_bytes(p[3:5], "little"),
            "last_entry_count": p[5],
            "last_frame_length": p[6],
            "last_result": p[7],
            "node_id": int.from_bytes(p[8:12], "little"),
        }

    async def ninalink_lab_status(self):
        p = await self.ndp_command(NDP_NINALINK_LAB, b"\x00")
        s = self._decode_ninalink_lab_status(p)
        result_names = {0:"IDLE",1:"SENT",2:"NO_DATA",3:"RADIO_BUSY",4:"BUILD_ERROR",5:"TIMER_ERROR"}
        print("=== NINALINK B4.1 LAB TX ===")
        print(f"Active:          {'yes' if s['active'] else 'no'}")
        print(f"Node ID:         0x{s['node_id']:08X}")
        print(f"Period:          {s['period_s']} s")
        print(f"Next sequence:   {s['next_sequence']}")
        print(f"Last entries:    {s['last_entry_count']}")
        print(f"Last frame len:  {s['last_frame_length']} bytes")
        print(f"Last result:     {result_names.get(s['last_result'], s['last_result'])}")
        return s

    async def ninalink_lab_start(self, every_s: int):
        if every_s < 2 or every_s > 300:
            raise ValueError("--every must be 2..300 seconds")
        st, bp = await self.sensor_read_raw(NDP_SENSOR_BATTERY, retries=5, delay=0.20)
        if st == 0 and len(bp) >= 2:
            cv = int.from_bytes(bp[:2], "little")
            print(f"Primed battery:  {cv / 100.0:.2f} V")
        else:
            print(f"Battery prime:   {NDP_STATUS_NAMES.get(st, st)}")
        st, tp = await self.sensor_read_raw(NDP_SENSOR_DS18B20, retries=4, delay=0.80)
        if st == 0 and len(tp) == 4:
            mc = int.from_bytes(tp, "little", signed=True)
            print(f"Primed temp:     {mc / 1000.0:.3f} C")
        elif st == 3:
            print("Primed temp:     unavailable (DS18B20 unsupported/absent)")
        else:
            print(f"Temperature prime: {NDP_STATUS_NAMES.get(st, st)}")
        p = await self.ndp_command(NDP_NINALINK_LAB, b"\x01" + struct.pack("<H", every_s))
        s = self._decode_ninalink_lab_status(p)
        print(f"NinaLink lab TX armed: node=0x{s['node_id']:08X}, every {s['period_s']}s; first TX after one period.")
        print("RAM-only mode; stops on reboot or ninalink-tx-stop.")
        return s

    async def ninalink_tx_max(self):
        p = await self.ndp_command(NDP_NINALINK_LAB, b"\x03")
        s = self._decode_ninalink_lab_status(p)
        print(
            f"NinaLink 64-byte CAP_REPORT submitted: "
            f"node=0x{s['node_id']:08X}, "
            f"next_seq={s['next_sequence']}, "
            f"entries={s['last_entry_count']}, "
            f"len={s['last_frame_length']}"
        )
        if s["last_frame_length"] != 64 or s["last_entry_count"] != 6:
            raise RuntimeError(
                "Firmware did not report the expected 64-byte/6-entry frame"
            )
        return s

    async def ninalink_lab_stop(self):
        p = await self.ndp_command(NDP_NINALINK_LAB, b"\x02")
        s = self._decode_ninalink_lab_status(p)
        print("NinaLink lab TX stopped.")
        return s

    def _decode_ninalink_link_status(self, p: bytes) -> dict:
        if len(p) != 14:
            raise RuntimeError(
                f"Invalid NINALINK_LINK status length: {len(p)}"
            )
        return {
            "state": p[0],
            "result": p[1],
            "sequence": int.from_bytes(p[2:4], "little"),
            "tx_len": p[4],
            "ack_len": p[5],
            "ack_rssi_x2": int.from_bytes(p[6:8], "little", signed=True),
            "ack_snr_x4": int.from_bytes(p[8:10], "little", signed=True),
            "acked_count": int.from_bytes(p[10:12], "little"),
            "timeout_count": int.from_bytes(p[12:14], "little"),
        }

    async def ninalink_link_status(self, quiet: bool = False):
        p = await self.ndp_command(NDP_NINALINK_LINK, b"\x00")
        s = self._decode_ninalink_link_status(p)
        if not quiet:
            states = {0:"IDLE",1:"WAIT_TX",2:"WAIT_ACK",3:"BACKOFF",4:"DONE",5:"APP_RESULT_ARM",6:"APP_RESULT_TX"}
            results = {
                0:"NONE",1:"ACKED",2:"TIMEOUT",3:"BAD_ACK",
                4:"TX_FAIL",5:"RX_FAIL",6:"NO_DATA",7:"BUILD_FAIL",
            }
            print("=== NINALINK NODE LINK ===")
            print(f"State:           {states.get(s['state'], s['state'])}")
            print(f"Result:          {results.get(s['result'], s['result'])}")
            print(f"Sequence:        {s['sequence']}")
            print(f"TX frame:        {s['tx_len']} bytes")
            print(f"RX/downlink frame:{s['ack_len']:>4} bytes")
            if s["ack_len"]:
                print(f"ACK RSSI:        {s['ack_rssi_x2']/2:.1f} dBm")
                print(f"ACK SNR:         {s['ack_snr_x4']/4:.2f} dB")
            print(f"ACKed total:     {s['acked_count']}")
            print(f"Timeout total:   {s['timeout_count']}")
        return s

    async def ninalink_ack_test(self, window_ms: int = 600, wait_s: float = 3.0):
        import time
        if window_ms < 100 or window_ms > 4000:
            raise ValueError("--window must be 100..4000 ms")
        if wait_s <= 0:
            raise ValueError("--wait must be > 0 seconds")

        st, bp = await self.sensor_read_raw(
            NDP_SENSOR_BATTERY, retries=5, delay=0.20
        )
        if st == 0 and len(bp) >= 2:
            cv = int.from_bytes(bp[:2], "little")
            print(f"Primed battery:  {cv / 100.0:.2f} V")

        st, tp = await self.sensor_read_raw(
            NDP_SENSOR_DS18B20, retries=4, delay=0.80
        )
        if st == 0 and len(tp) == 4:
            mc = int.from_bytes(tp, "little", signed=True)
            print(f"Primed temp:     {mc / 1000.0:.3f} C")

        p = await self.ndp_command(
            NDP_NINALINK_LINK,
            b"\x01" + struct.pack("<H", window_ms),
        )
        s = self._decode_ninalink_link_status(p)
        print(
            f"NinaLink ACK_REQ submitted: seq={s['sequence']} "
            f"len={s['tx_len']} window={window_ms} ms"
        )

        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            await asyncio.sleep(0.05)
            s = await self.ninalink_link_status(quiet=True)
            if s["state"] == 3:
                break

        await self.ninalink_link_status()
        if s["state"] != 3:
            raise RuntimeError("ACK test did not reach DONE before --wait")
        if s["result"] != 1:
            raise RuntimeError(f"ACK test failed with result={s['result']}")
        return s

    async def ninalink_link_reliability_status(self, quiet: bool = False):
        p = await self.ndp_command(NDP_NINALINK_LINK, b"\x02")
        if len(p) != 10:
            raise RuntimeError(
                f"Invalid NINALINK_LINK reliability length: {len(p)}"
            )
        s = {
            "attempts": p[0],
            "max_attempts": p[1],
            "retry_count": int.from_bytes(p[2:4], "little"),
            "timeout_count": int.from_bytes(p[4:6], "little"),
            "base_backoff_ms": int.from_bytes(p[6:8], "little"),
            "last_backoff_ms": int.from_bytes(p[8:10], "little"),
        }
        if not quiet:
            print("=== NINALINK B4.4 RELIABILITY ===")
            print(f"Attempts:        {s['attempts']}/{s['max_attempts']}")
            print(f"Retry total:     {s['retry_count']}")
            print(f"Timeout total:   {s['timeout_count']}")
            print(f"Base backoff:    {s['base_backoff_ms']} ms")
            print(f"Last backoff:    {s['last_backoff_ms']} ms")
        return s

    async def ninalink_reliable_test(
            self, window_ms=600, attempts=3, backoff_ms=200,
            wait_s=6.0, expect_timeout=False):
        import time

        if window_ms < 100 or window_ms > 4000:
            raise ValueError("--window must be 100..4000 ms")
        if attempts < 1 or attempts > 5:
            raise ValueError("--attempts must be 1..5")
        if attempts > 1 and (backoff_ms < 1 or backoff_ms > 4000):
            raise ValueError("--backoff must be 1..4000 ms")
        if wait_s <= 0:
            raise ValueError("--wait must be > 0 seconds")

        st, bp = await self.sensor_read_raw(
            NDP_SENSOR_BATTERY, retries=5, delay=0.20
        )
        if st == 0 and len(bp) >= 2:
            cv = int.from_bytes(bp[:2], "little")
            print(f"Primed battery:  {cv / 100.0:.2f} V")

        st, tp = await self.sensor_read_raw(
            NDP_SENSOR_DS18B20, retries=4, delay=0.80
        )
        if st == 0 and len(tp) == 4:
            mc = int.from_bytes(tp, "little", signed=True)
            print(f"Primed temp:     {mc / 1000.0:.3f} C")

        payload = (
            b"\x01"
            + struct.pack("<H", window_ms)
            + bytes([attempts])
            + struct.pack("<H", backoff_ms if attempts > 1 else 0)
        )

        p = await self.ndp_command(NDP_NINALINK_LINK, payload)
        s = self._decode_ninalink_link_status(p)

        print(
            f"Reliable NinaLink submitted: seq={s['sequence']} "
            f"window={window_ms} ms attempts={attempts} "
            f"backoff={backoff_ms if attempts > 1 else 0} ms"
        )

        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            await asyncio.sleep(0.05)
            s = await self.ninalink_link_status(quiet=True)
            if s["state"] == 4:
                break

        await self.ninalink_link_status()
        rs = await self.ninalink_link_reliability_status()

        if s["state"] != 4:
            raise RuntimeError("Reliable test did not reach DONE before --wait")

        if expect_timeout:
            if s["result"] != 2:
                raise RuntimeError(
                    f"Expected TIMEOUT, got result={s['result']}"
                )
            print("Expected timeout observed: PASS")
        else:
            if s["result"] != 1:
                raise RuntimeError(
                    f"Reliable test failed with result={s['result']}"
                )
            print("Reliable ACK observed: PASS")

        return s, rs

    async def ninalink_bridge_reliability(self):
        p = await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x04")
        if len(p) != 7:
            raise RuntimeError(
                f"Invalid bridge reliability length: {len(p)}"
            )
        s = {
            "duplicates": int.from_bytes(p[0:2], "little"),
            "ack_sent": int.from_bytes(p[2:4], "little"),
            "ack_test_dropped": int.from_bytes(p[4:6], "little"),
            "drop_next_ack": bool(p[6]),
        }
        print("=== NINALINK B4.4 BRIDGE RELIABILITY ===")
        print(f"Duplicates:      {s['duplicates']}")
        print(f"ACK sent:        {s['ack_sent']}")
        print(f"Test ACK drops:  {s['ack_test_dropped']}")
        print(f"Drop next ACK:   {'yes' if s['drop_next_ack'] else 'no'}")
        return s

    async def ninalink_bridge_drop_next_ack(self):
        await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x05")
        print(
            "Bridge armed: the next unique ACK_REQ frame will be accepted "
            "but its first ACK will be suppressed."
        )

    async def ninalink_bridge_app_status(self):
        p = await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x07")
        if len(p) != 15:
            raise RuntimeError(
                f"Invalid bridge app status length: {len(p)}"
            )
        states = {0:"IDLE",1:"PENDING",2:"WAIT_RESULT",3:"DONE"}
        results = {
            0:"OK",1:"UNSUPPORTED_CAP",2:"BAD_TYPE",
            3:"BAD_VALUE",4:"APPLY_FAILED",255:"NONE",
        }
        s = {
            "pending": bool(p[0]),
            "target_node": int.from_bytes(p[1:5], "little"),
            "command_seq": int.from_bytes(p[5:7], "little"),
            "requested_value": bool(p[7]),
            "state": p[8],
            "result": p[9],
            "sent_count": int.from_bytes(p[10:12], "little"),
            "completed_count": int.from_bytes(p[12:14], "little"),
            "timeout_count": p[14],
        }
        print("=== NINALINK B4.5 APPLICATION DOWNLINK ===")
        print(f"Pending:         {'yes' if s['pending'] else 'no'}")
        print(f"Target node:     0x{s['target_node']:08X}")
        print(f"Command seq:     {s['command_seq']}")
        print(
            f"tracking_active: "
            f"{'ON' if s['requested_value'] else 'OFF'}"
        )
        print(f"State:           {states.get(s['state'], s['state'])}")
        print(f"Result:          {results.get(s['result'], s['result'])}")
        print(f"Sent total:      {s['sent_count']}")
        print(f"Completed total: {s['completed_count']}")
        print(f"Result timeouts: {s['timeout_count']}")
        return s

    async def ninalink_cap_set_tracking(self, node_id: int, value: str):
        active = value.lower() == "on"
        payload = (
            b"\x06"
            + struct.pack("<I", node_id)
            + bytes([1 if active else 0])
        )
        await self.ndp_command(NDP_NINALINK_BRIDGE, payload)
        print(
            f"Queued CAP_SET tracking_active={'ON' if active else 'OFF'} "
            f"for node 0x{node_id:08X}."
        )
        print(
            "It will be delivered in the node's next ACK receive window."
        )
        return await self.ninalink_bridge_app_status()

    async def ninalink_node_app_status(self):
        p = await self.ndp_command(NDP_NINALINK_LINK, b"\x03")
        if len(p) != 9:
            raise RuntimeError(
                f"Invalid node app status length: {len(p)}"
            )
        results = {
            0:"OK",1:"UNSUPPORTED_CAP",2:"BAD_TYPE",
            3:"BAD_VALUE",4:"APPLY_FAILED",
        }
        s = {
            "valid": bool(p[0]),
            "sequence": int.from_bytes(p[1:3], "little"),
            "result": p[3],
            "applied_count": int.from_bytes(p[4:6], "little"),
            "duplicate_count": int.from_bytes(p[6:8], "little"),
            "tracking_active": bool(p[8]),
        }
        print("=== NINALINK B4.5 NODE APPLICATION ===")
        print(f"Command seen:    {'yes' if s['valid'] else 'no'}")
        print(f"Command seq:     {s['sequence']}")
        print(f"Result:          {results.get(s['result'], s['result'])}")
        print(f"Applied total:   {s['applied_count']}")
        print(f"Duplicate total: {s['duplicate_count']}")
        print(
            f"Tracking active: "
            f"{'yes' if s['tracking_active'] else 'no'}"
        )
        return s

    async def ninalink_node_drop_next_app_result(self):
        await self.ndp_command(NDP_NINALINK_LINK, b"\x04")
        print(
            "Node armed: the next CAP_SET will be applied, but its "
            "application-result ACK will be suppressed once."
        )
        return await self.ninalink_node_app_reliability()

    async def ninalink_node_app_reliability(self):
        p = await self.ndp_command(NDP_NINALINK_LINK, b"\x05")
        if len(p) != 3:
            raise RuntimeError(
                f"Invalid node app reliability length: {len(p)}"
            )
        s = {
            "drop_next_result": bool(p[0]),
            "result_drop_count": int.from_bytes(p[1:3], "little"),
        }
        print("=== NINALINK B4.6 NODE APP RELIABILITY ===")
        print(
            f"Drop next result: "
            f"{'yes' if s['drop_next_result'] else 'no'}"
        )
        print(f"Result drops:     {s['result_drop_count']}")
        return s

    async def _ninalink_command_result_chunks(self, opcode, action, total_len):
        data = bytearray()
        off = 0
        while off < total_len:
            p = await self.ndp_command(opcode, bytes([action, off]))
            if len(p) < 2 or p[0] != total_len or p[1] != off:
                raise RuntimeError(f"Invalid COMMAND result chunk: {p.hex()}")
            chunk = p[2:]
            if not chunk:
                raise RuntimeError("Empty COMMAND result chunk")
            data.extend(chunk)
            off += len(chunk)
        return bytes(data[:total_len])

    async def ninalink_command(self, node_id, command_id, data_hex=""):
        if not (0 <= command_id <= 0xFFFF):
            raise ValueError("--id must be 0..0xFFFF")
        clean = data_hex.replace(" ", "").replace(":", "")
        if len(clean) % 2:
            raise ValueError("--data must have an even number of hex digits")
        try:
            data = bytes.fromhex(clean)
        except ValueError as exc:
            raise ValueError("--data must be hexadecimal") from exc
        if len(data) > 7:
            raise ValueError("B4.7 NDP gate accepts at most 7 argument bytes")
        req = (
            b"\x08"
            + struct.pack("<I", node_id)
            + struct.pack("<H", command_id)
            + bytes([len(data)])
            + data
        )
        await self.ndp_command(NDP_NINALINK_BRIDGE, req)
        print(
            f"Queued COMMAND id=0x{command_id:04X} "
            f"for node 0x{node_id:08X}, args={data.hex() or '-'}."
        )
        return await self.ninalink_command_status()

    async def ninalink_command_echo(self, node_id, token):
        if not (0 <= token <= 0xFFFFFFFF):
            raise ValueError("--token must be 0..0xFFFFFFFF")
        return await self.ninalink_command(
            node_id, 0x0001, struct.pack("<I", token).hex()
        )

    async def ninalink_command_status(self):
        p = await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x09")
        if len(p) != 15:
            raise RuntimeError(f"Invalid bridge COMMAND status length: {len(p)}")
        states = {0:"IDLE",1:"PENDING",2:"WAIT_RESULT",3:"DONE"}
        results = {0:"OK",1:"UNSUPPORTED",2:"BAD_ARGS",3:"EXEC_FAILED",255:"NONE"}
        s = {
            "pending": bool(p[0]),
            "target_node": int.from_bytes(p[1:5], "little"),
            "command_seq": int.from_bytes(p[5:7], "little"),
            "command_id": int.from_bytes(p[7:9], "little"),
            "state": p[9],
            "result": p[10],
            "sent_count": p[11],
            "completed_count": p[12],
            "timeout_count": p[13],
            "result_len": p[14],
        }
        data = b""
        if s["result_len"]:
            data = await self._ninalink_command_result_chunks(
                NDP_NINALINK_BRIDGE, 10, s["result_len"]
            )
        s["result_data"] = data
        print("=== NINALINK B4.7 COMMAND DOWNLINK ===")
        print(f"Pending:         {'yes' if s['pending'] else 'no'}")
        print(f"Target node:     0x{s['target_node']:08X}")
        print(f"Command seq:     {s['command_seq']}")
        print(f"Command ID:      0x{s['command_id']:04X}")
        print(f"State:           {states.get(s['state'], s['state'])}")
        print(f"Result:          {results.get(s['result'], s['result'])}")
        print(f"Sent total:      {s['sent_count']}")
        print(f"Completed total: {s['completed_count']}")
        print(f"Result timeouts: {s['timeout_count']}")
        print(f"Result length:   {s['result_len']}")
        if data:
            print(f"Result data:     {data.hex()}")
            if s["command_id"] == 1 and len(data) == 4:
                print(f"ECHO token:      0x{int.from_bytes(data, 'little'):08X}")
        return s

    async def ninalink_node_command_status(self):
        p = await self.ndp_command(NDP_NINALINK_LINK, b"\x06")
        if len(p) < 11:
            raise RuntimeError(f"Invalid node COMMAND status length: {len(p)}")
        result_len = p[6]
        data = bytes(p[11:])
        if result_len > len(data):
            data = await self._ninalink_command_result_chunks(
                NDP_NINALINK_LINK, 7, result_len
            )
        else:
            data = data[:result_len]
        results = {0:"OK",1:"UNSUPPORTED",2:"BAD_ARGS",3:"EXEC_FAILED"}
        s = {
            "valid": bool(p[0]),
            "sequence": int.from_bytes(p[1:3], "little"),
            "command_id": int.from_bytes(p[3:5], "little"),
            "result": p[5],
            "result_len": result_len,
            "executed_count": int.from_bytes(p[7:9], "little"),
            "duplicate_count": int.from_bytes(p[9:11], "little"),
            "result_data": data,
        }
        print("=== NINALINK B4.7 NODE COMMAND ===")
        print(f"Command seen:    {'yes' if s['valid'] else 'no'}")
        print(f"Command seq:     {s['sequence']}")
        print(f"Command ID:      0x{s['command_id']:04X}")
        print(f"Result:          {results.get(s['result'], s['result'])}")
        print(f"Executed total:  {s['executed_count']}")
        print(f"Duplicate total: {s['duplicate_count']}")
        print(f"Result length:   {s['result_len']}")
        if data:
            print(f"Result data:     {data.hex()}")
            if s["command_id"] == 1 and len(data) == 4:
                print(f"ECHO token:      0x{int.from_bytes(data, 'little'):08X}")
        return s

    async def ninalink_node_drop_next_command_result(self):
        await self.ndp_command(NDP_NINALINK_LINK, b"\x04")
        print(
            "Node armed: the next CAP_SET/COMMAND will execute, "
            "but its result frame will be suppressed once."
        )
        return await self.ninalink_node_app_reliability()

    def _decode_ninalink_bridge_status(self, p: bytes) -> dict:
        if len(p) != 15:
            raise RuntimeError(
                f"Invalid NINALINK_BRIDGE status length: {len(p)}"
            )
        return {
            "active": bool(p[0]),
            "queued": p[1],
            "received": int.from_bytes(p[2:4], "little"),
            "valid": int.from_bytes(p[4:6], "little"),
            "invalid": int.from_bytes(p[6:8], "little"),
            "dropped": int.from_bytes(p[8:10], "little"),
            "radio_dropped": int.from_bytes(p[10:12], "little"),
            "last_error": p[12],
            "ack_sent": int.from_bytes(p[13:15], "little"),
        }

    async def ninalink_bridge_status(self, quiet: bool = False):
        p = await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x00")
        s = self._decode_ninalink_bridge_status(p)
        if not quiet:
            errors = {
                0: "NONE",
                1: "CORE",
                2: "SEMANTIC",
                3: "RADIO_STOPPED",
            }
            print("=== NINALINK B4.2 BRIDGE ===")
            print(f"Active:          {'yes' if s['active'] else 'no'}")
            print(f"Queued:          {s['queued']}")
            print(f"RF received:     {s['received']}")
            print(f"NinaLink valid:  {s['valid']}")
            print(f"NinaLink invalid:{s['invalid']:>5}")
            print(f"Bridge dropped:  {s['dropped']}")
            print(f"Radio dropped:   {s['radio_dropped']}")
            print(f"ACK sent:        {s['ack_sent']}")
            print(
                f"Last error:      "
                f"{errors.get(s['last_error'], s['last_error'])}"
            )
        return s

    async def ninalink_bridge_start(self):
        await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x01")
        print("NinaLink B4.2 bridge RX started.")
        print("Bridge remains active after this NUS connection closes.")
        return await self.ninalink_bridge_status()

    async def ninalink_bridge_stop(self):
        await self.ndp_command(NDP_NINALINK_BRIDGE, b"\x02")
        print("NinaLink B4.2 bridge RX stopped.")

    async def ninalink_bridge_rx(self, timeout_s: float = 45.0):
        import time

        if timeout_s <= 0:
            raise ValueError("timeout must be > 0 seconds")

        status = await self.ninalink_bridge_status(quiet=True)
        if not status["active"]:
            raise RuntimeError(
                "NinaLink bridge is not active; run ninalink-bridge-start first"
            )

        print(
            f"NinaLink B4.2 bridge: draining validated frames "
            f"({timeout_s:g}s window)"
        )
        print(
            "Frames shown here already passed on-device B3.1 validation; "
            "CAP_REPORT/CAP_EVENT also passed B3.2 parsing."
        )

        deadline = time.monotonic() + timeout_s
        packet_count = 0
        assembly = bytearray()
        expected_len = None
        packet_rssi_x2 = 0
        packet_snr_x4 = 0

        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                break
            if not self.client.is_connected:
                raise ConnectionError("NUS connection lost")

            await asyncio.sleep(min(0.03, remain))
            remain = deadline - time.monotonic()
            if remain <= 0:
                break

            poll_timeout = min(1.0, max(0.20, remain))
            try:
                p = await asyncio.wait_for(
                    self.ndp_command(
                        NDP_NINALINK_BRIDGE, b"\x03", timeout=poll_timeout
                    ),
                    timeout=poll_timeout + 0.25,
                )
            except (asyncio.TimeoutError, TimeoutError):
                continue

            if not p or p[0] == 0:
                continue
            if p[0] == 2:
                raise RuntimeError("NinaLink bridge stopped unexpectedly")
            if p[0] != 1 or len(p) < 7:
                raise RuntimeError(
                    f"Invalid bridge chunk response: {p.hex()}"
                )

            total_len = p[1]
            offset = p[2]
            rssi_x2 = int.from_bytes(p[3:5], "little", signed=True)
            snr_x4 = int.from_bytes(p[5:7], "little", signed=True)
            chunk = p[7:]

            if total_len < 15 or total_len > 64:
                raise RuntimeError(
                    f"Invalid validated frame length: {total_len}"
                )

            if offset == 0:
                assembly = bytearray()
                expected_len = total_len
                packet_rssi_x2 = rssi_x2
                packet_snr_x4 = snr_x4
            elif expected_len is None:
                raise RuntimeError("Bridge continuation without packet start")

            if total_len != expected_len:
                raise RuntimeError("Bridge frame length changed during reassembly")
            if offset != len(assembly):
                raise RuntimeError(
                    f"Bridge chunk offset mismatch: "
                    f"got {offset}, expected {len(assembly)}"
                )
            if len(assembly) + len(chunk) > expected_len:
                raise RuntimeError("Bridge chunk exceeds frame length")

            assembly.extend(chunk)

            if len(assembly) != expected_len:
                continue

            data = bytes(assembly)
            packet_count += 1
            elapsed = timeout_s - max(0.0, deadline - time.monotonic())

            print(
                f"[{packet_count:04d} +{elapsed:7.2f}s] "
                f"VALIDATED len={len(data)} "
                f"RSSI={packet_rssi_x2/2:.1f} dBm "
                f"SNR={packet_snr_x4/4:.2f} dB"
            )

            nf = decode_ninalink(data)
            tname = NINALINK_TYPE_NAMES.get(
                nf["type"], f"0x{nf['type']:02X}"
            )
            print(
                f"NinaLink v1 {tname} "
                f"net=0x{nf['network_id']:04X} "
                f"node=0x{nf['node_id']:08X} "
                f"seq={nf['sequence']}"
            )
            for entry in nf["entries"]:
                print("  " + format_ninalink_value(entry))

            assembly = bytearray()
            expected_len = None

        final_status = await self.ninalink_bridge_status(quiet=True)
        print(
            f"NinaLink bridge window complete: {packet_count} frame(s); "
            f"valid={final_status['valid']} "
            f"invalid={final_status['invalid']} "
            f"dropped={final_status['dropped']} "
            f"radio_dropped={final_status['radio_dropped']}"
        )
        return packet_count

    async def ninalink_rx(self, timeout_s: float = 45.0):
        import time

        if timeout_s <= 0:
            raise ValueError("timeout must be > 0 seconds")

        await self.lora_rx_cancel(quiet=True)
        await asyncio.wait_for(
            self.ndp_command(NDP_LORA_DIAG_RX, b"\x00", timeout=3.0),
            timeout=4.0,
        )

        print(f"NinaLink RX: continuous listening ({timeout_s:g}s window)")
        print("Chunked NDP/NUS transport: up to 8 RF bytes per BLE response.")
        print("Press Ctrl-C to abort; otherwise exits only at timeout or NUS disconnect.")

        deadline = time.monotonic() + timeout_s
        packet_count = 0
        assembly = bytearray()
        expected_len = None
        packet_rssi_x2 = 0
        packet_snr_x4 = 0

        try:
            while True:
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break
                if not self.client.is_connected:
                    raise ConnectionError("NUS connection lost")

                await asyncio.sleep(min(0.03, remain))
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break

                poll_timeout = min(1.0, max(0.20, remain))
                try:
                    p = await asyncio.wait_for(
                        self.ndp_command(
                            NDP_LORA_DIAG_RX, b"\x03", timeout=poll_timeout
                        ),
                        timeout=poll_timeout + 0.25,
                    )
                except (asyncio.TimeoutError, TimeoutError):
                    if not self.client.is_connected:
                        raise ConnectionError("NUS connection lost")
                    continue

                if not p or p[0] == 0:
                    continue
                if p[0] == 2:
                    raise RuntimeError("NinaLink continuous RX stopped unexpectedly")
                if p[0] != 1 or len(p) < 7:
                    raise RuntimeError(
                        f"Invalid chunked NinaLink RX response: {p.hex()}"
                    )

                total_len = p[1]
                offset = p[2]
                rssi_x2 = int.from_bytes(p[3:5], "little", signed=True)
                snr_x4 = int.from_bytes(p[5:7], "little", signed=True)
                chunk = p[7:]

                if total_len == 0 or total_len > 48:
                    raise RuntimeError(
                        f"Invalid chunked RF packet length: {total_len}"
                    )

                if offset == 0:
                    assembly = bytearray()
                    expected_len = total_len
                    packet_rssi_x2 = rssi_x2
                    packet_snr_x4 = snr_x4
                elif expected_len is None:
                    raise RuntimeError("Received continuation without packet start")

                if total_len != expected_len:
                    raise RuntimeError("RF packet length changed during reassembly")
                if offset != len(assembly):
                    raise RuntimeError(
                        f"Chunk offset mismatch: got {offset}, expected {len(assembly)}"
                    )
                if len(assembly) + len(chunk) > expected_len:
                    raise RuntimeError("Chunk exceeds announced RF packet length")

                assembly.extend(chunk)

                if len(assembly) != expected_len:
                    continue

                data = bytes(assembly)
                packet_count += 1
                elapsed = timeout_s - max(0.0, deadline - time.monotonic())

                print(
                    f"[{packet_count:04d} +{elapsed:7.2f}s] "
                    f"RX len={len(data)} "
                    f"RSSI={packet_rssi_x2/2:.1f} dBm "
                    f"SNR={packet_snr_x4/4:.2f} dB"
                )

                try:
                    nf = decode_ninalink(data)
                    tname = NINALINK_TYPE_NAMES.get(
                        nf["type"], f"0x{nf['type']:02X}"
                    )
                    print(
                        f"NinaLink v1 {tname} "
                        f"net=0x{nf['network_id']:04X} "
                        f"node=0x{nf['node_id']:08X} "
                        f"seq={nf['sequence']}"
                    )
                    for entry in nf["entries"]:
                        print("  " + format_ninalink_value(entry))
                except ValueError as exc:
                    print(f"NinaLink INVALID: {exc}")
                    print("HEX:   " + " ".join(f"{b:02X}" for b in data))

                assembly = bytearray()
                expected_len = None

        finally:
            if self.client.is_connected:
                await self.lora_rx_cancel(quiet=True)

        print(
            f"NinaLink RX window complete: "
            f"{packet_count} packet(s) in {timeout_s:g}s"
        )
        return packet_count

    async def lora_rx_cancel(self, quiet: bool = False):
        try:
            await asyncio.wait_for(
                self.ndp_command(NDP_LORA_DIAG_RX, b"\x02", timeout=2.0),
                timeout=3.0,
            )
            if not quiet:
                print("LoRa RX: cancelled")
        except Exception as exc:
            if not quiet:
                print(f"LoRa RX cancel warning: {exc}")

    async def lora_rx(self, timeout_s: float = 30.0, ninalink: bool = False):
        import time
        if timeout_s <= 0:
            raise ValueError("timeout must be > 0 seconds")

        # Clear a stale direct-RX session from a previous interrupted CLI run.
        await self.lora_rx_cancel(quiet=True)

        await asyncio.wait_for(
            self.ndp_command(NDP_LORA_DIAG_RX, b"\x00", timeout=3.0),
            timeout=4.0,
        )
        print(f"{'NinaLink' if ninalink else 'LoRa'} RX: continuous listening ({timeout_s:g}s window)")
        print("Press Ctrl-C to abort; otherwise exits only at timeout or NUS disconnect.")
        deadline = time.monotonic() + timeout_s
        packet_count = 0

        try:
            while True:
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break

                if not self.client.is_connected:
                    raise ConnectionError("NUS connection lost")

                # Poll quickly only to drain the firmware queue.  RF reception is
                # continuous in firmware, so BLE polling latency no longer opens
                # a receive gap on the LLCC68.
                await asyncio.sleep(min(0.05, remain))
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break

                poll_timeout = min(1.0, max(0.20, remain))
                try:
                    p = await asyncio.wait_for(
                        self.ndp_command(
                            NDP_LORA_DIAG_RX, b"\x01", timeout=poll_timeout
                        ),
                        timeout=poll_timeout + 0.25,
                    )
                except (asyncio.TimeoutError, TimeoutError):
                    if not self.client.is_connected:
                        raise ConnectionError("NUS connection lost")
                    continue

                if not p or p[0] == 0:
                    continue
                if p[0] == 2:
                    raise RuntimeError("LoRa continuous RX stopped unexpectedly")
                if p[0] != 1 or len(p) < 6:
                    raise RuntimeError(f"Invalid direct LoRa RX response: {p.hex()}")

                n = p[1]
                if len(p) != 6 + n:
                    raise RuntimeError("Invalid direct LoRa RX payload length")
                rssi_x2 = int.from_bytes(p[2:4], "little", signed=True)
                snr_x4 = int.from_bytes(p[4:6], "little", signed=True)
                data = p[6:]
                packet_count += 1
                elapsed = timeout_s - max(0.0, deadline - time.monotonic())
                print(f"[{packet_count:04d} +{elapsed:7.2f}s] RX len={n} "
                      f"RSSI={rssi_x2/2:.1f} dBm SNR={snr_x4/4:.2f} dB")
                if ninalink:
                    try:
                        nf = decode_ninalink(data)
                        tname = NINALINK_TYPE_NAMES.get(nf["type"], f"0x{nf['type']:02X}")
                        print(f"NinaLink v1 {tname} net=0x{nf['network_id']:04X} node=0x{nf['node_id']:08X} seq={nf['sequence']}")
                        for entry in nf["entries"]:
                            print("  " + format_ninalink_value(entry))
                    except ValueError as exc:
                        print(f"NinaLink INVALID: {exc}")
                        print("HEX:   " + " ".join(f"{b:02X}" for b in data))
                else:
                    print("HEX:   " + " ".join(f"{b:02X}" for b in data))
                    print("ASCII: " + data.decode("utf-8", errors="replace"))
                # Deliberately continue.  lora-rx is now a receive window, not
                # a one-packet command.
        finally:
            # On normal expiry or Ctrl-C, stop continuous RX.  If NUS has
            # already fallen this best-effort cancel simply cannot be delivered.
            if self.client.is_connected:
                await self.lora_rx_cancel(quiet=True)

        print(f"LoRa RX window complete: {packet_count} packet(s) in {timeout_s:g}s")
        return packet_count

    async def lora_patch(
        self,
        frequency_hz=None,
        power_dbm=None,
        sf=None,
        bw_khz=None,
        cr=None,
        sync_word=None,
        preamble=None,
    ):
        if sync_word is not None or preamble is not None:
            cur = await self.lora_get_ext(quiet=True)
            await self.lora_set_ext(
                cur["frequency_hz"] if frequency_hz is None else frequency_hz,
                cur["power_dbm"] if power_dbm is None else power_dbm,
                cur["sf"] if sf is None else sf,
                cur["bw_khz"] if bw_khz is None else bw_khz,
                cur["cr"] if cr is None else cr,
                cur["sync_word"] if sync_word is None else sync_word,
                cur["preamble"] if preamble is None else preamble,
            )
            return
        cur = await self.lora_get(quiet=True)
        await self.lora_set(
            cur["frequency_hz"] if frequency_hz is None else frequency_hz,
            cur["power_dbm"] if power_dbm is None else power_dbm,
            cur["sf"] if sf is None else sf,
            cur["bw_khz"] if bw_khz is None else bw_khz,
            cur["cr"] if cr is None else cr,
        )

    async def board(self):
        print("=== BOARD INFO ===")
        await self.board_info()
        print("\n=== MEMORY ===")
        await self.board_memory()
        print("\n=== RESET ===")
        await self.board_reset()
        print("\n=== BOOTLOADER ===")
        await self.board_bootloader()
        print("\n=== NDP STATUS ===")
        await self.ndp_status()

    async def hello(self):
        p = await self.command(CMD_HELLO)

        if len(p) < 10:
            raise RuntimeError("Short HELLO response")

        proto = f"{p[0]}.{p[1]}"
        suffix = "".join(f"{b:02X}" for b in p[2:5])
        max_program = p[5] | (p[6] << 8)
        loaded = p[7] | (p[8] << 8)
        state = p[9]

        print(f"Protocol:       {proto}")
        print(f"MAC suffix:     {suffix}")
        print(f"Max bytecode:   {max_program} bytes")
        print(f"Loaded program: {loaded} bytes")
        print(f"VM state:       {VM_STATES.get(state, state)}")

    async def status(
        self,
        quiet: bool = False,
    ):
        p = await self.command(CMD_STATUS)

        if len(p) < 6:
            raise RuntimeError("Short STATUS response")

        state = p[0]
        pc = p[1] | (p[2] << 8)
        loaded = p[3] | (p[4] << 8)
        programming = bool(p[5])

        flash = None
        slot = 0xFF
        generation = 0

        sched_state = None
        sched_mode = None
        next_epoch = 0

        if len(p) >= 12:
            flash = p[6]
            slot = p[7]
            generation = int.from_bytes(
                p[8:12],
                "little",
            )

        if len(p) >= 18:
            sched_state = p[12]
            sched_mode = p[13]
            next_epoch = int.from_bytes(p[14:18], "little")

        if not quiet:
            line = (
                f"VM={VM_STATES.get(state, state)} "
                f"PC={pc} "
                f"loaded={loaded} "
                f"programming={programming}"
            )

            if flash is not None:
                line += (
                    f" flash={FLASH_STATES.get(flash, flash)}"
                    f" slot={'none' if slot == 0xFF else slot}"
                    f" generation={generation}"
                )

            if sched_state is not None:
                line += (
                    f" sched={SCHED_STATES.get(sched_state, sched_state)}"
                    f" mode={sched_mode}"
                    f" next={next_epoch}"
                )

            print(line)

        return {
            "state": state,
            "pc": pc,
            "loaded": loaded,
            "programming": programming,
            "flash": flash,
            "slot": slot,
            "generation": generation,
            "sched_state": sched_state,
            "sched_mode": sched_mode,
            "next_epoch": next_epoch,
        }

    async def sync_time(self):
        epoch = int(time.time())

        await self.command(
            CMD_TIME_SYNC,
            struct.pack("<I", epoch),
        )

        print(f"RTC synchronized: epoch={epoch}")

    async def ping(self):
        await self.command(CMD_PING)
        print("PONG")

    async def run_loaded(self):
        await self.command(CMD_PROGRAM_RUN)
        print("RUN accepted.")

    async def stop(self):
        await self.command(CMD_PROGRAM_STOP)
        print("STOP accepted.")

    async def wait_flash_commit(
        self,
        previous_generation: int,
        timeout: float = 15.0,
    ):
        end = asyncio.get_running_loop().time() + timeout

        while True:
            st = await self.status(quiet=True)

            flash = st["flash"]

            if flash is None:
                raise RuntimeError(
                    "Firmware does not expose Stage-4 Flash status. "
                    "Are you running the Stage-4 firmware?"
                )

            if flash == 3:
                raise RuntimeError(
                    "Flash persistence ERROR"
                )

            if (
                flash == 1
                and st["slot"] != 0xFF
                and st["generation"] != previous_generation
            ):
                print(
                    "Flash committed: "
                    f"slot={st['slot']} "
                    f"generation={st['generation']}"
                )

                return st

            if asyncio.get_running_loop().time() >= end:
                raise TimeoutError(
                    "Timeout waiting for atomic Flash commit"
                )

            await asyncio.sleep(0.15)

    async def reset_device(self):
        await self.command(CMD_RESET)
        print("RESET accepted; device rebooting.")

    async def enter_dfu(self):
        await self.command(CMD_DFU_ENTER)
        print("DFU accepted; device rebooting into nRFClaw bootloader.")

    async def inject_event(self, event_type: int, arg0: int = 0, arg1: int = 0):
        payload = bytes([event_type & 0xFF]) + struct.pack("<II", arg0 & 0xFFFFFFFF, arg1 & 0xFFFFFFFF)
        await self.command(CMD_EVENT_INJECT, payload)
        print(f"Event injected: type={event_type} arg0={arg0} arg1={arg1}")

    async def capabilities(self):
        p = await self.command(CMD_CAPABILITIES)

        # Backward compatibility with the first Stage-8 firmware.
        if len(p) == 4:
            active = int.from_bytes(p, "little")
            supported = active
        elif len(p) == 8:
            supported = int.from_bytes(p[0:4], "little")
            active = int.from_bytes(p[4:8], "little")
        else:
            raise RuntimeError("short CAPABILITIES response")

        for cap, name in CAP_NAMES.items():
            is_supported = bool(supported & (1 << cap))
            is_active = bool(active & (1 << cap))

            if is_active:
                state = "yes"
            elif not is_supported:
                state = "no (unsupported)"
            elif cap == 4:  # LIS2DH12 can be probed safely by WHO_AM_I.
                state = "no (not present)"
            elif cap in (3, 8):  # DS18B20 / HALL are opt-in shared resources.
                state = "no (inactive)"
            else:
                state = "no (inactive)"

            if name == "SERIAL":
                state = "supported, active" if is_active else "supported, inactive"
            elif name == "TRACKING":
                state = "experimental, active" if is_active else "experimental, inactive"
            elif name == "VIB_HEALTH":
                state = "experimental, yes" if is_active else ("experimental, unavailable" if is_supported else "no (unsupported)")
            elif name == "VIB_AUTO":
                state = "experimental, yes" if is_active else ("experimental, unavailable" if is_supported else "no (unsupported)")

            print(f"{name:10s}: {state}")

        return supported, active
    async def upload(
        self,
        code: bytes,
        persist: bool = True,
        schedule: dict | None = None,
    ):
        if not code:
            raise ValueError("Empty bytecode")

        if schedule is None:
            schedule = schedule_manual()

        before = await self.status(quiet=True)

        previous_generation = before["generation"]

        crc = crc16_ccitt(code)

        print(
            f"Uploading {len(code)} bytes, "
            f"CRC16=0x{crc:04X}"
        )

        await self.command(
            CMD_PROGRAM_BEGIN,
            struct.pack(
                "<HH",
                len(code),
                crc,
            ),
        )

        offset = 0

        while offset < len(code):
            chunk = code[
                offset:offset + 17
            ]

            await self.command(
                CMD_PROGRAM_DATA,
                struct.pack("<H", offset)
                + chunk,
            )

            offset += len(chunk)

            print(
                f"\r  {offset}/{len(code)}",
                end="",
                flush=True,
            )

        print()

        await self.command(
            CMD_PROGRAM_SCHEDULE,
            encode_schedule(schedule),
        )

        auth = bytecode_hmac(
            code,
            schedule,
            self.auth_key,
        )

        print(
            f"HMAC-SHA256={auth.hex()}"
        )

        auth_offset = 0

        while auth_offset < len(auth):
            chunk = auth[
                auth_offset:auth_offset + 17
            ]

            await self.command(
                CMD_PROGRAM_AUTH,
                struct.pack("<H", auth_offset)
                + chunk,
            )

            auth_offset += len(chunk)

        await self.command(
            CMD_PROGRAM_END
        )

        print(
            "Program authenticated, validated and loaded in RAM."
        )

        if persist:
            print(
                "Waiting for atomic Flash commit..."
            )

            await self.wait_flash_commit(
                previous_generation
            )

    async def battery(self, schedule=None, run_now=True):
        code = battery_notify_test()

        await self.upload(
            code,
            persist=True,
            schedule=schedule,
        )

        if not run_now:
            return

        await self.run_loaded()

        try:
            cv = await self.wait_event(
                VM_NOTIFY_U32,
                1,
                timeout=10.0,
            )
        except TimeoutError:
            print(
                "No battery notification received; "
                "querying status..."
            )
            await self.status()
            raise

        print(
            f"Battery: {cv / 100.0:.2f} V "
            f"({cv} cV)"
        )

    async def notify_string(
        self,
        text: str,
        channel: int,
    ):
        code = string_notify_test(
            text,
            channel,
        )

        await self.upload(
            code,
            persist=True,
        )

        await self.run_loaded()

        try:
            value = await self.wait_event(
                VM_NOTIFY_STRING,
                channel,
                timeout=10.0,
            )
        except TimeoutError:
            print(
                "No string notification received; "
                "querying status..."
            )
            await self.status()
            raise

        print(
            f"String[{channel}]: {value}"
        )

    async def test_battery_lora(
        self,
        wait_s: int,
    ):
        code = battery_lora_test(wait_s)

        print(
            "Bytecode:",
            code.hex(" "),
        )

        await self.sync_time()

        await self.upload(
            code,
            persist=True,
        )

        await self.run_loaded()

        for _ in range(60):
            await asyncio.sleep(0.5)

            st = await self.status()

            if st["state"] in (6, 7):
                break


    async def state_save(self, key: int, value: int):
        code = state_save_test(key, value)

        await self.upload(
            code,
            persist=True,
            schedule=schedule_manual(),
        )

        await self.run_loaded()

        # Let the asynchronous flash state journal operation complete.
        # PERSIST_SAVE is intentionally rare/explicit.
        for _ in range(80):
            await asyncio.sleep(0.1)
            st = await self.status(quiet=True)
            if st["state"] in (6, 7):
                break

        print(
            f"Persistent state[{key}] saved = {value & 0xFFFFFFFF} "
            f"(0x{value & 0xFFFFFFFF:08X})"
        )

    async def state_load(self, key: int):
        channel = 3
        code = state_load_notify_test(key, channel)

        await self.upload(
            code,
            persist=True,
            schedule=schedule_manual(),
        )

        await self.run_loaded()

        value = await self.wait_event(
            VM_NOTIFY_U32,
            channel,
            timeout=10.0,
        )

        print(
            f"Persistent state[{key}] = {value} "
            f"(0x{value:08X})"
        )

    async def state_get(self, key: int):
        channel = 4
        code = state_get_notify_test(key, channel)

        await self.upload(
            code,
            persist=True,
            schedule=schedule_manual(),
        )

        await self.run_loaded()

        value = await self.wait_event(
            VM_NOTIFY_U32,
            channel,
            timeout=10.0,
        )

        print(
            f"RAM state[{key}] = {value} "
            f"(0x{value:08X})"
        )

    async def test_app_echo(self, interval: int, payload: str):
        code = app_echo_boot_test(interval, payload)

        print(
            "Bytecode:",
            code.hex(" "),
        )

        await self.upload(
            code,
            persist=True,
            schedule={"mode": 4, "dow_mask": 0, "arg0": 0, "arg1": 0},
        )

        print(
            "Stage-8 BLE Application echo program persisted as BOOT."
        )
        print(
            "Run 'reset' to leave NUS programming and start Application BLE."
        )


    async def factory_reset(self):
        await self.command(CMD_FACTORY_RESET, timeout=5.0)
        print("Factory reset accepted; user program/state will be erased and device rebooted.")

    async def test_tracking(
        self,
        key_hex: str,
        interval_ms: int,
        tx_power_dbm: int,
    ):
        code = tracking_test_program(
            key_hex,
            interval_ms,
            tx_power_dbm,
        )

        print("Tracking key:", key_hex.lower())
        print("Bytecode:", code.hex(" "))

        await self.upload(
            code,
            persist=True,
            schedule=schedule_boot(),
        )

        print(
            "Stage 8.2 experimental TRACKING program persisted as BOOT. "
            "Run 'reset' so NUS releases the BLE advertising resource; "
            "tracking will start autonomously on the next boot."
        )


    async def tracking_stop(self):
        code = tracking_stop_program()

        print("Bytecode:", code.hex(" "))

        await self.upload(
            code,
            persist=True,
            schedule=schedule_manual(),
        )

        await self.run_loaded()

        print("TRACKING_STOP executed.")



    async def tracking_enable(
        self,
        interval_ms: int,
        tx_power_dbm: int,
        rotation_seconds: int,
    ):
        code = tracking_enable_program(
            interval_ms=interval_ms,
            tx_power_dbm=tx_power_dbm,
            rotation_seconds=rotation_seconds,
        )

        print("Bytecode:", code.hex(" "))

        await self.upload(
            code,
            persist=True,
            schedule=schedule_boot(),
        )

        print(
            "Stage 8.2 autonomous TRACKING persisted as BOOT. "
            "No external key file is required."
        )
        print(
            "Run 'reset' to close NUS and start TRACKING."
        )


    async def tracking_info(self):
        p = await self.command(
            CMD_TRACKING_INFO,
            timeout=5.0,
        )

        if len(p) != 16:
            raise RuntimeError(
                f"Invalid TRACKING_INFO response length: {len(p)}"
            )

        identity_valid = bool(p[0])
        active = bool(p[1])
        active_slot = p[2]

        key_index = int.from_bytes(
            p[3:7],
            "little",
        )

        rotation_seconds = int.from_bytes(
            p[7:11],
            "little",
        )

        adv_interval_ms = int.from_bytes(
            p[11:13],
            "little",
        )

        tx_power_dbm = int.from_bytes(
            p[13:14],
            "little",
            signed=True,
        )

        generation = int.from_bytes(
            p[14:16],
            "little",
        )

        print(
            f"Identity valid : {'yes' if identity_valid else 'no'}"
        )
        print(
            f"Active         : {'yes' if active else 'no'}"
        )
        print(
            "Active slot    : "
            f"{'none' if active_slot == 0xFF else active_slot}"
        )
        print(f"Key index      : {key_index}")
        print(f"Rotation       : {rotation_seconds} s")
        print(f"ADV interval   : {adv_interval_ms} ms")
        print(f"TX power       : {tx_power_dbm} dBm")
        print(f"Generation     : {generation}")


    async def tracking_identity_seed(self):
        seed = bytearray(32)

        for offset in (0, 16):
            p = await self.command(
                CMD_TRACKING_IDENTITY,
                bytes([offset]),
                timeout=5.0,
            )

            if len(p) != 17:
                raise RuntimeError(
                    "Invalid TRACKING_IDENTITY response length"
                )

            if p[0] != offset:
                raise RuntimeError(
                    "Invalid TRACKING_IDENTITY offset"
                )

            seed[offset:offset + 16] = p[1:17]

        return bytes(seed)

    async def tracking_identity(self):
        seed = await self.tracking_identity_seed()
        seed_hex = seed.hex()

        print(f"Tracking identity seed: {seed_hex}")
        print(
            "Identity URI: "
            f"nrfclaw-oh1:{seed_hex}"
        )
        print(
            "Keep this identity secret: it can derive the "
            "OpenHaystack private-key sequence."
        )

    async def tracking_provision(
        self,
        device_name,
        name=None,
        output=None,
        keygen_url=TRACKING_KEYGEN_URL,
        no_qr=False,
    ):
        info = await self.command(CMD_TRACKING_INFO, timeout=5.0)
        if len(info) != 16:
            raise RuntimeError(
                f"Invalid TRACKING_INFO response length: {len(info)}"
            )

        identity_valid = bool(info[0])
        current_index = int.from_bytes(info[3:7], "little")
        rotation_seconds = int.from_bytes(info[7:11], "little")
        if not identity_valid:
            raise RuntimeError(
                "tracking identity is not initialized; run tracking-enable first"
            )
        if rotation_seconds == 0:
            raise RuntimeError(
                "tracking rotation is zero; configure tracking before provisioning"
            )

        seed = await self.tracking_identity_seed()

        if name:
            provision_name = name.strip()
        else:
            suffix = str(device_name or "").strip()
            if not suffix:
                provision_name = "nRFClaw-Tracking"
            elif suffix.lower().startswith("nrfclaw-"):
                provision_name = suffix
            else:
                provision_name = f"nRFClaw-{suffix}"

        url = build_tracking_provision_url(
            seed=seed,
            device_name=provision_name,
            current_index=current_index,
            rotation_seconds=rotation_seconds,
            base_url=keygen_url,
        )

        qr_path = None
        qr_warning = None

        print("=== nRFClaw Tracking Provisioning ===")
        print(f"Device       : {provision_name}")
        print(f"KDF          : {TRACKING_KDF_LABEL.decode('ascii')}")
        print(f"Current index: {current_index}")
        print(f"Rotation     : {rotation_seconds} s")
        if not no_qr:
            print("Scan this QR code:")
            print(render_tracking_provision_qr_unicode(url))
        print("Provisioning URL:")
        print(url)

        # PNG is opt-in. --output is kept as a legacy alias for --qr-png.
        if output:
            try:
                qr_path = write_tracking_provision_qr(url, Path(output))
            except RuntimeError as exc:
                qr_warning = str(exc)
        if qr_path is not None:
            print(f"QR PNG       : {qr_path}")
        elif qr_warning:
            print(f"QR PNG       : not generated ({qr_warning})")
        print(
            "SECRET: the provisioning URL/QR contains the tracking master "
            "seed and can derive the private-key sequence."
        )
        print(
            "The seed is stored after '#', so it is not included in the "
            "normal HTTP request to the keygen server."
        )
        return url, qr_path


    async def tracking_export(self, count, start_index, name, output):
        if count < 1 or count > TRACKING_MAX_EXPORT_KEYS:
            raise ValueError(
                f"--count must be 1..{TRACKING_MAX_EXPORT_KEYS}"
            )

        info = await self.command(CMD_TRACKING_INFO, timeout=5.0)
        if len(info) != 16:
            raise RuntimeError(
                f"Invalid TRACKING_INFO response length: {len(info)}"
            )
        current_index = int.from_bytes(info[3:7], "little")
        rotation_seconds = int.from_bytes(info[7:11], "little")
        identity_valid = bool(info[0])
        if not identity_valid:
            raise RuntimeError(
                "tracking identity is not initialized; run tracking-enable first"
            )

        if start_index is None:
            start_index = current_index
        if start_index < 0 or start_index > 0xFFFFFFFF:
            raise ValueError("--start-index must be 0..4294967295")
        if start_index + count - 1 > 0xFFFFFFFF:
            raise ValueError("requested key range exceeds uint32 tracking index")

        seed = await self.tracking_identity_seed()
        rows = []
        for index in range(start_index, start_index + count):
            private_key, adv_key, retry = derive_tracking_keypair(seed, index)
            hashed = hashlib.sha256(adv_key).digest()
            rows.append(
                {
                    "index": index,
                    "retry": retry,
                    "private_b64": base64.b64encode(private_key).decode("ascii"),
                    "adv_b64": base64.b64encode(adv_key).decode("ascii"),
                    "hashed_adv_b64": base64.b64encode(hashed).decode("ascii"),
                    "mac": tracking_mac_from_adv_key(adv_key),
                }
            )

        device_name = name or "nRFClaw-Tracking"
        output_path = Path(output or f"{device_name}_devices.json")
        output_path.write_text(
            json.dumps(build_macless_devices_json(device_name, rows), indent=2)
            + "\n",
            encoding="utf-8",
        )

        manifest_path = output_path.with_name(
            output_path.stem.replace("_devices", "") + "_tracking_keys.csv"
        )
        with manifest_path.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.writer(fh)
            writer.writerow(
                ["index", "retry", "advertisement_key_b64", "hashed_adv_key_b64", "ble_address"]
            )
            for row in rows:
                writer.writerow(
                    [
                        row["index"],
                        row["retry"],
                        row["adv_b64"],
                        row["hashed_adv_b64"],
                        row["mac"],
                    ]
                )

        coverage = count * rotation_seconds
        print("=== nRFClaw -> Macless-Haystack export ===")
        print(f"Device name      : {device_name}")
        print(f"Current key index: {current_index}")
        print(f"Exported indexes : {start_index}..{start_index + count - 1}")
        print(f"Rotation         : {rotation_seconds} s")
        print(f"Coverage         : {coverage} s ({coverage / 86400.0:.2f} days)")
        print(f"Macless JSON     : {output_path}")
        print(f"Public manifest  : {manifest_path}")
        print(
            "Private tracking keys are contained in the Macless JSON. "
            "Keep that file secret."
        )

    async def tracking_debug(self):
        p = await self.command(CMD_TRACKING_DEBUG, timeout=5.0)

        if len(p) != 16:
            raise RuntimeError(
                f"Invalid TRACKING_DEBUG response length: {len(p)}"
            )

        stages = {
            0: "NONE",
            1: "START_REQUESTED",
            2: "JOURNAL_PENDING",
            3: "INDEX_COMMITTED",
            4: "KEY_DERIVED",
            5: "ADV_STOP",
            6: "ADDR_SET",
            7: "ADV_CONFIGURE",
            8: "TX_POWER",
            9: "ADV_START",
            10: "ACTIVE",
        }

        key_index = int.from_bytes(p[8:12], "little")
        sd_error = int.from_bytes(p[12:16], "little")

        print(f"Stage                : {stages.get(p[0], p[0])}")
        print(f"start_pending        : {bool(p[1])}")
        print(f"index_commit_pending : {bool(p[2])}")
        print(f"apply_pending        : {bool(p[3])}")
        print(f"active               : {bool(p[4])}")
        print(f"commit_state         : {p[5]}")
        print(f"journal_state        : {p[6]}")
        print(f"key_index            : {key_index}")
        print(f"last_sd_error        : 0x{sd_error:08X}")




    async def vib_model_set(
        self, rms: int, rms_tol: int, peak: int, peak_tol: int,
        p2p: int, p2p_tol: int, zero_cross: int, zero_cross_tol: int,
        key: bytes,
    ):
        values=(rms,rms_tol,peak,peak_tol,p2p,p2p_tol,zero_cross,zero_cross_tol)
        if any(v < 0 or v > 0xFFFF for v in values):
            raise ValueError("all vibration model values must be 0..65535")
        if any(v == 0 for v in (rms_tol,peak_tol,p2p_tol,zero_cross_tol)):
            raise ValueError("tolerances must be > 0")
        await self.ndp_auth_control(key)
        await self.ndp_command(NDP_VIB_MODEL_SET, struct.pack("<8H", *values))
        print("=== EXPERIMENTAL VIBRATION MODEL ===")
        print(f"RMS:        {rms} +/- {rms_tol} mg")
        print(f"Peak:       {peak} +/- {peak_tol} mg")
        print(f"P2P:        {p2p} +/- {p2p_tol} mg")
        print(f"Zero-cross: {zero_cross} +/- {zero_cross_tol} Hz")
        print("Warning >= 2.0; Alarm >= 4.0 normalized score")

    async def vib_health(self, timeout: float = 3.0):
        # r1d is asynchronous by design: the first request starts one short
        # FIFO acquisition and returns BUSY. Poll until INT2 has produced the
        # metrics; no continuous streaming is used.
        loop=asyncio.get_running_loop()
        deadline=loop.time()+timeout
        p=None
        while True:
            status, body = await self.ndp_command_raw(NDP_VIB_HEALTH)
            if status == 0:
                p=body
                break
            if status != 4:  # NDP_BUSY
                raise RuntimeError(
                    f"NDP 0x{NDP_VIB_HEALTH:02X}: "
                    f"{NDP_STATUS_NAMES.get(status, hex(status))}"
                )
            if loop.time() >= deadline:
                raise TimeoutError(
                    "VIB_HEALTH acquisition timed out waiting for LIS2DH12 FIFO/INT2"
                )
            await asyncio.sleep(0.20)

        if len(p) != 12:
            raise RuntimeError(f"Invalid VIB_HEALTH length: {len(p)}")
        valid,state,score,rms,peak,p2p,zc=struct.unpack("<BBHHHHH",p)
        names={0:"UNKNOWN",1:"NORMAL",2:"WARNING",3:"ALARM"}
        print("=== EXPERIMENTAL VIBRATION HEALTH ===")
        print(f"Model:      {'loaded' if valid else 'not loaded'}")
        print(f"State:      {names.get(state,state)}")
        print(f"Score:      {score/256.0:.2f}")
        print(f"RMS:        {rms} mg")
        print(f"Peak:       {peak} mg")
        print(f"P2P:        {p2p} mg")
        print(f"Zero-cross: {zc} Hz")
        return {"state":state,"score":score/256.0,"rms_mg":rms,"peak_mg":peak,"p2p_mg":p2p,"zero_cross_hz":zc}



    async def vib_auto_start(self, relearn: bool, key: bytes):
        await self.ndp_auth_control(key)
        await self.ndp_command(NDP_VIB_AUTO_START, bytes([1 if relearn else 0]))
        print("VIB_AUTO r2 started" + (" with fresh discovery" if relearn else ""))

    async def vib_auto_stop(self, key: bytes):
        await self.ndp_auth_control(key)
        await self.ndp_command(NDP_VIB_AUTO_STOP)
        print("VIB_AUTO stopped")

    async def vib_auto_reset(self, key: bytes):
        await self.ndp_auth_control(key)
        await self.ndp_command(NDP_VIB_AUTO_RESET)
        print("VIB_AUTO profiles cleared; next machine activity starts a new discovery window")

    async def _vib_auto_get_config(self):
        a = await self.ndp_command(NDP_VIB_AUTO_CONFIG, b"\x80")
        b = await self.ndp_command(NDP_VIB_AUTO_CONFIG, b"\x81")
        if len(a) != 15 or len(b) != 13:
            raise RuntimeError(f"Invalid VIB_AUTO config response: {len(a)}/{len(b)}")
        learning, discovery, normal, suspicious, wake_thr, wake_dur, confirm = struct.unpack("<IHHHHHB", a)
        off_rms, match_q8, adapt_q8 = struct.unpack_from("<HHH", b, 0)
        cand, alarm, maxp, sens, ashift, stable_after, maxmul = struct.unpack_from("<BBBBBBB", b, 6)
        # r3.8.8 page 0x82 carries the canonical 32-bit installation delay.
        # Fall back to the legacy byte when talking to an older firmware.
        arming = confirm
        try:
            x = await self.ndp_command(NDP_VIB_AUTO_CONFIG, b"\x82")
            if len(x) == 4:
                arming = struct.unpack("<I", x)[0]
        except Exception:
            pass
        return {
            "learning_time": learning, "discovery_interval": discovery,
            "normal_interval": normal, "suspicious_interval": suspicious,
            "wake_threshold": wake_thr, "wake_duration": wake_dur,
            "confirm_delay": confirm, "arming_delay": arming, "off_rms": off_rms,
            "profile_match": match_q8, "adapt_limit": adapt_q8,
            "candidate_confirmations": cand, "alarm_consecutive": alarm,
            "max_profiles": maxp, "sensitivity": sens,
            "adaptation_shift": ashift, "stable_expand_after": stable_after,
            "max_interval_multiplier": maxmul,
        }

    async def vib_auto_config(self, key: bytes, **overrides):
        c = await self._vib_auto_get_config()
        # r3.8.8: arming_delay is canonical (uint32). --confirm-delay remains
        # a legacy alias when --arming-delay is not explicitly supplied.
        if overrides.get("arming_delay") is None and overrides.get("confirm_delay") is not None:
            overrides["arming_delay"] = overrides["confirm_delay"]
        for k, v in overrides.items():
            if v is not None:
                c[k] = v
        if not 0 <= int(c["arming_delay"]) <= 604800:
            raise RuntimeError("--arming-delay must be between 0 and 604800 seconds (7 days)")
        c["confirm_delay"] = min(int(c["arming_delay"]), 255)
        sens = c["sensitivity"]
        if isinstance(sens, str):
            sens = {"low": 0, "normal": 1, "high": 2}[sens]
            c["sensitivity"] = sens
        p0 = struct.pack("<BIHHHHHB", 0, c["learning_time"], c["discovery_interval"],
                         c["normal_interval"], c["suspicious_interval"],
                         c["wake_threshold"], c["wake_duration"], c["confirm_delay"])
        p1 = struct.pack("<BHHHBBBBBBB", 1, c["off_rms"], c["profile_match"], c["adapt_limit"],
                         c["candidate_confirmations"], c["alarm_consecutive"], c["max_profiles"],
                         c["sensitivity"], c["adaptation_shift"], c["stable_expand_after"],
                         c["max_interval_multiplier"])
        p2 = struct.pack("<BI", 2, int(c["arming_delay"]))
        await self.ndp_auth_control(key)
        await self.ndp_command(NDP_VIB_AUTO_CONFIG, p0)
        await self.ndp_command(NDP_VIB_AUTO_CONFIG, p1)
        await self.ndp_command(NDP_VIB_AUTO_CONFIG, p2)
        print("=== EXPERIMENTAL VIB_AUTO r3.8.12 CONFIG ===")
        print(f"Discovery time:       {format_duration(c['learning_time'])}")
        print(f"Active discovery:     {c['discovery_interval']} s between windows")
        print(f"Normal sample base:   {c['normal_interval']} s")
        print(f"Suspicious recheck:   {c['suspicious_interval']} s")
        print(f"Arming delay:         {c['arming_delay']} s")
        print(f"Wake INT1:            {c['wake_threshold']} mg / {c['wake_duration']} ms")
        print(f"Profiles:             auto, max {c['max_profiles']}")
        print(f"New-profile confirms: {c['candidate_confirmations']}")
        print(f"Sensitivity:          {['low','normal','high'][c['sensitivity']]}")
        print(f"Aging adapt limit:    {c['adapt_limit']/256.0:.2f}")
        print(f"Aging alpha:          ~1/{1 << c['adaptation_shift']}")
        print(f"Stable interval max:  {c['max_interval_multiplier']}x")

    async def vib_auto_status(self):
        p = await self.ndp_command(NDP_VIB_AUTO_STATUS)
        q = await self.ndp_command(NDP_VIB_AUTO_STATUS, b"\x01")
        if len(p) != 15 or len(q) != 13:
            raise RuntimeError(f"Invalid VIB_AUTO_STATUS length: {len(p)}/{len(q)}")
        enabled, complete, state, profiles, candidate, best, bad = struct.unpack_from("<BBBBBBB", p, 0)
        score, rms, peak, next_s = struct.unpack_from("<HHHH", p, 7)
        remaining, p2p, zc, stable, quiet = struct.unpack_from("<IHHHH", q, 0)
        store_busy = q[12]
        names={0:"DISABLED",1:"WAIT_MACHINE",2:"DISCOVERY",3:"MONITORING",4:"SUSPICIOUS",5:"ALARM",6:"ERROR",7:"ARMING",8:"ACTIVE_CONFIRM"}
        print("=== EXPERIMENTAL VIB_AUTO r2 ===")
        print(f"Enabled:             {'yes' if enabled else 'no'}")
        print(f"Discovery:           {'complete' if complete else 'learning'}")
        print(f"State:               {names.get(state,state)}")
        print(f"Profiles discovered: {profiles}")
        print(f"Candidate windows:   {candidate}")
        print(f"Best profile:        {'none' if best == 0xFF else best}")
        print(f"Bad streak:          {bad}")
        print(f"Stable streak:       {stable}")
        print(f"Last score:          {score/256.0:.2f}")
        print(f"Last RMS/Peak/P2P:   {rms}/{peak}/{p2p} mg")
        print(f"Last Zero-cross:     {zc} Hz")
        print(f"Learn remaining:     {format_duration(remaining) if remaining else '0s'}")
        print(f"Quiet RMS learned:   {quiet} mg")
        print(f"Next sample:         {next_s} s")
        print(f"Profile store:       {'busy' if store_busy else 'ready'}")

    async def vib_auto_diagnostics(self):
        pages = [await self.ndp_command(NDP_VIB_AUTO_STATUS, bytes([page])) for page in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 32, 33)]
        p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, p32, p33 = pages
        if tuple(map(len,pages)) != (14,12,15,10,14,14,12,14,12,11,14,11,14,15,15):
            raise RuntimeError("VIB_AUTO diagnostics unavailable or invalid: " + "/".join(str(len(x)) for x in pages))
        total, quiet, active, starts, matches, resets, pmatches = struct.unpack("<HHHHHHH", p2)
        rrms, rpeak, rp2p, rzc, last_score = struct.unpack_from("<HHHHH", p3, 0)
        last_feature, reject_mask = p3[10], p3[11]
        valid = bool(p4[0])
        means = struct.unpack_from("<HHHH", p4, 1)
        tol3 = struct.unpack_from("<HHH", p4, 9)
        ztol = struct.unpack_from("<H", p5, 0)[0]
        fs = struct.unpack_from("<HHHH", p5, 2)
        names = {0:"none", 1:"RMS", 2:"Peak", 3:"P2P", 4:"Zero-cross"}
        bits = ["RMS", "Peak", "P2P", "Zero-cross"]
        mask_names = [bits[i] for i in range(4) if reject_mask & (1 << i)]
        print("=== VIB_AUTO DISCOVERY DIAGNOSTICS ===")
        print(f"Windows sampled:      {total} (quiet={quiet}, active={active})")
        print(f"Candidate starts:     {starts}")
        print(f"Candidate matches:    {matches}")
        print(f"Candidate resets:     {resets}")
        print(f"Existing-profile hits:{pmatches}")
        if resets:
            print(f"Reset dominant counts:RMS={rrms} Peak={rpeak} P2P={rp2p} ZC={rzc}")
            print(f"Last reject dominant: {names.get(last_feature,last_feature)}")
            print(f"Last reject mask:     {', '.join(mask_names) if mask_names else 'combined-score only'}")
            print(f"Last candidate score: {last_score/256.0:.2f}")
            print("Last feature scores:  " + " ".join(f"{n}={v/256.0:.2f}" for n,v in zip(bits,fs)))
        if valid:
            tols = (tol3[0], tol3[1], tol3[2], ztol)
            print(f"Current candidate:    RMS={means[0]} Peak={means[1]} P2P={means[2]} mg ZC={means[3]} Hz")
            print(f"Discovery tolerance:  RMS=+/-{tols[0]} Peak=+/-{tols[1]} P2P=+/-{tols[2]} mg ZC=+/-{tols[3]} Hz")
        else:
            print("Current candidate:    none")
        if total:
            print(f"Active ratio:         {100.0*active/total:.1f}%")
        if active:
            print(f"Reset/active ratio:   {100.0*resets/active:.1f}%")

        timer_fires, discovery_due, probe_req, motion_rel, win_ok, win_fail, metrics_ready = struct.unpack("<HHHHHHH", p6)
        probe_active, stage = p7[0], p7[1]
        probe_age = struct.unpack_from("<H", p7, 2)[0]
        purpose, accel_mode = p7[4], p7[5]
        svc_calls, fifo_empty, captures, capture_ok = struct.unpack_from("<HHHH", p7, 6)
        read_fail, write_fail, sample_fail, int2_isr, int2_evt = struct.unpack_from("<HHHHH", p8, 0)
        fifo_src, fifo_count = p8[10], p8[11]
        stages={0:"IDLE",1:"TIMER_DUE",2:"MOTION_RELEASED",3:"CONFIGURING",4:"FIFO_WAIT",5:"METRICS_READY",6:"CLASSIFIED",7:"ERROR"}
        purposes={0:"NONE",1:"DISCOVERY",2:"MONITOR",3:"ARMING"}
        modes={0:"OFF",1:"MOTION",2:"TAP",3:"FALL",4:"WALK",5:"VIBRATION"}
        print("\n=== VIB_AUTO PROBE PIPELINE ===")
        print(f"Timer expirations:    {timer_fires}")
        print(f"Discovery due:        {discovery_due}")
        print(f"Probe requests:       {probe_req}")
        print(f"Motion releases:      {motion_rel}")
        print(f"Window config OK:     {win_ok}")
        print(f"Window config FAIL:   {win_fail}")
        print(f"Metrics ready:        {metrics_ready}")
        print(f"Current probe:        {'ACTIVE' if probe_active else 'idle'} stage={stages.get(stage,stage)} age={probe_age}s")
        print(f"Purpose/accel mode:   {purposes.get(purpose,purpose)} / {modes.get(accel_mode,accel_mode)}")
        print("\n=== LIS2DH12 PIPELINE ===")
        print(f"Vibration services:   {svc_calls}")
        print(f"FIFO empty polls:     {fifo_empty}")
        print(f"Capture attempts/OK:  {captures}/{capture_ok}")
        print(f"TWI read/write fail:  {read_fail}/{write_fail}")
        print(f"Sample read failures: {sample_fail}")
        print(f"INT2 ISR/events:      {int2_isr}/{int2_evt}")
        print(f"Last FIFO src/count:  0x{fifo_src:02X}/{fifo_count}")
        if probe_req:
            print(f"VIB_AUTO completion:  {100.0*metrics_ready/probe_req:.1f}% ({metrics_ready}/{probe_req})")

        sched, stops, start_ok, start_fail, motion_evt, motion_ok, motion_ignored = struct.unpack("<HHHHHHH", p9)
        arm_calls, arm_ok, arm_fail, last_sched = struct.unpack_from("<HHHH", p10, 0)
        last_purpose, last_trigger = p10[8], p10[9]
        int1_isr = struct.unpack_from("<H", p10, 10)[0]
        trigger_names={0:"NONE",1:"TIMER",2:"INT1"}
        print("\n=== VIB_AUTO SCHEDULER / INT1 ===")
        print(f"Schedule calls:       {sched}")
        print(f"Timer stop calls:     {stops}")
        print(f"Timer start OK/FAIL:  {start_ok}/{start_fail}")
        print(f"INT1 ISR:             {int1_isr}")
        print(f"Motion events:        {motion_evt}")
        print(f"Motion accepted:      {motion_ok}")
        print(f"Motion ignored state: {motion_ignored}")
        print(f"Arm wake calls OK/FAIL:{arm_calls} {arm_ok}/{arm_fail}")
        print(f"Last scheduled:       {last_sched}s {purposes.get(last_purpose,last_purpose)}")
        print(f"Last sample trigger:  {trigger_names.get(last_trigger,last_trigger)}")
        arming_starts, arming_done, ignored_arm, int_first, arm_rem = struct.unpack_from("<HHHHH", p11, 0)
        arm_active = bool(p11[10])
        print("\n=== VIB_AUTO INT-FIRST / ARMING ===")
        print(f"Arming active:        {'yes' if arm_active else 'no'}")
        print(f"Arming remaining:     {arm_rem}s")
        print(f"Arming starts/done:   {arming_starts}/{arming_done}")
        print(f"Motion ignored arming:{ignored_arm}")
        print(f"INT-first windows:    {int_first}")

        snap_count, snap_fail = struct.unpack_from("<HH", p12, 0)
        ctrl1,ctrl2,ctrl3,ctrl4,ctrl5,ctrl6,fifo_ctrl,int1_cfg,int1_ths,int1_dur = p12[4:14]
        twi_en,int1_lvl,int2_lvl,port_evt,int1_latch,int2_latch,int1_sense,int2_sense,pwr_mode,scl_lvl,sda_lvl = p13
        print("\n=== LIS2DH12 LOW-POWER RETURN ===")
        print(f"Motion snapshots OK/FAIL:{snap_count}/{snap_fail}")
        print(f"CTRL1..6:            {ctrl1:02X} {ctrl2:02X} {ctrl3:02X} {ctrl4:02X} {ctrl5:02X} {ctrl6:02X}")
        print(f"FIFO/INT1 CFG:       {fifo_ctrl:02X} / {int1_cfg:02X}")
        print(f"INT1 THS/DURATION:   {int1_ths:02X} / {int1_dur:02X}")
        print(f"TWI enabled:         {'yes' if twi_en else 'no'}")
        print(f"GPIO INT1/INT2:      {int1_lvl}/{int2_lvl}")
        print(f"GPIOTE PORT event:   {port_evt}")
        print(f"GPIO LATCH INT1/2:   {int1_latch}/{int2_latch}")
        print(f"GPIO SENSE INT1/2:   {int1_sense}/{int2_sense}")
        print(f"SCL/SDA level:       {scl_lvl}/{sda_lvl}")
        print(f"Driver accel mode:   {modes.get(pwr_mode,pwr_mode)}")
        expected = (ctrl1 == 0x2F and ctrl2 == 0x01 and ctrl3 == 0x40 and ctrl4 == 0x00 and ctrl5 == 0x00 and ctrl6 == 0x00 and fifo_ctrl == 0x00 and int1_cfg == 0x2A)
        print(f"Motion register set: {'EXPECTED' if expected else 'DIFFERS'}")

        twi_rel = struct.unpack_from("<H", p14, 0)[0]
        cur_pscl,cur_psda,cur_err,cur_hf,last_pscl,last_psda,last_err,last_hf,cur_scl_cnf,cur_sda_cnf,last_scl_cnf,last_sda_cnf = p14[2:14]
        print("\n=== TWI GLOBAL LOW-POWER CLEANUP ===")
        print(f"Low-power releases:  {twi_rel}")
        print(f"PSEL SCL/SDA disc:   {cur_pscl}/{cur_psda} (last cleanup {last_pscl}/{last_psda})")
        print(f"ERRORSRC current/last:{cur_err}/{last_err}")
        print(f"HFCLK current/last:  {cur_hf}/{last_hf}")
        print(f"PIN_CNF SCL/SDA:     0x{cur_scl_cnf:02X}/0x{cur_sda_cnf:02X} (last 0x{last_scl_cnf:02X}/0x{last_sda_cnf:02X})")
        print(f"TWI cleanup state:   {'EXPECTED' if (not twi_en and cur_pscl and cur_psda) else 'CHECK'}")

        wd_arms,wd_exp,wd_retry,wd_abort,wd_int2,wd_timer,wd_ms = struct.unpack_from("<HHHHHHH", p32, 0)
        wd_src = p32[14]
        wd_names={0:"NONE",1:"INT2",2:"TIMER",3:"ABORT"}
        print("\n=== FIFO AUTONOMOUS WATCHDOG r3.8.11 ===")
        print(f"Watchdog arms:        {wd_arms}")
        print(f"Expirations/retries:  {wd_exp}/{wd_retry}")
        print(f"INT2 completions:     {wd_int2}")
        print(f"Timer completions:    {wd_timer}")
        print(f"Watchdog aborts:      {wd_abort}")
        print(f"Last active estimate: {wd_ms} ms")
        print(f"Last completion:      {wd_names.get(wd_src,wd_src)}")
        confirm_starts, confirm_ok, confirm_rejects, confirm_delay = struct.unpack_from("<HHHH", p33, 0)
        confirm_pending = bool(p33[8])
        confirm_retries = confirm_retry_ok = retry_delay_ms = 0
        if len(p33) >= 15:
            confirm_retries, confirm_retry_ok, retry_delay_ms = struct.unpack_from("<HHH", p33, 9)
        print("\n=== ROBUST ACTIVE CONFIRMATION r3.8.13 ===")
        print(f"Confirm starts:       {confirm_starts}")
        print(f"Confirmed active:     {confirm_ok}")
        print(f"Rejected transient:   {confirm_rejects}")
        print(f"Confirm retries:      {confirm_retries}")
        print(f"Confirmed on retry:   {confirm_retry_ok}")
        print(f"Confirm delay:        {confirm_delay}s")
        print(f"Retry delay:          {retry_delay_ms}ms")
        print(f"Confirm pending:      {'yes' if confirm_pending else 'no'}")

    async def vib_auto_trace(self):
        summary = await self.ndp_command(NDP_VIB_AUTO_STATUS, b"\x0f")
        if len(summary) != 2:
            raise RuntimeError("VIB_AUTO trace unavailable")
        count, last_reason = summary
        events={1:"ARMING_DONE",2:"ARM_WAKE",3:"TIMER_SCHEDULE",4:"TIMER_DUE",5:"MOTION_ACCEPT",6:"MOTION_IGNORED",7:"INT_DISABLE",8:"FIFO_BEGIN",9:"FIFO_CONFIG_OK",10:"METRICS_ACTIVE",11:"METRICS_QUIET",12:"DISCOVERY_SCHEDULE",13:"FIFO_WATCHDOG_FAIL",14:"CONFIRM_SCHEDULE",15:"CONFIRM_OK",16:"CONFIRM_REJECT",17:"CONFIRM_RETRY",18:"CONFIRM_RETRY_OK"}
        states={0:"DISABLED",1:"WAIT_MACHINE",2:"DISCOVERY",3:"MONITORING",4:"SUSPICIOUS",5:"ALARM",6:"ERROR",7:"ARMING",8:"ACTIVE_CONFIRM"}
        purposes={0:"NONE",1:"DISCOVERY",2:"MONITOR",3:"ARMING"}
        modes={0:"OFF",1:"MOTION",2:"TAP",3:"FALL",4:"WALK",5:"VIBRATION"}
        reasons={0:"NONE",1:"ARMING_DONE",2:"DISCOVERY_QUIET",3:"MONITOR_QUIET",4:"DEADLINE_WAIT",5:"START_NO_DELAY",6:"CONFIG_REAPPLY",7:"RESET_LEARNING",8:"CONFIRM_REJECT"}
        print("=== VIB_AUTO STATE TRACE r3.8.14 ===")
        print(f"Entries:              {count}/16")
        print(f"Last arm reason:      {reasons.get(last_reason,last_reason)}")
        print("idx  time(s)     event               state         purpose     accel       arm-reason")
        for i in range(count):
            q=await self.ndp_command(NDP_VIB_AUTO_STATUS, bytes([16+i]))
            if len(q)!=9: continue
            ts=struct.unpack_from("<I",q,0)[0]; ev,state,purpose,mode,reason=q[4:9]
            print(f"{i:>3}  {ts:<10} {events.get(ev,ev):<19} {states.get(state,state):<13} {purposes.get(purpose,purpose):<11} {modes.get(mode,mode):<11} {reasons.get(reason,reason)}")

    async def ble_boot_control(self, action: str):
        op={"status":0,"disable":1,"enable":2}[action]
        p=await self.ndp_command(NDP_BLE_BOOT_CONTROL, bytes([op]))
        if len(p)!=2:
            raise RuntimeError("Invalid BLE boot-control response")
        enabled, store_state=p
        states={0:"IDLE",1:"SAVING",2:"ERROR"}
        if action != "status":
            # Let the asynchronous SoftDevice flash journal commit before the
            # user resets. Poll through NUS; this does not affect next-boot mode.
            deadline=time.monotonic()+5.0
            while store_state==1 and time.monotonic()<deadline:
                await asyncio.sleep(0.1)
                p=await self.ndp_command(NDP_BLE_BOOT_CONTROL,b"\x00")
                enabled,store_state=p
        print("=== BLE BOOT CONTROL r3.8.10 ===")
        print(f"Application/NDP boot: {'ENABLED' if enabled else 'DISABLED'}")
        print(f"State journal:        {states.get(store_state,store_state)}")
        print("P0.21/NUS:            ALWAYS AVAILABLE")
        if action != "status": print("Setting applies on next reset/boot.")

    async def vib_auto_baseline(self):
        st = await self.ndp_command(NDP_VIB_AUTO_STATUS)
        if len(st) != 15:
            raise RuntimeError("Invalid VIB_AUTO status")
        count = st[3]
        print("=== EXPERIMENTAL VIB_AUTO r2 PROFILES ===")
        if not count:
            print("No profiles learned yet")
            return
        for i in range(count):
            a = await self.ndp_command(NDP_VIB_AUTO_BASELINE, bytes([i,0]))
            b = await self.ndp_command(NDP_VIB_AUTO_BASELINE, bytes([i,1]))
            if len(a) != 11 or len(b) != 8:
                raise RuntimeError(f"Invalid profile {i} response")
            conf = a[0]; obs = struct.unpack_from("<H",a,1)[0]
            rms,rms_tol,peak,peak_tol = struct.unpack_from("<HHHH",a,3)
            p2p,p2p_tol,zc,zc_tol = struct.unpack("<HHHH",b)
            print(f"PROFILE_{i}: confidence={conf}% observations={obs}")
            print(f"  RMS        {rms} +/- {rms_tol} mg")
            print(f"  Peak       {peak} +/- {peak_tol} mg")
            print(f"  P2P        {p2p} +/- {p2p_tol} mg")
            print(f"  Zero-cross {zc} +/- {zc_tol} Hz")

def format_capability(name: str, supported: bool, active: bool) -> str:
    """
    Stage 8.2 presentation rules.

    SERIAL is a supported native capability that intentionally remains
    inactive unless bytecode explicitly owns the UART.

    TRACKING is experimental and likewise inactive by default.
    """
    if name == "SERIAL":
        return "supported, active" if active else "supported, inactive"

    if name == "TRACKING":
        return "experimental, active" if active else "experimental, inactive"

    if not supported:
        return "no"

    return "yes" if active else "yes"





async def run_ha_mode(args):
    if args.ha_action == "scan":
        await print_bleak_scan(args.scan_timeout, args.device)
        return

    if args.ha_action == "reconnect":
        count = args.count
        retries = args.connect_retries
        retry_delay = args.retry_delay
        if count < 1 or count > 1000:
            raise ValueError("--count must be 1..1000")
        if retries < 1 or retries > 10:
            raise ValueError("--connect-retries must be 1..10")
        if retry_delay < 0.0 or retry_delay > 10.0:
            raise ValueError("--retry-delay must be 0..10 seconds")

        ok = 0
        first_attempt_ok = 0
        recovered_retries = 0
        hard_failures = 0
        total_connect_attempts = 0
        started = time.monotonic()

        # R1D reconnect robustness rule: never carry a BleakDevice object from
        # one reconnect cycle to the next.  Every connection attempt starts
        # with fresh unfiltered discovery so the test validates the complete
        # advertise -> scan -> connect -> NDP -> disconnect lifecycle.
        for i in range(1, count + 1):
            cycle_ok = False
            last_exc = None

            for attempt in range(1, retries + 1):
                total_connect_attempts += 1
                try:
                    name, dev = await find_app_device(
                        args.device,
                        args.scan_timeout,
                        verbose=(attempt == retries),
                    )

                    async with ApplicationNDPClient(dev, ndp_access_key(args.ndp_key)) as app:
                        await app.ndp_command(NDP_INFO)
                        await app.ndp_command(NDP_STATUS)

                    ok += 1
                    if attempt == 1:
                        first_attempt_ok += 1
                        print(
                            f"{i:3d}/{count}: connect -> NDP -> disconnect OK"
                        )
                    else:
                        recovered_retries += 1
                        print(
                            f"{i:3d}/{count}: connect -> NDP -> disconnect OK "
                            f"(recovered {attempt}/{retries})"
                        )
                    cycle_ok = True
                    break

                except Exception as exc:
                    last_exc = exc
                    detail = str(exc) or repr(exc)
                    if attempt < retries:
                        print(
                            f"{i:3d}/{count}: connect attempt "
                            f"{attempt}/{retries} failed ({detail}); rescanning"
                        )
                        if retry_delay:
                            await asyncio.sleep(retry_delay)
                    else:
                        print(
                            f"{i:3d}/{count}: HARD FAIL after {retries} "
                            f"attempts ({detail})"
                        )

            if not cycle_ok:
                hard_failures += 1
                if not args.keep_going:
                    raise RuntimeError(
                        f"Reconnect cycle {i}/{count} failed after "
                        f"{retries} fresh-scan connect attempts: "
                        f"{str(last_exc) or repr(last_exc)}"
                    ) from last_exc

        elapsed = time.monotonic() - started
        print("=== HA RECONNECT SUMMARY ===")
        print(f"Reconnect cycles:       {ok}/{count} PASS")
        print(f"First-attempt connects: {first_attempt_ok}/{count}")
        print(f"Recovered retries:      {recovered_retries}")
        print(f"Hard failures:          {hard_failures}")
        print(f"Connect attempts:       {total_connect_attempts}")
        print(f"Elapsed:                {elapsed:.2f}s")
        if ok != count:
            raise RuntimeError(
                f"HA reconnect failed: {ok}/{count} successful, "
                f"{hard_failures} hard failures"
            )
        return

    name, dev = await find_app_device(args.device, args.scan_timeout, verbose=True)
    print(f"Connecting Application BLE: {name} ({dev.address})")
    async with ApplicationNDPClient(dev, ndp_access_key(args.ndp_key)) as app:
        if args.ha_action == "info":
            print("=== HA / APPLICATION INFO ===")
            await app.board_info()
            await app.info()
        elif args.ha_action == "device-info":
            await app.device_info(ble_name=name)
        elif args.ha_action == "status":
            print("=== HA / APPLICATION STATUS ===")
            await app.status()
        elif args.ha_action == "battery":
            print("=== HA / APPLICATION BATTERY ===")
            await app.battery()
        elif args.ha_action == "sensors":
            await app.sensors()
        elif args.ha_action == "raw":
            opcode = int(args.opcode, 0)
            if not 0x20 <= opcode <= 0xFF:
                raise ValueError("Application NDP opcode must be 0x20..0xFF")
            try:
                payload = bytes.fromhex(args.payload.replace(":", " ").replace(",", " ")) if args.payload else b""
            except ValueError as exc:
                raise ValueError("--payload must be hexadecimal bytes") from exc
            status, body = await app.ndp_command_raw(opcode, payload)
            print(f"Opcode:          0x{opcode:02X}")
            print(f"Status:          {status} ({NDP_STATUS_NAMES.get(status, 'UNKNOWN')})")
            print(f"Payload:         {body.hex(' ') if body else '<empty>'}")
            if status != 0:
                raise RuntimeError(f"NDP status {NDP_STATUS_NAMES.get(status, status)}")
        elif args.ha_action == "test":
            print("=== HA COMPATIBILITY TEST ===")
            checks = []
            async def check(label, coro):
                try:
                    value = await coro
                    checks.append((label, True, value))
                    print(f"{label:22s} OK")
                    return value
                except Exception as exc:
                    checks.append((label, False, exc))
                    print(f"{label:22s} FAIL ({exc})")
                    return None
            await check("Application GATT", app.info())
            await check("BOARD_INFO", app.board_info(quiet=True))
            await check("NDP STATUS", app.status(quiet=True))
            await check("BATTERY", app.battery(quiet=True))
            await check("SENSORS", app.sensors(quiet=True))
            failed = [x for x in checks if not x[1]]
            print()
            print(f"HA compatibility: {'PASS' if not failed else 'FAIL'}")
            if failed:
                raise RuntimeError(f"{len(failed)} HA compatibility check(s) failed")
        elif args.ha_action == "stress":
            count = args.count
            if count < 1 or count > 10000:
                raise ValueError("--count must be 1..10000")
            ok = 0
            started = time.monotonic()
            for i in range(1, count + 1):
                try:
                    await app.ndp_command(NDP_INFO)
                    await app.ndp_command(NDP_STATUS)
                    ok += 1
                    if args.verbose or i == count or i % 10 == 0:
                        print(f"{i:4d}/{count}: OK")
                except Exception as exc:
                    print(f"{i:4d}/{count}: FAIL ({exc})")
                    if not args.keep_going:
                        raise
            elapsed = time.monotonic() - started
            print(f"HA stress:        {ok}/{count} PASS in {elapsed:.2f}s")
            if ok != count:
                raise RuntimeError(f"HA stress failed: {ok}/{count} successful")
        else:
            raise RuntimeError(f"Unsupported HA action {args.ha_action}")


def _resolve_prompt_compilation(args, source):
    """R3.8.20: provider -> Semantic IR -> local deterministic compiler.

    AUTO uses a configured/reachable agent when available.  The agent may only
    emit versioned Semantic IR; local validation/lowering and the pseudo
    compiler remain the sole path to VM bytecode.  --standalone preserves the
    deterministic hybrid-v22 fallback.  An explicit semantic MISSING_CAPABILITY
    is never hidden by falling back to a parser that might silently drop it.
    """
    force_agent = bool(getattr(args, "agent", False))
    force_standalone = bool(getattr(args, "standalone", False))
    cfg = load_agent_config()

    override_provider = getattr(args, "agent_provider", None)
    override_model = getattr(args, "agent_model", None)
    if override_provider:
        pm = provider_catalog()[override_provider]
        cfg = AgentConfig(enabled=True, provider=override_provider,
                          model=override_model or pm["default_model"],
                          base_url=pm["default_base_url"], api_key_env=pm["api_key_env"],
                          timeout_s=cfg.timeout_s)
    elif override_model:
        cfg = AgentConfig(**vars(cfg)); cfg.model=override_model; cfg.enabled=True
    elif force_agent:
        cfg = AgentConfig(**vars(cfg)); cfg.enabled=True

    use_agent = not force_standalone and (force_agent or agent_external_available(cfg))
    if use_agent:
        try:
            plan, agent_meta = agent_plan_ir_with_metadata(source, cfg)
        except AgentError as exc:
            if force_agent:
                raise TextCompileError(f"Semantic agent unavailable/rejected: {exc}") from exc
            print(f"AI AGENT: unavailable/rejected ({exc}); falling back to deterministic hybrid-v{SEMANTIC_ENGINE_VERSION}")
        else:
            status = plan.get("status")
            print(f"AI AGENT: provider={cfg.provider} model={cfg.model} KB_SHA256={agent_meta.get('kb_sha256','')[:12]} attempts={agent_meta.get('attempts',1)} provider_ms={agent_meta.get('total_provider_ms',0):.0f}")
            for _a in agent_meta.get('attempt_log', []):
                _state = 'VALID' if _a.get('valid') else 'REJECTED'
                _err = f" error={_a.get('error')}" if _a.get('error') else ''
                _bad = f" invalid_action={_a.get('invalid_action')}" if _a.get('invalid_action') else ''
                print(f"  AGENT ATTEMPT {_a.get('attempt')}: {_state} {_a.get('elapsed_ms',0):.0f}ms stage={_a.get('transport_stage','?')} tokens={_a.get('total_tokens','?')} prompt={_a.get('prompt_tokens','?')} completion={_a.get('completion_tokens','?')}{_err}{_bad}")
            print(f"  AGENT TOTAL: tokens={agent_meta.get('total_tokens',0)} prompt={agent_meta.get('prompt_tokens',0)} completion={agent_meta.get('completion_tokens',0)} cost={agent_meta.get('cost',0)}")
            print(f"SEMANTIC: agent-ir-v{SEMANTIC_IR_VERSION} VM_ABI={SEMANTIC_IR_VM_ABI} status={status}")
            if status != "COMPILE":
                details = plan.get("missing_capabilities") or plan.get("unresolved_clauses") or plan.get("notes") or []
                detail = "; ".join(details) if details else "no additional detail"
                raise TextCompileError(f"Semantic IR {status}: {detail}")
            try:
                result, pseudo = compile_semantic_ir(plan, source=source)
            except SemanticIRMissingCapability as exc:
                raise TextCompileError(f"Semantic IR MISSING_CAPABILITY {exc.capability}: {exc.detail}") from exc
            except SemanticIRError as exc:
                raise TextCompileError(f"Semantic IR rejected locally: {exc}") from exc
            if getattr(args, 'ast', False):
                import json as _json
                print("SEMANTIC IR:")
                print(_json.dumps(plan, indent=2, ensure_ascii=False))
            print("PSEUDO-CODE:")
            print(pseudo)
            return result, {"route":"agent-ir", "family":"semantic-ir", "semantic_ir":plan, "agent_metadata":agent_meta}, pseudo

    result, meta = compile_hybrid(source, force_boot=args.boot)
    print(f"SEMANTIC: deterministic hybrid-v{SEMANTIC_ENGINE_VERSION} route={meta.get('route')} family={meta.get('family')}")
    if meta.get("canonical_english") and (meta.get("language") != "en" or meta.get("canonical_english") != source.strip().lower()):
        print(f"LANGUAGE FRONTEND: {meta.get('language','unknown')} -> en")
        print(f"CANONICAL ENGLISH: {meta.get('canonical_english')}")
    if meta.get('semantic_goal'):
        g=meta['semantic_goal']; print(f"SEMANTIC GOAL: {g.get('goal')} preserve={','.join(g.get('preserve',[]))}")
    if getattr(args,'ast',False):
        import json as _json
        print("SEMANTIC AST:")
        print(_json.dumps(meta.get('ast',{}), indent=2, ensure_ascii=False))
    pseudo=pseudo_from_result(result)
    print("PSEUDO-CODE:")
    print(pseudo)
    return result, meta, pseudo

def _print_result_schedule(result):
    schedule = getattr(result, "schedule", None)
    if schedule is None:
        return
    for line in format_schedule_abi(schedule):
        print(line)

def _handle_semantic_admin(args):
    if args.action == "semantic-ir-schema":
        import json as _json
        print(_json.dumps(semantic_ir_schema_contract(), indent=2, ensure_ascii=False))
        return True
    if args.action == "semantic-ir-compile":
        import json as _json
        if args.file:
            raw = Path(args.file).read_text(encoding="utf-8")
        else:
            raw = " ".join(args.json)
        try:
            obj = _json.loads(raw)
            obj = validate_semantic_ir(obj, require_compile=True)
            result, pseudo = compile_semantic_ir(obj, source="semantic-ir-cli")
        except (ValueError, OSError, SemanticIRError) as exc:
            raise TextCompileError(f"Semantic IR compile failed: {exc}") from exc
        print("SEMANTIC IR: validated")
        print("PSEUDO-CODE:")
        print(pseudo)
        print(f"BYTECODE ({len(result.bytecode)} bytes): {result.bytecode.hex(' ')}")
        _print_result_schedule(result)
        return True
    if args.action == "translate":
        n = canonicalize_to_english(" ".join(args.text))
        print(f"language: {n.language}")
        print(f"canonical_english: {n.canonical_english}")
        return True
    if args.action == "semantic-status":
        cfg = load_semantic_config(); low = cfg.get("battery_low_cv")
        print(f"engine:            R3.8.20 Semantic IR v{SEMANTIC_IR_VERSION} + hybrid-v{SEMANTIC_ENGINE_VERSION} fallback")
        print(f"vm_abi:            {SEMANTIC_IR_VM_ABI}")
        print(f"battery_low:       {(f'{low/100:.2f} V' if low is not None else '(unset)')}")
        print(f"battery_check:     {cfg.get('battery_check_s')} s")
        print(f"tracking_default:  {cfg.get('tracking_interval_ms')} ms")
        print(f"config:            {SEMANTIC_CONFIG_PATH}")
        print(f"user_examples:     {SEMANTIC_USER_EXAMPLES}")
        return True
    if args.action == "semantic-set":
        cfg = update_semantic_config(battery_low_v=args.battery_low_v, battery_check_s=args.battery_check_s)
        low = cfg.get("battery_low_cv")
        print("Hybrid semantic defaults updated.")
        print(f"battery_low:   {(f'{low/100:.2f} V' if low is not None else '(unset)')}")
        print(f"battery_check: {cfg.get('battery_check_s')} s")
        print(f"Config: {SEMANTIC_CONFIG_PATH}")
        return True
    if args.action == "semantic-teach":
        path = semantic_teach_example(" ".join(args.text), args.family, args.canonical or "")
        print(f"Semantic example stored: {path}")
        return True
    if args.action == "semantic-ast":
        import json as _json
        d = semantic_diagnose(" ".join(args.text), force_boot=args.boot)
        print(_json.dumps(d, indent=2, ensure_ascii=False))
        return True
    if args.action == "ask":
        import json as _json
        q=" ".join(args.text); cfg=load_agent_config()
        if not getattr(args,'standalone',False) and agent_external_available(cfg):
            try:
                a=agent_answer_external(q,cfg)
                print(a["answer"])
                if getattr(args,"json",False): print(_json.dumps(a, indent=2, ensure_ascii=False))
                return True
            except Exception as exc:
                print(f"AI AGENT QA unavailable ({exc}); using standalone knowledge base")
        a = answer_question(q); print(a["answer"])
        if getattr(args, "json", False): print(_json.dumps(a, indent=2, ensure_ascii=False))
        return True
    if args.action == 'pseudo-help':
        print(PSEUDO_GRAMMAR.rstrip()); return True
    if args.action in ('compile', 'compile-pseudo'):
        if args.file:
            text=Path(args.file).read_text(encoding='utf-8')
        else:
            text=' '.join(args.text or [])
        result=compile_pseudo(text, force_boot=args.boot)
        print(format_natural_ir(result))
        print(f"BYTECODE ({len(result.bytecode)} bytes): {natural_bytecode_hex(result.bytecode)}")
        _print_result_schedule(result)
        if args.output:
            Path(args.output).write_bytes(result.bytecode); print(f"Wrote: {args.output}")
        return True
    if args.action == 'examples':
        import json as _json
        paths=[Path(__file__).resolve().parent/'nrfclaw_semantic_examples.json', Path(__file__).resolve().parent/'nrfclaw_semantic_stress_cases.json']
        shown=0
        for path in paths:
            try: arr=_json.loads(path.read_text(encoding='utf-8'))
            except Exception: continue
            if not isinstance(arr,list): continue
            for item in arr:
                if isinstance(item,str): text=item
                elif isinstance(item,dict): text=item.get('text') or item.get('prompt')
                else: continue
                if not text: continue
                shown+=1; print(f"{shown:2d}. {text}")
                if shown>=args.limit: return True
        return True
    if args.action == 'caps-help':
        import json as _json
        caps=Path(__file__).resolve().parent/'nrfclaw_semantic_capabilities.json'
        kb=Path(__file__).resolve().parent/'nrfclaw_knowledge.json'
        print('=== nRFClaw local capability catalog ===')
        for path in (caps,kb):
            try:
                obj=_json.loads(path.read_text(encoding='utf-8'))
                print(f"\n[{path.name}]")
                if path==caps:
                    print(_json.dumps(obj,indent=2,ensure_ascii=False))
                else:
                    print('features:'); [print(f"  - {x}") for x in obj.get('features',[])]
                    print('limitations:'); [print(f"  - {x}") for x in obj.get('limitations',[])]
            except Exception as exc: print(f"cannot read {path}: {exc}")
        return True
    if args.action == 'ha-help':
        print('Home Assistant integration:')
        print('  Plane: BLE Application GATT + NDP-SESSION')
        print('  Local integration: integrations/home-assistant/custom_components/nrfclaw/')
        print('  Useful commands: ha scan | ha info | ha status | ha battery | ha sensors | ha test')
        print('  Reconnect validation: ha reconnect --count 30')
        print('Natural-language programs may emit HA events when the runtime capability is available.')
        return True
    return False

def _handle_agent_admin(args):
    if args.action == "agent-list":
        print("AI semantic-agent providers:")
        for name, meta in provider_catalog().items():
            key = meta["api_key_env"] or "none (local)"
            print(f"  {name:10s} default_model={meta['default_model']} key={key}")
        print("  builtin    deterministic parser only (no AI, current legacy behavior)")
        return True
    if args.action == "agent-status":
        cfg = load_agent_config()
        print(f"enabled:     {'yes' if cfg.enabled else 'no'}")
        print(f"provider:    {cfg.provider}")
        print(f"model:       {cfg.model}")
        print(f"base_url:    {cfg.base_url}")
        print(f"api_key_env: {cfg.api_key_env or '(none)'}")
        print(f"timeout:     {cfg.timeout_s}s")
        print(f"config:      {AGENT_CONFIG_PATH}")
        print(f"KB/legacy schema: {AGENT_KB_VERSION}/{AGENT_SCHEMA_VERSION} VM_ABI={AGENT_VM_ABI}")
        print(f"Semantic IR:      v{SEMANTIC_IR_VERSION} VM_ABI={SEMANTIC_IR_VM_ABI}")
        return True
    if args.action == "agent-set":
        cfg = configure_agent(args.provider, model=args.model, base_url=args.base_url,
                              api_key_env=args.api_key_env, timeout_s=args.timeout)
        print(f"AI agent enabled: provider={cfg.provider} model={cfg.model}")
        print(f"Config: {AGENT_CONFIG_PATH}")
        if cfg.api_key_env:
            print(f"API key is read only from environment variable {cfg.api_key_env}; it is not stored in config.")
        return True
    if args.action == "agent-off":
        disable_agent()
        print("AI semantic agent disabled. AUTO prompt mode falls back to standalone engine.")
        return True
    if args.action == "agent-kb-status":
        cfg=load_agent_config()
        print(f"kb_sha256:   {agent_knowledge_sha256()}")
        print(f"provider:    {cfg.provider}")
        print(f"model:       {cfg.model}")
        print(f"reachable:   {'yes' if agent_external_available(cfg) else 'no'}")
        print("learning:    automatic context injection on every request; no fine-tuning required")
        return True
    if args.action == "agent-test":
        cfg=load_agent_config()
        if not cfg.enabled:
            raise AgentError("AI agent is disabled; configure it first with agent-set")
        print(f"Testing provider={cfg.provider} model={cfg.model} ...")
        ans=agent_answer_external("Answer in one sentence: what is the maximum NINASENSE supply voltage?",cfg)
        print("API:         OK")
        print(f"KB_SHA256:   {ans.get('kb_sha256','')}")
        print(f"Answer:      {ans.get('answer','')}")
        return True
    return False



def _intel_hex_to_app(path: Path) -> bytes:
    mem = {}
    upper = 0
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        line=line.strip()
        if not line: continue
        if not line.startswith(":"): raise ValueError(f"Invalid Intel HEX line {lineno}")
        raw=bytes.fromhex(line[1:])
        if (sum(raw)&0xFF)!=0: raise ValueError(f"Intel HEX checksum error line {lineno}")
        count=raw[0]; addr=(raw[1]<<8)|raw[2]; typ=raw[3]; data=raw[4:4+count]
        if typ==0x00:
            base=upper+addr
            for i,b in enumerate(data): mem[base+i]=b
        elif typ==0x01: break
        elif typ==0x04:
            if count!=2: raise ValueError("Invalid extended linear address")
            upper=((data[0]<<8)|data[1])<<16
        elif typ in (0x02,0x03,0x05):
            continue
    app={a:b for a,b in mem.items() if DFU_APP_START<=a<DFU_APP_END}
    outside=[a for a in mem if a>=DFU_APP_END and a<0x00080000]
    if outside: raise ValueError(f"HEX contains data in reserved nRFClaw data/bootloader area at 0x{min(outside):08X}")
    if not app: raise ValueError("HEX contains no application bytes in 0x26000..0x6BFFF")
    end=max(app)+1
    out=bytearray([0xFF])*(end-DFU_APP_START)
    for a,b in app.items(): out[a-DFU_APP_START]=b
    return bytes(out)

def load_firmware_image(filename: str) -> bytes:
    path=Path(filename)
    if path.suffix.lower() in (".hex", ".ihex"):
        data=_intel_hex_to_app(path)
    else:
        data=path.read_bytes()
    if not data: raise ValueError("Firmware image is empty")
    if len(data)>DFU_MAX_IMAGE: raise ValueError(f"Firmware too large: {len(data)} > {DFU_MAX_IMAGE} bytes")
    if len(data)<8: raise ValueError("Firmware image is too short")
    sp=int.from_bytes(data[0:4],"little"); pc=int.from_bytes(data[4:8],"little")
    if (sp & 0x2FFE0000)!=0x20000000: raise ValueError(f"Invalid application initial SP 0x{sp:08X}")
    if not (DFU_APP_START <= (pc & ~1) < DFU_APP_END) or not (pc&1): raise ValueError(f"Invalid application reset vector 0x{pc:08X}")
    return data


async def _bluez_forget_device(address: str, reason: str = ""):
    """Linux/BlueZ cache refresh for same-address app -> DFU transitions.

    BlueZ can keep the application's old GATT object tree while the same BLE
    address has already rebooted into the DFU database. Bleak may then fail
    inside service discovery with KeyError('org.bluez.GattService1'). Removing
    the unpaired cache entry forces a clean rediscovery. Other platforms are
    left untouched.
    """
    if not sys.platform.startswith("linux"):
        return False
    try:
        proc = await asyncio.create_subprocess_exec(
            "bluetoothctl", "remove", address,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        out, _ = await asyncio.wait_for(proc.communicate(), timeout=5.0)
        text = out.decode(errors="replace").strip()
        if reason:
            print(f"    BlueZ cache refresh ({reason})...")
        if proc.returncode == 0:
            print(f"    BlueZ device cache removed: {address}")
            await asyncio.sleep(0.75)
            return True
        if text:
            print(f"    BlueZ cache refresh skipped: {text}")
    except (FileNotFoundError, asyncio.TimeoutError):
        pass
    except Exception as exc:
        print(f"    BlueZ cache refresh warning: {type(exc).__name__}: {exc}")
    return False

async def find_dfu_device(timeout: float):
    def match(dev,adv):
        name=(adv.local_name or dev.name or "")
        uuids={u.lower() for u in (adv.service_uuids or [])}
        return name.startswith("nRFClaw-DFU") and NUS_SERVICE.lower() in uuids
    dev=await BleakScanner.find_device_by_filter(match,timeout=timeout)
    if dev is None: raise RuntimeError("nRFClaw-DFU not found after reboot")
    return dev

class NRFClawDFUClient:
    def __init__(self,dev):
        self.dev=dev
        self.client=BleakClient(dev)
        self.q=asyncio.Queue()
        self.notifications=False

    async def open(self, connect_timeout=12.0, notify_timeout=8.0):
        print("    BLE connect...")
        await asyncio.wait_for(self.client.connect(), timeout=connect_timeout)
        if not self.client.is_connected:
            raise RuntimeError("BLE connect returned without an active connection")
        print("    BLE connected")

        # BlueZ does not expose the negotiated ATT MTU through the normal
        # Bleak API until AcquireWrite/AcquireNotify has been used.  Without
        # this step a 244-byte NUS WriteValue can be treated as a long write
        # while the host still assumes MTU 23, which the tiny DFU service does
        # not support.  Bleak itself uses this helper in its BlueZ MTU example.
        self.att_mtu = 23
        if sys.platform.startswith("linux"):
            backend=getattr(self.client,"_backend",None)
            acquire=getattr(backend,"_acquire_mtu",None)
            if callable(acquire):
                try:
                    print("    Negotiating ATT MTU through BlueZ...")
                    await asyncio.wait_for(acquire(), timeout=8.0)
                    self.att_mtu=int(getattr(self.client,"mtu_size",23) or 23)
                    print(f"    ATT MTU={self.att_mtu}")
                except Exception as exc:
                    print(f"    ATT MTU acquisition failed ({type(exc).__name__}: {exc}); using MTU 23 fallback")
                    self.att_mtu=23
        else:
            try:
                self.att_mtu=int(getattr(self.client,"mtu_size",23) or 23)
            except Exception:
                self.att_mtu=23

        print("    Enabling NUS notifications...")
        await asyncio.wait_for(
            self.client.start_notify(NUS_TX, self._notify),
            timeout=notify_timeout,
        )
        self.notifications=True
        print("    NUS notifications enabled")
        return self

    async def close(self):
        if self.notifications and self.client.is_connected:
            try:
                await asyncio.wait_for(self.client.stop_notify(NUS_TX), timeout=3.0)
            except Exception:
                pass
        self.notifications=False
        if self.client.is_connected:
            try:
                await asyncio.wait_for(self.client.disconnect(), timeout=5.0)
            except Exception:
                pass

    async def __aenter__(self):
        return await self.open()

    async def __aexit__(self,*exc):
        await self.close()

    def _notify(self,_s,data): self.q.put_nowait(bytes(data))
    async def xfer(self,frame: bytes, timeout=8.0):
        while not self.q.empty():
            try:self.q.get_nowait()
            except asyncio.QueueEmpty:break
        # Fix4b6 reliability policy: use an ATT Write Request for every DFU
        # command, including DATA.  The previous fast path used Write Command
        # (response=False) for DATA.  On BlueZ this proved fast but intermittent:
        # long transfers could stop at arbitrary offsets and even a following
        # INFO would time out.  A Write Request gives us controller/ATT-level
        # backpressure before waiting for the bootloader's end-to-end DFU ACK.
        # We deliberately trade one extra ATT response per block for reliability.
        await self.client.write_gatt_char(NUS_RX,frame,response=True)

        # Wait for the response matching the command we actually sent and ignore
        # stale responses from another opcode instead of treating them as a
        # protocol failure.
        loop=asyncio.get_running_loop()
        deadline=loop.time()+timeout
        while True:
            remaining=deadline-loop.time()
            if remaining<=0:
                raise asyncio.TimeoutError()
            r=await asyncio.wait_for(self.q.get(),remaining)
            if len(r)!=6:
                continue
            cmd,st=r[0],r[1]; nxt=int.from_bytes(r[2:6],"little")
            if cmd!=frame[0]:
                continue
            if st!=0: raise RuntimeError(f"DFU command 0x{cmd:02X} failed status={st} next={nxt}")
            return nxt

    async def dfu_offset(self, timeout=3.0):
        """Return active DFU offset using INFO, or None if INFO also times out."""
        try:
            return await self.xfer(bytes([DFU_CMD_INFO]), timeout=timeout)
        except Exception:
            return None

async def _dfu_reconnect_active_session(args, attempts=5):
    """Reconnect to an already-running DFU bootloader without ABORT/BEGIN.

    The bootloader keeps m_active/m_next across a BLE disconnect, so INFO after
    reconnect is the authoritative resume offset.  This is also required after
    ATT errors because BlueZ may still report is_connected=True briefly while
    the link is already being torn down.
    """
    last_exc=None
    for attempt in range(1, attempts+1):
        if attempt>1:
            await asyncio.sleep(min(0.5*attempt, 2.0))
        try:
            dev=await find_dfu_device(args.dfu_timeout)
            candidate=NRFClawDFUClient(dev)
            await candidate.open()
            info=await candidate.xfer(bytes([DFU_CMD_INFO]), timeout=5.0)
            return candidate, info
        except Exception as exc:
            last_exc=exc
            try:
                await candidate.close()
            except Exception:
                pass
    raise RuntimeError(
        f"Unable to reconnect active DFU session; last error: "
        f"{type(last_exc).__name__}: {last_exc!s}"
    )

async def firmware_upgrade(args):
    image=load_firmware_image(args.firmware)
    crc=zlib.crc32(image)&0xFFFFFFFF
    print(f"Firmware: {len(image)} bytes CRC32=0x{crc:08X}")
    if not args.bootloader_already:
        name,dev=await find_device(args.device,args.scan_timeout)
        print(f"Connecting application: {name} ({dev.address})")
        async with NRFClawClient(dev,stage5_key(args.key)) as nrf:
            await nrf.enter_dfu()
        app_address=dev.address
        await asyncio.sleep(1.0)
        # Application and DFU intentionally reuse the same BLE address but
        # expose different GATT databases. Clear BlueZ's old application GATT
        # object tree before discovering the bootloader.
        await _bluez_forget_device(app_address, "application -> DFU")
    # The application and DFU bootloader intentionally use the same BLE
    # address.  BlueZ/Bleak can need a short settling period after the reset
    # and, after an aborted connection, a fresh scan result.  Retry the whole
    # scan -> connect -> notify -> INFO handshake instead of retrying only the
    # GATT operation on a stale Bleak device object.
    dfu=None
    last_exc=None
    attempts=5
    for attempt in range(1, attempts+1):
        if attempt>1:
            await asyncio.sleep(min(0.75*attempt, 3.0))
        print(f"DFU connection attempt {attempt}/{attempts}: scanning...")
        try:
            dev=await find_dfu_device(args.dfu_timeout)
            print(f"  Bootloader found: {getattr(dev,'name',None) or 'nRFClaw-DFU'} ({dev.address})")
            candidate=NRFClawDFUClient(dev)
            try:
                await candidate.open()

                # A failed DATA transaction can leave the bootloader's RAM
                # session active.  In Fix4b/4b1 INFO then returns m_next rather
                # than the capability flag, which made a reconnect incorrectly
                # fall back to 12-byte DATA chunks.  This upgrader always starts
                # from offset zero, so explicitly clear any stale session first.
                print("    Resetting stale DFU session...")
                await candidate.xfer(bytes([DFU_CMD_ABORT]), timeout=5.0)

                print("    DFU INFO...")
                info=await candidate.xfer(bytes([DFU_CMD_INFO]), timeout=8.0)
                # Fix4b capability encoding: bit31 set, low 16 bits = maximum
                # firmware bytes in one DATA command.  Also clamp to the MTU
                # actually negotiated by BlueZ: ATT value <= MTU-3 and the DFU
                # DATA frame consumes another 5 bytes for cmd+offset.
                if info & 0x80000000:
                    server_cap=max(12, min(info & 0xFFFF, 239))
                    mtu=int(getattr(candidate,"att_mtu",23) or 23)
                    host_cap=max(12, mtu-8)
                    candidate.max_data_chunk=min(server_cap,host_cap)
                else:
                    server_cap=12
                    candidate.max_data_chunk=12
                print(f"    DFU INFO OK (server={server_cap}, ATT MTU={getattr(candidate,'att_mtu',23)}, DATA chunk={candidate.max_data_chunk} bytes)")
                dfu=candidate
                break
            except Exception:
                await candidate.close()
                raise
        except Exception as exc:
            last_exc=exc
            print(f"  Attempt {attempt} failed: {type(exc).__name__}: {exc!s}")
            if isinstance(exc, KeyError) and "org.bluez.GattService1" in str(exc):
                addr=getattr(locals().get("dev", None), "address", None)
                if addr:
                    await _bluez_forget_device(addr, "stale GATT service tree")

    if dfu is None:
        raise RuntimeError(
            f"Unable to establish DFU NUS session after {attempts} attempts; "
            f"last error: {type(last_exc).__name__}: {last_exc!s}"
        )

    try:
        print("DFU BEGIN...")
        await dfu.xfer(bytes([DFU_CMD_BEGIN])+struct.pack('<II',len(image),crc), timeout=10.0)
        print("DFU BEGIN OK")
        off=0
        negotiated_chunk=getattr(dfu,"max_data_chunk",12)
        # R3.8.19b Fix4b8: bench testing on F07138 established 64 firmware
        # bytes per DATA command as the stable production transport size.
        # Do not probe larger packets during a normal firmware upgrade: the
        # 96-byte path eventually produced ATT 0x0e, while 128..192 failed
        # immediately despite an ATT MTU of 247. Keep MTU negotiation for
        # capability/diagnostics, but cap the production DATA payload at 64.
        chunk=min(64, negotiated_chunk)
        print(f"DFU transport: stable {chunk}-byte firmware DATA chunks (negotiated max={negotiated_chunk})")

        # Reconnect/resume from Fix4b7 remains active. A transport failure does
        # not imply that the current DATA block was lost: after reconnect INFO
        # returns the bootloader's authoritative m_next. Retry the same 64-byte
        # block when it was not consumed, or continue when it was consumed.
        consecutive_transport_errors=0
        max_transport_errors=5
        while off<len(image):
            page_left=4096-(off%4096)
            n=min(chunk,len(image)-off,page_left)
            expected=off+n
            try:
                nxt=await dfu.xfer(bytes([DFU_CMD_DATA])+struct.pack('<I',off)+image[off:off+n],timeout=10.0)
                consecutive_transport_errors=0
            except Exception as exc:
                consecutive_transport_errors += 1
                try:
                    await dfu.close()
                except Exception:
                    pass
                print(f"  DATA transport error at offset {off}: {type(exc).__name__}: {exc}; reconnecting...")
                dfu,pos=await _dfu_reconnect_active_session(args)
                if pos==expected:
                    nxt=pos
                    consecutive_transport_errors=0
                    print(f"  DFU resume: block already consumed; offset={pos}")
                elif pos==off:
                    if consecutive_transport_errors >= max_transport_errors:
                        raise RuntimeError(
                            f"DFU DATA failed {consecutive_transport_errors} consecutive times "
                            f"at offset {off} using stable {chunk}-byte chunks"
                        ) from exc
                    print(
                        f"  DFU resume: block not consumed; retry {consecutive_transport_errors}/"
                        f"{max_transport_errors-1} at offset {off} with {chunk}-byte chunk"
                    )
                    continue
                else:
                    raise RuntimeError(
                        f"DFU resume offset mismatch after reconnect: device={pos} "
                        f"host={off} expected={expected}"
                    ) from exc
            if nxt!=expected:
                raise RuntimeError(f"DFU offset mismatch: device={nxt} host={expected}")
            off=nxt
            if off==len(image) or off%4096==0:
                print(f"  {off:6d}/{len(image)} bytes ({100.0*off/len(image):5.1f}%)")
        print("DFU END / CRC verify...")
        await dfu.xfer(bytes([DFU_CMD_END]),timeout=15.0)
        print("Firmware verified and committed; device rebooting into application.")
    finally:
        await dfu.close()

async def main_async(args):
    # R3.8.16 local semantic/agent configuration commands never touch BLE.
    if _handle_semantic_admin(args) or _handle_agent_admin(args):
        return

    # Compile-only mode must not touch BLE. The optional AI agent only creates
    # a validated semantic plan; bytecode still comes from deterministic compiler.
    if args.action == "prompt" and not args.upload:
        source = " ".join(args.text)
        result, _plan, _canonical = _resolve_prompt_compilation(args, source)
        print(format_natural_ir(result))
        print(f"BYTECODE ({len(result.bytecode)} bytes): {natural_bytecode_hex(result.bytecode)}")
        _print_result_schedule(result)
        if args.output:
            Path(args.output).write_bytes(result.bytecode)
            print(f"Wrote: {args.output}")
        print("Validation: OK (deterministic compiler; no device upload requested)")
        return

    if args.action == "firmware-upgrade":
        if not args.device and not args.bootloader_already:
            raise RuntimeError("--device is required unless --bootloader-already is used")
        await firmware_upgrade(args)
        return

    if not args.device:
        raise RuntimeError("--device is required for commands that access BLE hardware")

    # 'ha' is the reference client for the normal Application BLE plane.
    # All pre-existing commands below remain on the validated NUS plane.
    if args.action == "ha":
        await run_ha_mode(args)
        return

    name, dev = await find_device(
        args.device,
        args.scan_timeout,
    )

    print(
        f"Connecting: {name} "
        f"({dev.address})"
    )

    async with NRFClawClient(
        dev,
        stage5_key(args.key),
    ) as nrf:
        if args.action == "accel-probe":
            await nrf.accel_probe()

        elif args.action == "accel":
            await nrf.accel_read()

        elif args.action == "ds18b20":
            await nrf.ds18b20_read()

        elif args.action == "accel-config":
            await nrf.accel_config(
                args.mode,
                args.odr,
                args.scale,
                args.threshold,
                args.duration,
                args.low_power,
                None,
            )

        elif args.action == "vibration-read":
            await nrf.vibration_read(args.csv)

        elif args.action == "vib-model-set":
            await nrf.vib_model_set(
                args.rms,args.rms_tol,args.peak,args.peak_tol,
                args.p2p,args.p2p_tol,args.zero_cross,args.zero_cross_tol,
                None,
            )

        elif args.action == "vib-health":
            await nrf.vib_health()

        elif args.action == "vib-auto-start":
            await nrf.vib_auto_start(args.relearn, None)

        elif args.action == "vib-auto-stop":
            await nrf.vib_auto_stop(None)

        elif args.action == "vib-auto-status":
            await nrf.vib_auto_status()

        elif args.action == "vib-auto-diagnostics":
            await nrf.vib_auto_diagnostics()

        elif args.action == "vib-auto-trace":
            await nrf.vib_auto_trace()

        elif args.action == "ble-boot":
            await nrf.ble_boot_control(args.ble_boot_action)

        elif args.action == "vib-auto-baseline":
            await nrf.vib_auto_baseline()

        elif args.action == "vib-auto-reset-learning":
            await nrf.vib_auto_reset(None)

        elif args.action == "vib-auto-config":
            await nrf.vib_auto_config(None,
                learning_time=args.learning_time, discovery_interval=args.discovery_interval,
                normal_interval=args.normal_interval, suspicious_interval=args.suspicious_interval,
                wake_threshold=args.wake_threshold, wake_duration=args.wake_duration,
                confirm_delay=args.confirm_delay, arming_delay=args.arming_delay, off_rms=args.off_rms,
                profile_match=int(args.profile_match * 256) if args.profile_match is not None else None,
                adapt_limit=int(args.adapt_limit * 256) if args.adapt_limit is not None else None,
                candidate_confirmations=args.candidate_confirmations, alarm_consecutive=args.alarm_consecutive,
                max_profiles=args.max_profiles, sensitivity=args.sensitivity,
                adaptation_shift=args.adaptation_shift, stable_expand_after=args.stable_expand_after,
                max_interval_multiplier=args.max_interval_multiplier)

        elif args.action == "ndp-access":
            await nrf.ndp_access_status()

        elif args.action == "ndp-logout":
            await nrf.ndp_logout()

        elif args.action == "ndp-key-status":
            await nrf.ndp_key_status()

        elif args.action == "ndp-key-get":
            await nrf.ndp_key_get()

        elif args.action == "ndp-key-generate":
            await nrf.ndp_key_generate()

        elif args.action == "hall-status":
            await nrf.hall_status()

        elif args.action == "hall-single":
            await nrf.hall_config(
                HALL_MODE_SINGLE,
                pullup=args.pullup,
                count_edges=args.count,
                emit_events=args.event,
                channel=args.channel,
            )

        elif args.action == "hall-quadrature":
            await nrf.hall_config(
                HALL_MODE_QUADRATURE,
                pullup=args.pullup,
                count_edges=args.count,
                emit_events=args.event,
            )

        elif args.action == "hall-disable":
            await nrf.hall_disable()

        elif args.action == "hall-read":
            await nrf.hall_read()

        elif args.action == "beacon-status":
            await nrf.beacon_status()

        elif args.action == "beacon-start":
            await nrf.beacon_start(args.interval,args.tx_power)

        elif args.action == "beacon-stop":
            await nrf.beacon_stop()

        elif args.action == "lora-get":
            try:
                await nrf.lora_get_ext()
            except RuntimeError:
                await nrf.lora_get()

        elif args.action == "lora-info":
            await nrf.lora_info()

        elif args.action == "lora-send":
            if args.every is not None:
                await nrf.lora_send_continuous(args.payload, args.every, args.count)
            else:
                await nrf.lora_send(args.payload)

        elif args.action == "lora-tx-boot":
            base = args.payload
            raw_base = base.encode("utf-8")
            if not raw_base or len(raw_base) > 32:
                raise ValueError("payload base must be 1..32 UTF-8 bytes")
            if '"' in base or "\\" in base:
                raise ValueError("payload base cannot contain quote or backslash characters")
            prefix = base + "-"
            period = int(args.every)
            if period < 1 or period > 604800:
                raise ValueError("--every must be between 1 and 604800 seconds")
            pseudo = (
                "PROGRAM BOOT\n"
                "MOV R0 = 1\n"
                "MOV R1 = 1\n"
                "LABEL LOOP\n"
                f'FORMAT BUFFER "{prefix}" + R0 AS U32_02\n'
                "LORA SEND BUFFER\n"
                "ADD R0 = R0 + R1\n"
                f"WAIT {period}s\n"
                "JMP LOOP\n"
                "END\n"
            )
            result = compile_pseudo(pseudo, force_boot=True)
            print(pseudo_from_result(result))
            print(f"BYTECODE ({len(result.bytecode)} bytes): {natural_bytecode_hex(result.bytecode)}")
            if getattr(result, "schedule", None):
                _s=result.schedule; print(f"SCHEDULE ABI: {schedule_mode_name(_s['mode'])} dow_mask=0x{_s['dow_mask']:02x} arg0={_s['arg0']} arg1={_s['arg1']}")
            await nrf.upload(result.bytecode, persist=True, schedule=schedule_boot())
            print(f"Autonomous LoRa TX persisted: {base}-01, {base}-02, ... every {period}s")
            print("Uses the persisted LoRa RF profile. Power-cycle or reset; NUS is not required after boot.")

        elif args.action == "lora-rx":
            await nrf.lora_rx(args.timeout)

        elif args.action == "ninalink-rx":
            await nrf.ninalink_rx(args.timeout)

        elif args.action == "ninalink-tx-lab":
            await nrf.ninalink_lab_start(args.every)

        elif args.action == "ninalink-tx-status":
            await nrf.ninalink_lab_status()

        elif args.action == "ninalink-tx-stop":
            await nrf.ninalink_lab_stop()

        elif args.action == "ninalink-tx-max":
            await nrf.ninalink_tx_max()

        elif args.action == "ninalink-bridge-start":
            await nrf.ninalink_bridge_start()

        elif args.action == "ninalink-bridge-status":
            await nrf.ninalink_bridge_status()

        elif args.action == "ninalink-bridge-rx":
            await nrf.ninalink_bridge_rx(args.timeout)

        elif args.action == "ninalink-bridge-stop":
            await nrf.ninalink_bridge_stop()

        elif args.action == "ninalink-ack-test":
            await nrf.ninalink_ack_test(args.window, args.wait)

        elif args.action == "ninalink-link-status":
            await nrf.ninalink_link_status()

        elif args.action == "ninalink-reliable-test":
            await nrf.ninalink_reliable_test(
                args.window, args.attempts, args.backoff,
                args.wait, args.expect_timeout
            )

        elif args.action == "ninalink-link-reliability":
            await nrf.ninalink_link_reliability_status()

        elif args.action == "ninalink-bridge-reliability":
            await nrf.ninalink_bridge_reliability()

        elif args.action == "ninalink-bridge-drop-next-ack":
            await nrf.ninalink_bridge_drop_next_ack()

        elif args.action == "ninalink-cap-set-tracking":
            await nrf.ninalink_cap_set_tracking(args.node, args.value)

        elif args.action == "ninalink-app-status":
            await nrf.ninalink_bridge_app_status()

        elif args.action == "ninalink-node-app-status":
            await nrf.ninalink_node_app_status()

        elif args.action == "ninalink-node-drop-next-app-result":
            await nrf.ninalink_node_drop_next_app_result()

        elif args.action == "ninalink-node-app-reliability":
            await nrf.ninalink_node_app_reliability()

        elif args.action == "ninalink-command":
            await nrf.ninalink_command(args.node, args.command_id, args.data)

        elif args.action == "ninalink-command-echo":
            await nrf.ninalink_command_echo(args.node, args.token)

        elif args.action == "ninalink-command-status":
            await nrf.ninalink_command_status()

        elif args.action == "ninalink-node-command-status":
            await nrf.ninalink_node_command_status()

        elif args.action == "ninalink-node-drop-next-command-result":
            await nrf.ninalink_node_drop_next_command_result()

        elif args.action == "lora-rx-cancel":
            await nrf.lora_rx_cancel()

        elif args.action == "lora-set":
            if args.sync is not None or args.preamble is not None:
                cur = await nrf.lora_get_ext(quiet=True)
                await nrf.lora_set_ext(
                    args.freq, args.power, args.sf, args.bw, args.cr,
                    cur["sync_word"] if args.sync is None else args.sync,
                    cur["preamble"] if args.preamble is None else args.preamble,
                )
            else:
                await nrf.lora_set(args.freq, args.power, args.sf, args.bw, args.cr)

        elif args.action == "lora-frequency":
            await nrf.lora_patch(frequency_hz=args.freq)

        elif args.action == "lora-power":
            await nrf.lora_patch(power_dbm=args.power)

        elif args.action == "lora-sf":
            await nrf.lora_patch(sf=args.sf)

        elif args.action == "lora-sync":
            await nrf.lora_patch(sync_word=args.sync)

        elif args.action == "lora-preamble":
            await nrf.lora_patch(preamble=args.preamble)

        elif args.action == "board":
            await nrf.board()

        elif args.action == "board-info":
            await nrf.board_info()

        elif args.action == "board-memory":
            await nrf.board_memory()

        elif args.action == "board-reset":
            await nrf.board_reset()

        elif args.action == "board-bootloader":
            await nrf.board_bootloader()

        elif args.action == "ndp-info":
            await nrf.ndp_info()

        elif args.action == "ndp-status":
            await nrf.ndp_status()

        elif args.action == "info":
            await nrf.hello()
            await nrf.status()

        elif args.action == "status":
            await nrf.status()

        elif args.action == "ping":
            await nrf.ping()

        elif args.action == "time":
            await nrf.sync_time()

        elif args.action == "run":
            await nrf.run_loaded()

        elif args.action == "stop":
            await nrf.stop()

        elif args.action == "prompt":
            source = " ".join(args.text)
            result, _plan, _canonical = _resolve_prompt_compilation(args, source)
            print(format_natural_ir(result))
            print(f"BYTECODE ({len(result.bytecode)} bytes): {natural_bytecode_hex(result.bytecode)}")
            _print_result_schedule(result)
            if args.output:
                Path(args.output).write_bytes(result.bytecode)
                print(f"Wrote: {args.output}")
            if args.run and not args.upload:
                raise ValueError("--run requires --upload")
            if args.upload:
                schedule = getattr(result, "schedule", None) or (schedule_boot() if result.schedule_mode == SCHED_BOOT else schedule_manual())
                await nrf.upload(result.bytecode, persist=True, schedule=schedule)
                print(f"Text program uploaded and persisted with schedule={result.schedule_name}.")
                if args.run:
                    if result.schedule_mode in (SCHED_BOOT,SCHED_AT,SCHED_EVERY,SCHED_WEEKLY):
                        raise ValueError("--run cannot be combined with an autonomous scheduled text program; let the scheduler start it")
                    await nrf.run_loaded()
                    print("Text program started.")
                elif result.schedule_mode == SCHED_BOOT:
                    print("Reset the board to start the BOOT program autonomously.")
                elif result.schedule_mode in (SCHED_AT,SCHED_EVERY,SCHED_WEEKLY):
                    print("Autonomous schedule persisted; execution will be started by the firmware scheduler.")
            else:
                print("Preview only; use --upload to persist this program on the device.")

        elif args.action == "upload":
            code = Path(args.file).read_bytes()

            await nrf.upload(
                code,
                persist=True,
                schedule=schedule_boot() if args.boot else schedule_manual(),
            )
            if args.boot:
                print("Generic BOOT schedule persisted. Reset to start autonomously.")

        elif args.action == "beacon-boot-test":
            code = beacon_boot_program(args.name, args.interval, args.tx_power, args.wait)
            await nrf.upload(code, persist=True, schedule=schedule_boot())
            print(f"Beacon BOOT persisted: wait={args.wait}s name={args.name!r} interval={args.interval}ms tx={args.tx_power:+d}dBm")
            print("Reset the board; the VM will start it autonomously.")

        elif args.action == "boot-audit":
            supported, active = await nrf.capabilities()
            print("\nGeneric BOOT scheduler: supported")
            print("Persistence: atomic program+schedule flash slots")
            print("BOOT scope: entire VM bytecode, not per-capability flags")
            print("VM BOOT-capable primitives: GPIO BATTERY DS18B20 ACCEL LORA BLE_APP RTC HALL STATE SERIAL TRACKING VIB_HEALTH/VIB_AUTO via native/NDP where exposed")
            print("Note: a capability must have a VM opcode/native primitive to be orchestrated inside one bytecode program.")

        elif args.action == "upload-run":
            code = Path(args.file).read_bytes()

            await nrf.upload(
                code,
                persist=True,
            )

            await nrf.run_loaded()

        elif args.action == "battery":
            await nrf.battery()

        elif args.action == "battery-every":
            await nrf.sync_time()
            await nrf.battery(
                schedule=schedule_every(args.seconds),
                run_now=False,
            )
            await nrf.status()

        elif args.action == "battery-at":
            await nrf.sync_time()
            await nrf.battery(
                schedule=schedule_at(args.epoch),
                run_now=False,
            )
            await nrf.status()

        elif args.action == "battery-weekly":
            await nrf.sync_time()
            await nrf.battery(
                schedule=schedule_weekly(args.days, args.time),
                run_now=False,
            )
            await nrf.status()

        elif args.action == "notify-string":
            await nrf.notify_string(
                args.text,
                args.channel,
            )

        elif args.action == "test-battery-lora":
            await nrf.test_battery_lora(
                args.wait
            )

        elif args.action == "inject-event":
            await nrf.inject_event(args.type, args.arg0, args.arg1)

        elif args.action == "caps":
            await nrf.capabilities()

        elif args.action == "reset":
            await nrf.reset_device()

        elif args.action == "dfu-enter":
            await nrf.enter_dfu()

        elif args.action in ("default", "factory-reset"):
            await nrf.factory_reset()

        elif args.action == "test-event-gpio":
            code = event_gpio_test(args.event, args.pin, args.value)
            print("Bytecode:", code.hex(" "))
            await nrf.upload(code, persist=True, schedule=schedule_boot())
            print("BOOT event program persisted. Reset the board to start it autonomously.")
            await nrf.status()

        elif args.action == "state-save":
            await nrf.state_save(args.state_key, args.value)

        elif args.action == "state-load":
            await nrf.state_load(args.state_key)

        elif args.action == "state-get":
            await nrf.state_get(args.state_key)

        elif args.action == "test-app-echo":
            await nrf.test_app_echo(args.interval, args.payload)

        elif args.action == "test-serial":
            code = serial_write_test(args.tx, args.rx, args.baud, args.text)
            print("Bytecode:", code.hex(" "))
            await nrf.upload(code, persist=True, schedule=schedule_manual())
            await nrf.run_loaded()

        elif args.action == "test-tracking":
            await nrf.test_tracking(
                args.key_hex,
                args.interval,
                args.tx_power,
            )

        elif args.action == "tracking-stop":
            await nrf.tracking_stop()

        elif args.action == "tracking-enable":
            await nrf.tracking_enable(
                args.interval,
                args.tx_power,
                args.rotation,
            )

        elif args.action == "tracking-info":
            await nrf.tracking_info()

        elif args.action == "tracking-identity":
            await nrf.tracking_identity()

        elif args.action == "tracking-export":
            await nrf.tracking_export(
                args.count,
                args.start_index,
                args.name,
                args.output,
            )

        elif args.action == "tracking-provision":
            await nrf.tracking_provision(
                args.device,
                name=args.name,
                output=args.output,
                keygen_url=args.keygen_url,
                no_qr=args.no_qr,
            )

        elif args.action == "tracking-debug":
            await nrf.tracking_debug()

        else:
            raise RuntimeError(
                f"Unsupported action {args.action}"
            )


def build_parser():
    p = argparse.ArgumentParser(
        description="nRFClaw reference dual-plane CLI: NUS programming + Application/HA NDP"
    )

    p.add_argument(
        "--device",
        required=False,
        help="MAC suffix shown in nRFClaw(NDP/NUS)-XXXXXX; not needed for ask/semantic/prompt-preview",
    )

    p.add_argument(
        "--scan-timeout",
        type=float,
        default=10.0,
    )

    p.add_argument(
        "--key",
        help=(
            "32-byte HMAC key as 64 hex characters. "
            "Defaults to NRFCLAW_KEY env or Stage-5 development key."
        ),
    )

    p.add_argument(
        "--control-key",
        help=(
            "Pack-02 CONTROL key. Plain UTF-8 or hex:<hex>. "
            "Deprecated for NUS in R3.7; accepted for compatibility but NUS is keyless."
        ),
    )

    p.add_argument(
        "--ndp-key",
        help=(
            "Optional 256-bit Application/NDP key. Use hex:<64 hex digits>. "
            "Needed only after the owner runs ndp-key-generate over P0.21/NUS."
        ),
    )

    sub = p.add_subparsers(
        dest="action",
        required=True,
    )

    sub.add_parser(
        "accel-probe",
        help="Probe LIS2DH12 address/WHO_AM_I via NDP over NUS",
    )

    sub.add_parser(
        "accel",
        help="Read LIS2DH12 XYZ acceleration in mg via NDP over NUS",
    )

    sub.add_parser(
        "ds18b20",
        help="Read DS18B20 temperature via NDP over NUS",
    )

    ac = sub.add_parser(
        "accel-config",
        help="Configure LIS2DH12 OFF/MOTION/TAP/FALL/WALK/VIBRATION (physical NUS access)",
    )
    ac.add_argument(
        "mode",
        choices=["off", "motion", "tap", "fall", "walk", "vibration"],
    )
    ac.add_argument(
        "--odr",
        type=int,
        choices=[1, 10, 25, 50, 100, 200, 400],
        default=10,
        help="output data rate in Hz",
    )
    ac.add_argument(
        "--scale",
        type=int,
        choices=[2, 4, 8, 16],
        default=2,
        help="full scale in g",
    )
    ac.add_argument(
        "--threshold",
        type=int,
        default=120,
        help="event threshold in mg",
    )
    ac.add_argument(
        "--duration",
        type=int,
        default=100,
        help="event duration in ms",
    )
    ac.add_argument(
        "--low-power",
        action="store_true",
        help="request LIS2DH12 low-power mode",
    )

    vr = sub.add_parser(
        "vibration-read",
        help="Read vibration metrics and captured XYZ window via NDP over NUS",
    )
    vr.add_argument(
        "--csv",
        help="save raw XYZ samples to CSV instead of printing them",
    )

    vm = sub.add_parser(
        "vib-model-set",
        help="EXPERIMENTAL: load compact normal-vibration model (physical NUS access)",
    )
    vm.add_argument("--rms", type=int, required=True)
    vm.add_argument("--rms-tol", type=int, required=True)
    vm.add_argument("--peak", type=int, required=True)
    vm.add_argument("--peak-tol", type=int, required=True)
    vm.add_argument("--p2p", type=int, required=True)
    vm.add_argument("--p2p-tol", type=int, required=True)
    vm.add_argument("--zero-cross", type=int, required=True)
    vm.add_argument("--zero-cross-tol", type=int, required=True)

    sub.add_parser(
        "vib-health",
        help="EXPERIMENTAL: acquire one low-duty vibration window and score it",
    )

    vas = sub.add_parser("vib-auto-start", help="EXPERIMENTAL: enable autonomous vibration learning/monitoring")
    vas.add_argument("--relearn", action="store_true", help="discard current baseline and learn again on next machine start")

    sub.add_parser("vib-auto-stop", help="EXPERIMENTAL: stop autonomous vibration monitoring")
    sub.add_parser("vib-auto-status", help="EXPERIMENTAL: show autonomous learner/monitor state")
    sub.add_parser("vib-auto-diagnostics", help="EXPERIMENTAL: show discovery clustering instrumentation")
    sub.add_parser("vib-auto-trace", help="r3.8.13: show RAM-only VIB_AUTO transition/arm/robust-confirmation trace")
    sub.add_parser("vib-auto-baseline", help="EXPERIMENTAL: show locally learned baseline")
    sub.add_parser("vib-auto-reset-learning", help="EXPERIMENTAL: clear baseline and relearn on next machine start")

    vac = sub.add_parser("vib-auto-config", help="EXPERIMENTAL r2: configure self-learning multi-profile vibration policy")
    vac.add_argument("--learning-time", type=parse_duration_seconds, default=None, help="discovery window, e.g. 5m, 2h, 24h")
    vac.add_argument("--discovery-interval", type=int, default=None, help="seconds between discovery windows while machine is active")
    vac.add_argument("--normal-interval", type=int, default=None, help="base seconds between healthy monitoring windows")
    vac.add_argument("--suspicious-interval", type=int, default=None, help="seconds between anomaly rechecks")
    vac.add_argument("--wake-threshold", type=int, default=None, help="HP-filtered INT1 threshold in mg")
    vac.add_argument("--wake-duration", type=int, default=None, help="INT1 duration in ms")
    vac.add_argument("--confirm-delay", type=int, default=None, help="legacy field; r3.8.5 INT-first starts the first FIFO window immediately")
    vac.add_argument("--arming-delay", type=int, default=None, help="installation settle delay in seconds before INT1 is armed (0..604800; 7 days)")
    vac.add_argument("--off-rms", type=int, default=None, help="initial quiet RMS floor; device refines noise floor")
    vac.add_argument("--max-profiles", type=int, default=None, help="upper bound only; device discovers actual count (1..6)")
    vac.add_argument("--candidate-confirmations", type=int, default=None, help="unknown windows required before creating a profile")
    vac.add_argument("--alarm-consecutive", type=int, default=None, help="anomalous windows required before alarm")
    vac.add_argument("--sensitivity", choices=["low","normal","high"], default=None)
    vac.add_argument("--profile-match", type=float, default=None, help="normalized discovery cluster score threshold")
    vac.add_argument("--adapt-limit", type=float, default=None, help="only samples below this score may slowly age a profile")
    vac.add_argument("--adaptation-shift", type=int, default=None, help="EMA power-of-two shift; default 11 => ~1/2048")
    vac.add_argument("--stable-expand-after", type=int, default=None, help="stable samples before monitoring interval expands")
    vac.add_argument("--max-interval-multiplier", type=int, default=None, help="maximum automatic interval expansion")

    sub.add_parser(
        "ndp-access",
        help="Show current PUBLIC/CONTROL/PROVISION NDP access level",
    )

    sub.add_parser(
        "ndp-logout",
        help="Drop current NDP session back to PUBLIC",
    )

    sub.add_parser("ndp-key-status", help="Show optional NDP protection state (physical NUS only)")
    sub.add_parser("ndp-key-get", help="Read the stored 256-bit NDP key (physical NUS only)")
    sub.add_parser("ndp-key-generate", help="Generate/replace and persist a 256-bit NDP key (physical NUS only)")

    bb = sub.add_parser("ble-boot", help="r3.8.10: persistent Application/NDP boot advertising control; NUS remains on P0.21")
    bb.add_argument("ble_boot_action", choices=["status","disable","enable"])

    sub.add_parser("hall-status", help="Show current Hall ownership/mode")

    hs = sub.add_parser("hall-single", help="Claim HALL1 or HALL2 as a single Hall/digital sensor")
    hs.add_argument("--channel", type=int, choices=[1,2], default=1, help="Hall channel: 1=HALL1, 2=HALL2")
    hs.add_argument("--pullup", action="store_true", help="enable internal pull-up")
    hs.add_argument("--count", action="store_true", help="count detected edges")
    hs.add_argument("--event", action="store_true", help="emit nRFClaw Hall events")

    hq = sub.add_parser("hall-quadrature", help="Claim HALL1+HALL2 as quadrature Hall encoder")
    hq.add_argument("--pullup", action="store_true", help="enable internal pull-ups")
    hq.add_argument("--count", action="store_true", help="count quadrature edges in addition to position")
    hq.add_argument("--event", action="store_true", help="emit nRFClaw Hall events")

    sub.add_parser("hall-disable", help="Release Hall ownership of HALL1/HALL2")
    sub.add_parser("hall-read", help="Read Hall count or quadrature position via NDP SENSOR_READ")

    sub.add_parser("beacon-status", help="Show BLE Beacon/Broadcaster test mode")
    bs = sub.add_parser("beacon-start", help="Configure non-connectable BLE Beacon/Broadcaster mode")
    bs.add_argument("--interval", type=int, default=2000, help="advertising interval in ms (20..10240)")
    bs.add_argument("--tx-power", type=int, default=4, help="BLE advertising TX power in dBm")
    sub.add_parser("beacon-stop", help="Disable BLE Beacon/Broadcaster mode")

    sub.add_parser("lora-get", help="Show current LLCC68 LoRa profile")
    sub.add_parser("lora-info", help="Show LLCC68 profile for direct RF diagnostics")
    ltx = sub.add_parser("lora-send", help="Direct LLCC68 TX diagnostic; bypass VM/compiler")
    ltx.add_argument("payload", help="UTF-8 payload base, 1..48 bytes")
    ltx.add_argument("--every", type=float, default=None, metavar="SECONDS",
                     help="continuous TX interval; payload becomes BASE_01, BASE_02, ...")
    ltx.add_argument("--count", type=int, default=0, metavar="N",
                     help="number of packets with --every (default: unlimited until Ctrl-C/NUS disconnect)")

    lboot = sub.add_parser("lora-tx-boot", help="Persist autonomous BOOT LoRa TX counter sequence")
    lboot.add_argument("payload", help="UTF-8 payload base; packets become BASE-01, BASE-02, ...")
    lboot.add_argument("--every", type=int, default=30, metavar="SECONDS",
                       help="autonomous TX period in seconds (default: 30); uses persisted RF profile")
    lrx = sub.add_parser("lora-rx", help="Direct LLCC68 RX diagnostic; bypass VM/compiler")
    lrx.add_argument("--timeout", type=float, default=30.0, help="receive-window duration in seconds; prints every packet until expiry")
    sub.add_parser("lora-rx-cancel", help="Cancel a stale direct LLCC68 RX diagnostic")
    nlrx = sub.add_parser("ninalink-rx", help="B4.1 decode NinaLink CAP_REPORT frames from continuous LLCC68 RX")
    nlrx.add_argument("--timeout", type=float, default=45.0, help="receive-window duration in seconds")
    nltx = sub.add_parser("ninalink-tx-lab", help="B4.1 arm RAM-only autonomous NinaLink battery/temperature uplink")
    nltx.add_argument("--every", type=int, default=15, metavar="SECONDS", help="period 2..300s; first TX after one full period")
    sub.add_parser("ninalink-tx-status", help="Show B4.1 autonomous NinaLink lab TX status")
    sub.add_parser("ninalink-tx-stop", help="Stop B4.1 autonomous NinaLink lab TX")
    sub.add_parser("ninalink-tx-max", help="B4.2 transmit one valid maximum 64-byte CAP_REPORT")
    sub.add_parser("ninalink-bridge-start", help="B4.2 start continuous on-device validated NinaLink bridge RX")
    sub.add_parser("ninalink-bridge-status", help="Show B4.2 NinaLink bridge counters/status")
    nbrx = sub.add_parser("ninalink-bridge-rx", help="Drain B4.2 on-device validated NinaLink frames")
    nbrx.add_argument("--timeout", type=float, default=45.0, help="validated bridge-drain window in seconds")
    sub.add_parser("ninalink-bridge-stop", help="Stop B4.2/B4.3 NinaLink bridge RX")
    nack = sub.add_parser("ninalink-ack-test", help="B4.3 send ACK_REQ CAP_REPORT and receive bridge ACK")
    nack.add_argument("--window", type=int, default=600, metavar="MS", help="node ACK RX window, 100..4000 ms")
    nack.add_argument("--wait", type=float, default=3.0, metavar="SECONDS", help="CLI wait for ACK-test completion")
    sub.add_parser("ninalink-link-status", help="Show B4.3/B4.4 node link state")
    nrel = sub.add_parser("ninalink-reliable-test", help="B4.4 reliable ACK_REQ with bounded retry/backoff")
    nrel.add_argument("--window", type=int, default=600, metavar="MS", help="ACK RX window, 100..4000 ms")
    nrel.add_argument("--attempts", type=int, default=3, help="maximum TX attempts, 1..5")
    nrel.add_argument("--backoff", type=int, default=200, metavar="MS", help="base exponential retry backoff, 1..4000 ms")
    nrel.add_argument("--wait", type=float, default=6.0, metavar="SECONDS", help="CLI wait for transaction completion")
    nrel.add_argument("--expect-timeout", action="store_true", help="treat bounded retry exhaustion as the expected PASS")
    sub.add_parser("ninalink-link-reliability", help="Show B4.4 node retry/backoff counters")
    sub.add_parser("ninalink-bridge-reliability", help="Show B4.4 bridge duplicate/ACK counters")
    sub.add_parser("ninalink-bridge-drop-next-ack", help="B4.4 lab gate: suppress exactly the next first ACK")
    nset = sub.add_parser("ninalink-cap-set-tracking", help="B4.5 queue tracking_active CAP_SET on the bridge")
    nset.add_argument("--node", type=lambda x: int(x, 0), required=True, help="target node id, e.g. 0xAD64D423")
    nset.add_argument("--value", choices=("on","off"), required=True, help="requested tracking_active value")
    sub.add_parser("ninalink-app-status", help="Show B4.5 bridge application-downlink state")
    sub.add_parser("ninalink-node-app-status", help="Show B4.5/B4.6 last CAP_SET result on the node")
    sub.add_parser("ninalink-node-drop-next-app-result", help="B4.6 lab gate: apply next CAP_SET but suppress its result ACK once")
    sub.add_parser("ninalink-node-app-reliability", help="Show B4.6/B4.7 result loss-injection counters")
    ncmd = sub.add_parser("ninalink-command", help="B4.7 queue a generic NinaLink COMMAND")
    ncmd.add_argument("--node", type=lambda x: int(x, 0), required=True, help="target node id")
    ncmd.add_argument("--id", dest="command_id", type=lambda x: int(x, 0), required=True, help="command id")
    ncmd.add_argument("--data", default="", help="hex arguments, max 7 bytes in B4.7 NDP gate")
    necho = sub.add_parser("ninalink-command-echo", help="B4.7 queue ECHO_U32 COMMAND")
    necho.add_argument("--node", type=lambda x: int(x, 0), required=True, help="target node id")
    necho.add_argument("--token", type=lambda x: int(x, 0), required=True, help="u32 echo token")
    sub.add_parser("ninalink-command-status", help="Show B4.7 bridge COMMAND state/result")
    sub.add_parser("ninalink-node-command-status", help="Show B4.7 last COMMAND executed by node")
    sub.add_parser("ninalink-node-drop-next-command-result", help="B4.7 suppress next COMMAND result once")

    ls = sub.add_parser("lora-set", help="Set the complete LLCC68 LoRa profile")
    ls.add_argument("--freq", type=int, required=True, help="RF frequency in Hz")
    ls.add_argument("--power", type=int, required=True, help="TX power in dBm (-9..22)")
    ls.add_argument("--sf", type=int, required=True, help="spreading factor 5..11")
    ls.add_argument("--bw", type=int, required=True, choices=[125,250,500], help="bandwidth in kHz")
    ls.add_argument("--cr", type=int, required=True, choices=[1,2,3,4], help="coding rate 1..4 => 4/5..4/8")
    ls.add_argument("--sync", type=lambda x: int(x,0), default=None, help="LoRa sync word, e.g. 0x12")
    ls.add_argument("--preamble", type=int, default=None, help="preamble length in symbols")

    lf = sub.add_parser("lora-frequency", help="Change only LoRa RF frequency; preserve other profile fields")
    lf.add_argument("freq", type=int, help="RF frequency in Hz")

    lp = sub.add_parser("lora-power", help="Change only LoRa TX power; preserve other profile fields")
    lp.add_argument("power", type=int, help="TX power in dBm")

    lsf = sub.add_parser("lora-sf", help="Change only LoRa spreading factor; preserve other profile fields")
    lsf.add_argument("sf", type=int, help="spreading factor 5..11")

    lsync = sub.add_parser("lora-sync", help="Change and persist LoRa sync word")
    lsync.add_argument("sync", type=lambda x: int(x,0), help="sync word, e.g. 0x12 or 0x34")

    lpreamble = sub.add_parser("lora-preamble", help="Change and persist LoRa preamble length")
    lpreamble.add_argument("preamble", type=int, help="preamble symbols (1..65535)")

    sub.add_parser("board", help="Show complete Stage 10.2.3a BOARD information")
    sub.add_parser("board-info", help="Show board/firmware identity via NDP")
    sub.add_parser("board-memory", help="Show physical/reserved memory information via NDP")
    sub.add_parser("board-reset", help="Show captured RESETREAS via NDP")
    sub.add_parser("board-bootloader", help="Show bootloader presence/address via NDP")
    sub.add_parser("ndp-info", help="Test NDP INFO")
    sub.add_parser("ndp-status", help="Test NDP STATUS")
    sub.add_parser("info")
    sub.add_parser("status")
    sub.add_parser("ping")
    sub.add_parser("time")
    sub.add_parser("run")
    sub.add_parser("stop")
    sub.add_parser("battery")
    sub.add_parser("caps")
    sub.add_parser("reset")
    sub.add_parser("dfu-enter", help="Reboot from physical NUS into the nRFClaw BLE bootloader")
    fu=sub.add_parser("firmware-upgrade", help="Upgrade application firmware over BLE; no programmer after bootloader installation")
    fu.add_argument("firmware", help="application .bin or Intel .hex")
    fu.add_argument("--dfu-timeout", type=float, default=15.0, help="seconds to wait for bootloader advertising")
    fu.add_argument("--bootloader-already", action="store_true", help="skip application DFU-enter and connect directly to nRFClaw-DFU")
    sub.add_parser("default")
    sub.add_parser("factory-reset")

    be = sub.add_parser("battery-every")
    be.add_argument("seconds", type=int)

    ba = sub.add_parser("battery-at")
    ba.add_argument("epoch", type=int)

    bw = sub.add_parser("battery-weekly")
    bw.add_argument("--days", required=True, help="e.g. sat,sun")
    bw.add_argument("--time", required=True, help="UTC HH:MM[:SS]")

    ns = sub.add_parser(
        "notify-string"
    )

    ns.add_argument(
        "text"
    )

    ns.add_argument(
        "--channel",
        type=int,
        default=2,
    )

    pr = sub.add_parser(
        "prompt",
        help="compile text in AUTO mode (configured AI agent first, standalone fallback) into deterministic VM bytecode",
    )
    pr.add_argument(
        "text", nargs="+",
        help="natural-language program intent; quote the complete sentence",
    )
    pr.add_argument(
        "--boot", action="store_true",
        help="force generic BOOT persistence even when the sentence does not say boot/persistent",
    )
    pr.add_argument(
        "--upload", action="store_true",
        help="upload the compiled bytecode over physical NUS; default is preview only",
    )
    pr.add_argument(
        "--run", action="store_true",
        help="run after upload (MANUAL programs only; BOOT programs start after reset)",
    )
    pr.add_argument(
        "--output",
        help="also write the generated raw bytecode to this .bin file",
    )
    pr.add_argument(
        "--agent", action="store_true",
        help="force configured external/local API agent; default AUTO already prefers a reachable agent",
    )
    pr.add_argument(
        "--standalone", action="store_true",
        help="force offline standalone semantic engine and do not call any external/local agent API",
    )
    pr.add_argument(
        "--pseudo", action="store_true",
        help="show generated nRFClaw pseudo-code (AUTO mode shows it by default)",
    )
    pr.add_argument(
        "--ast", action="store_true",
        help="show semantic AST/plan before compilation",
    )
    pr.add_argument(
        "--agent-provider", choices=sorted(provider_catalog().keys()),
        help="override configured provider for this prompt (requires --agent)",
    )
    pr.add_argument(
        "--agent-model",
        help="override configured model for this prompt (requires --agent)",
    )

    sub.add_parser("semantic-ir-schema", help="show the R3.8.20 versioned Semantic IR contract")
    sir = sub.add_parser("semantic-ir-compile", help="validate/lower Semantic IR JSON locally into deterministic VM bytecode")
    sir.add_argument("json", nargs="*", help="Semantic IR JSON; for multiline input prefer --file")
    sir.add_argument("--file", help="read Semantic IR JSON from a UTF-8 file")

    trn = sub.add_parser("translate", help="offline domain translation/normalization to canonical English; no network or AI required")
    trn.add_argument("text", nargs="+", help="natural-language nRFclaw prompt to normalize")

    sub.add_parser("semantic-status", help="show Hybrid Semantic Engine defaults/status")
    sset = sub.add_parser("semantic-set", help="configure local semantic defaults")
    sset.add_argument("--battery-low-v", type=float, help="numeric low-battery threshold in volts")
    sset.add_argument("--battery-check-s", type=int, help="battery monitor cadence in seconds")
    steach = sub.add_parser("semantic-teach", help="teach a local phrase -> semantic family/canonical example")
    steach.add_argument("text", nargs="+", help="phrase to remember")
    steach.add_argument("--family", required=True, help="semantic family label")
    steach.add_argument("--canonical", help="optional deterministic canonical prompt")
    sast = sub.add_parser("semantic-ast", help="parse free text into the generic semantic AST without BLE")
    sast.add_argument("text", nargs="+", help="natural-language behavior to analyze")
    sast.add_argument("--boot", action="store_true", help="force BOOT schedule while analyzing")
    ask = sub.add_parser("ask", help="answer NINASENSE/nRFclaw product questions from the local knowledge base")
    ask.add_argument("text", nargs="+", help="product/electrical/radio/power question")
    ask.add_argument("--json", action="store_true", help="also print structured answer metadata")
    ask.add_argument("--standalone", action="store_true", help="answer only from local KB; never call configured agent")

    sub.add_parser("pseudo-help", help="show the deterministic nRFClaw pseudo-code grammar")
    cp = sub.add_parser("compile", help="compile nRFClaw pseudo-code directly into VM bytecode")
    cp.add_argument("text", nargs="*", help="pseudo-code text; for multiline input prefer --file")
    cp.add_argument("--file", help="read pseudo-code from a UTF-8 text file")
    cp.add_argument("--boot", action="store_true", help="force BOOT schedule")
    cp.add_argument("--output", "-o", help="write raw bytecode to .bin")
    cpl = sub.add_parser("compile-pseudo", help="legacy alias for 'compile'")
    cpl.add_argument("text", nargs="*", help="pseudo-code text; for multiline input prefer --file")
    cpl.add_argument("--file", help="read pseudo-code from a UTF-8 text file")
    cpl.add_argument("--boot", action="store_true", help="force BOOT schedule")
    cpl.add_argument("--output", "-o", help="write raw bytecode to .bin")
    ex = sub.add_parser("examples", help="show built-in semantic/programming examples")
    ex.add_argument("--limit", type=int, default=20, help="maximum examples to show")
    sub.add_parser("caps-help", help="show local capability catalog, board features and limitations without BLE")
    sub.add_parser("ha-help", help="show Home Assistant integration help without BLE")

    sub.add_parser("agent-list", help="list built-in AI semantic-agent providers")
    sub.add_parser("agent-status", help="show AI semantic-agent configuration")
    aset = sub.add_parser("agent-set", help="configure and enable an AI semantic-agent provider")
    aset.add_argument("provider", choices=sorted(provider_catalog().keys()))
    aset.add_argument("--model", help="provider model name")
    aset.add_argument("--base-url", help="provider API base URL")
    aset.add_argument("--api-key-env", help="environment variable containing API key; key itself is never stored")
    aset.add_argument("--timeout", type=int, default=45, help="HTTP timeout in seconds (default: 45)")
    sub.add_parser("agent-off", help="disable AI agent; AUTO prompt mode falls back to standalone")
    sub.add_parser("agent-kb-status", help="show automatic KB hash and external-agent reachability")
    sub.add_parser("agent-test", help="perform a real configured-provider API request and validate KB-aware JSON response")

    up = sub.add_parser(
        "upload"
    )

    up.add_argument(
        "file"
    )
    up.add_argument(
        "--boot", action="store_true",
        help="persist as generic BOOT program; auto-start after every reset"
    )

    bba = sub.add_parser("beacon-boot-test", help="persist a VM-driven Beacon BOOT test")
    bba.add_argument("--name", default="Teste-Claw")
    bba.add_argument("--interval", type=int, default=2000)
    bba.add_argument("--tx-power", type=int, default=4)
    bba.add_argument("--wait", type=int, default=5)

    sub.add_parser("boot-audit", help="audit generic BOOT scheduler and VM-capable device capabilities")

    ur = sub.add_parser(
        "upload-run"
    )

    ur.add_argument(
        "file"
    )

    tl = sub.add_parser(
        "test-battery-lora"
    )

    tl.add_argument(
        "--wait",
        type=int,
        default=5,
    )

    ie = sub.add_parser("inject-event")
    ie.add_argument("type", type=int)
    ie.add_argument("--arg0", type=int, default=0)
    ie.add_argument("--arg1", type=int, default=0)

    eg = sub.add_parser("test-event-gpio")
    eg.add_argument("--event", type=int, default=18, help="APP_COMMAND=18")
    eg.add_argument("--pin", type=int, default=15)
    eg.add_argument("--value", type=int, choices=[0,1], default=1)


    ss = sub.add_parser("state-save")
    ss.add_argument("state_key", metavar="key", type=int)
    ss.add_argument("value", type=lambda x: int(x, 0))

    sl = sub.add_parser("state-load")
    sl.add_argument("state_key", metavar="key", type=int)

    sg = sub.add_parser("state-get")
    sg.add_argument("state_key", metavar="key", type=int)

    tae = sub.add_parser("test-app-echo")
    tae.add_argument("--interval", type=int, default=1000)
    tae.add_argument("--payload", default="nRFClaw8")

    ts = sub.add_parser("test-serial")
    ts.add_argument("--tx", type=int, default=15)
    ts.add_argument("--rx", type=int, default=16)
    ts.add_argument("--baud", type=int, default=57600)
    ts.add_argument("--text", default="nRFClaw serial test\r\n")


    tr = sub.add_parser(
        "test-tracking",
        help="Stage 8.2 experimental OpenHaystack-compatible tracking test",
    )
    tr.add_argument(
        "--key",
        dest="key_hex",
        required=True,
        help="28-byte public key as 56 hexadecimal characters",
    )
    tr.add_argument(
        "--interval",
        type=int,
        default=1000,
        help="advertising interval in ms (100..10000)",
    )
    tr.add_argument(
        "--tx-power",
        type=int,
        default=0,
        help="advertising TX power in dBm",
    )

    sub.add_parser(
        "tracking-stop",
        help="Stop Stage 8.2 experimental tracking",
    )

    te = sub.add_parser(
        "tracking-enable",
        help=(
            "Enable autonomous Stage 8.2 OpenHaystack tracking; "
            "no external key file required"
        ),
    )
    te.add_argument(
        "--interval",
        type=int,
        default=1000,
        help="BLE advertising interval in ms (default: 1000)",
    )
    te.add_argument(
        "--tx-power",
        type=int,
        default=4,
        help="BLE advertising TX power in dBm (default: +4)",
    )
    te.add_argument(
        "--rotation",
        type=int,
        default=10800,
        help="key rotation interval in seconds (default: 10800 / 3 hours)",
    )

    sub.add_parser(
        "tracking-info",
        help="Show Stage 8.2 autonomous tracking identity/runtime information",
    )

    sub.add_parser(
        "tracking-identity",
        help=(
            "Export the autonomous tracking master identity "
            "(requires physical P0.21/NUS access)"
        ),
    )

    tx = sub.add_parser(
        "tracking-export",
        help=(
            "Export deterministic rotating tracking keys as a "
            "Macless-Haystack devices.json file (physical NUS only)"
        ),
    )
    tx.add_argument(
        "--count",
        type=int,
        default=250,
        help="number of rotating keys to export (1..250; default: 250)",
    )
    tx.add_argument(
        "--start-index",
        type=int,
        default=None,
        help="first tracking key index; default: current durable index",
    )
    tx.add_argument(
        "--name",
        default=None,
        help="accessory name stored in Macless-Haystack JSON",
    )
    tx.add_argument(
        "--output",
        default=None,
        help="output devices.json path",
    )

    tp = sub.add_parser(
        "tracking-provision",
        help=(
            "Generate keygen.nrfclaw.cloud provisioning URL/QR from the "
            "device tracking identity (physical NUS only)"
        ),
    )
    tp.add_argument(
        "--name",
        default=None,
        help="provisioned tracker name; default: nRFClaw-<device>",
    )
    tp.add_argument(
        "--qr-png", "--output",
        dest="output",
        default=None,
        help=(
            "optionally write QR PNG (requires external qrcode package); "
            "--output is retained as a legacy alias"
        ),
    )
    tp.add_argument(
        "--keygen-url",
        default=TRACKING_KEYGEN_URL,
        help=f"key generator base URL (default: {TRACKING_KEYGEN_URL})",
    )
    tp.add_argument(
        "--no-qr",
        action="store_true",
        help="print provisioning URL only; suppress the terminal Unicode QR",
    )

    sub.add_parser(
        "tracking-debug",
        help="Show Stage 8.3 tracking/radio diagnostic state",
    )

    ha = sub.add_parser(
        "ha",
        help="Reference client for Application BLE / Home Assistant NDP plane",
    )
    ha_sub = ha.add_subparsers(dest="ha_action", required=True)
    ha_sub.add_parser("scan", help="Diagnostic: list every BLE peripheral visible to Bleak/BlueZ")
    ha_sub.add_parser("info", help="Read board/NDP identity over Application GATT")
    ha_sub.add_parser(
        "device-info",
        help="Show consolidated NDP-v1 identity, capabilities, runtime and sensor snapshot",
    )
    ha_sub.add_parser("status", help="Read public runtime status over Application GATT")
    ha_sub.add_parser("battery", help="Read battery exactly through the HA/Application path")
    ha_sub.add_parser("sensors", help="Read the public sensor set used by Home Assistant")
    hr = ha_sub.add_parser("raw", help="Send one raw public NDP-SESSION request over Application GATT")
    hr.add_argument("opcode", help="NDP opcode, e.g. 0x20")
    hr.add_argument("--payload", default="", help="hex payload, e.g. '01 02' or '0102'")
    ha_sub.add_parser("test", help="Run reference Application-GATT/NDP compatibility checks")
    hsx = ha_sub.add_parser("stress", help="Run repeated NDP INFO/STATUS transactions on one Application connection")
    hsx.add_argument("--count", type=int, default=100, help="number of transaction pairs (default: 100)")
    hsx.add_argument("--verbose", action="store_true", help="print every successful iteration")
    hsx.add_argument("--keep-going", action="store_true", help="continue after individual failures")
    hrc = ha_sub.add_parser("reconnect", help="Validate repeated Application BLE connect/NDP/disconnect cycles")
    hrc.add_argument("--count", type=int, default=10, help="number of reconnect cycles (default: 10)")
    hrc.add_argument("--connect-retries", type=int, default=3, help="fresh-scan connect attempts per cycle (default: 3)")
    hrc.add_argument("--retry-delay", type=float, default=0.75, help="seconds before rescanning after a failed connect (default: 0.75)")
    hrc.add_argument("--keep-going", action="store_true", help="continue after a hard cycle failure")

    return p


def main():
    args = build_parser().parse_args()

    try:
        asyncio.run(
            main_async(args)
        )

    except KeyboardInterrupt:
        pass

    except Exception as exc:
        detail = str(exc) or repr(exc)
        print(
            f"ERROR: {type(exc).__name__}: {detail}",
            file=sys.stderr,
        )

        sys.exit(1)


if __name__ == "__main__":
    main()
