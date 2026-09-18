# NinaLink Command Discovery / Registry Advertisement

B4.9 makes the B4.8 command registry discoverable over NinaLink.

New NinaLink v1 message types:

```text
0x33 COMMANDS_REQUEST
0x34 COMMANDS_RESPONSE
```

This is a protocol feature, not another application COMMAND. A bridge can
therefore discover the command contract of a node without knowing any command
IDs in advance.

## COMMANDS_REQUEST

The bridge sends the request inside the receive window opened by a normal node
uplink. The request replaces the ordinary ACK and implicitly acknowledges the
uplink through `reply_to_seq`.

Header:

```text
message_type = 0x33
node_id      = target node
sequence     = discovery request sequence
ACK_REQ      = 1
```

Payload (3 bytes):

```text
reply_to_seq : u16 LE
start_index  : u8
```

Total frame size: 18 bytes.

## COMMANDS_RESPONSE

The node responds with `0x34`.

Header:

```text
node_id  = responding node
sequence = discovery request sequence
```

Payload:

```text
registry_version : u8
start_index      : u8
total_count      : u8
count            : u8
descriptor[0..count-1]
```

Each descriptor is 6 bytes:

```text
command_id : u16 LE
min_args   : u8
max_args   : u8
max_result : u8
flags      : u8
```

Registry version B4.9 is `1`.

Seven descriptors fit in one 64-byte NinaLink frame:

```text
4 bytes metadata + 7 * 6 bytes = 46-byte payload
13-byte header + 46 + 2-byte CRC = 61 bytes
```

The current B4.8 registry has three entries, so page zero is 37 bytes total.

## Pagination

`start_index` is an index into the registry. The response carries both
`total_count` and `count`.

The next page begins at:

```text
start_index + count
```

when that value is smaller than `total_count`.

A request at or beyond the end returns a valid empty page (`count=0`).

## Reliability

Discovery reuses the B4.6-B4.8 result reliability mechanism.

If a COMMANDS_RESPONSE is lost:

1. the bridge keeps the same discovery request sequence pending;
2. the next node uplink opens another receive window;
3. the bridge sends the same request sequence again, with a new
   `reply_to_seq`;
4. the node records it as a duplicate discovery request;
5. the registry page is regenerated and returned;
6. the bridge completes discovery only after receiving a valid response.

Discovery is read-only, so regeneration is safe and deterministic.
