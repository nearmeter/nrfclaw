# NinaLink Command Registry

B4.8 moves COMMAND dispatch out of the link transport into a dedicated
registry.

Commands frozen in this stage:

- `0x0001 ECHO_U32`
- `0x0002 GET_NODE_INFO`
- `0x0003 GET_TRACKING_STATE`

Each registry entry contains `command_id`, min/max argument length, maximum
result length, flags, and handler.

`CAP_SET` remains the way to change semantic state such as
`tracking_active`. `GET_TRACKING_STATE` is intentionally read-only.

## GET_NODE_INFO result

Eight bytes:

- byte 0: NinaLink protocol version (=1)
- byte 1: maximum RF frame (=64)
- byte 2: registry command count (=3)
- byte 3: feature flags
- bytes 4..7: node_id little-endian

Feature bits:

- bit0 CAP_SET
- bit1 reliable COMMAND
- bit2 command registry
- bit3 tracking

The reliable replay behavior remains in the transport. A repeated
`command_seq` returns the cached result without executing the registry handler
again.
