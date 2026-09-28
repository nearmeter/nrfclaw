"""Small, HA-independent NDP v1 codec used by the Direct-NDP gate."""

from __future__ import annotations

from dataclasses import dataclass


class NDPProtocolError(ValueError):
    """Raised when an NDP frame or key is invalid."""


@dataclass(frozen=True, slots=True)
class NDPResponse:
    """Decoded NDP response."""

    opcode: int
    sequence: int
    status: int
    payload: bytes


def encode_request(opcode: int, sequence: int, payload: bytes = b"") -> bytes:
    """Encode one NDP request for the current ATT MTU=23 transport."""
    if not 0 <= opcode <= 0xFF:
        raise NDPProtocolError("opcode must fit in one byte")
    if not 0 <= sequence <= 0xFF:
        raise NDPProtocolError("sequence must fit in one byte")
    if len(payload) > 16:
        raise NDPProtocolError("NDP payload exceeds the 16-byte Application limit")
    return bytes((0x00, opcode, sequence, len(payload))) + payload


def decode_response(raw: bytes) -> NDPResponse:
    """Decode and validate one NDP response notification."""
    if len(raw) < 5:
        raise NDPProtocolError("NDP response is too short")

    opcode = raw[1]
    sequence = raw[2]
    payload_length = raw[3]
    if len(raw) != 4 + payload_length:
        raise NDPProtocolError(
            f"NDP length mismatch: header={payload_length} actual={len(raw) - 4}"
        )

    body = raw[4:]
    if not body:
        raise NDPProtocolError("NDP response has no status byte")

    return NDPResponse(
        opcode=opcode,
        sequence=sequence,
        status=body[0],
        payload=body[1:],
    )


def decode_access_key(value: str | None) -> bytes | None:
    """Decode the optional 256-bit NDP owner key."""
    value = (value or "").strip()
    if not value:
        return None

    if value.startswith("hex:"):
        try:
            key = bytes.fromhex(value[4:])
        except ValueError as exc:
            raise NDPProtocolError("invalid hexadecimal NDP access key") from exc
    else:
        key = value.encode("utf-8")

    if len(key) != 32:
        raise NDPProtocolError("NDP access key must be exactly 32 bytes")
    return key
