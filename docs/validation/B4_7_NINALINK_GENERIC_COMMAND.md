# B4.7 — Generic COMMAND Downlink

Normal gate:

1. Start bridge.
2. Queue `ECHO_U32` token `0x12345678`.
3. Run one reliable node contact.
4. Expect a 24-byte COMMAND downlink, ACKED in one attempt.
5. Bridge command status must be DONE/OK, Sent=1, Completed=1, timeout=0,
   result token `0x12345678`.
6. Node command status must show Executed=1, Duplicate=0 and the same token.
7. COMMAND_RESULT is 23 bytes.

Reliable-result gate:

1. Arm `ninalink-node-drop-next-command-result`.
2. Queue ECHO token `0xA5A55A5A`.
3. First contact executes the command once but suppresses COMMAND_RESULT.
4. Bridge times out result RX and keeps the same command pending.
5. Second contact resends the same command sequence.
6. Node increments Duplicate, does not increment Executed, and returns cached
   result.
7. Bridge becomes DONE/OK with Sent=2, Completed=1, timeout=1.

Compatibility gate: with no CAP_SET/COMMAND pending, another reliable contact
must use the normal 18-byte ACK.
