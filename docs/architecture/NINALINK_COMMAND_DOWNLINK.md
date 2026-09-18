# NinaLink v1 Generic COMMAND Downlink

B4.7 freezes the remaining NinaLink v1 application-control message types:

- `0x30 CAP_SET`
- `0x31 COMMAND`
- `0x32 COMMAND_RESULT`

`CAP_SET` writes semantic state. `COMMAND` is an RPC-style operation.

COMMAND payload:
`reply_to_seq:u16, command_id:u16, arg_len:u8, args[arg_len]`.

COMMAND_RESULT payload:
`command_seq:u16, status:u8, result_len:u8, result[result_len]`.

The header `node_id` of COMMAND is the target node. `reply_to_seq` implicitly
acknowledges the uplink that opened the node's short receive window.

Status values are 0=OK, 1=UNSUPPORTED, 2=BAD_ARGS, 3=EXEC_FAILED.

The first command registry entry is `0x0001 ECHO_U32`: one u32 little-endian
argument and the same u32 returned as result.

Reliability reuses the B4.6 model. A pending command keeps the same
`command_seq` after result timeout. On a later uplink it may carry a new
`reply_to_seq`, but the node recognizes the original `command_seq`, does not
execute it twice, and retransmits its cached COMMAND_RESULT.
