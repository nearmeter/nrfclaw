# NinaLink v1 Application Downlink

B4.5 allocates NinaLink message type `0x30` to `CAP_SET`.

The first writable semantic capability is:

```text
0x0401 tracking_active
channel 0
type BOOL
value 0/1
```

## CAP_SET wire payload

For downlink control messages the header `node_id` identifies the target node.

`CAP_SET` payload is seven bytes:

```text
offset  size  field
0       2     reply_to_seq, little-endian
2       2     capability_id, little-endian
4       1     channel
5       1     value_type
6       1     BOOL value
```

A tracking CAP_SET frame is therefore 22 bytes total:

```text
13 header + 7 payload + 2 CRC
```

`ACK_REQ` is set.

## Implicit uplink ACK

A sleeping node only listens immediately after its uplink. To avoid spending
energy on two separate bridge transmissions, a pending `CAP_SET` replaces the
ordinary ACK.

The `reply_to_seq` field names the uplink being acknowledged. A node accepts
the CAP_SET only when `reply_to_seq` equals its outstanding uplink sequence.

Thus:

```text
node CAP_REPORT seq=N ACK_REQ
           ->
bridge CAP_SET cmd=M reply_to=N
           ->
node ACK ack_seq=M application_status
```

The CAP_SET is simultaneously:

1. proof that the bridge received uplink N;
2. an application command;
3. a request for an application-result ACK.

## Application result status

The ACK status byte is:

```text
0 OK
1 UNSUPPORTED_CAP
2 BAD_TYPE
3 BAD_VALUE
4 APPLY_FAILED
```

The bridge retains an unconfirmed command. If its result ACK is lost, the same
CAP_SET sequence is offered again on a later node uplink. The node caches its
most recent command sequence and re-ACKs a duplicate without reapplying it.
