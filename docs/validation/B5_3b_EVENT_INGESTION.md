# B5.3b — CAP_EVENT Ingestion / Ephemeral Event Journal

B5.3b closes the event side of the B5.3 internal consumer.

NinaLink v1 deliberately distinguishes CAP_EVENT from CAP_REPORT. Events such
as TAP and FALL are momentary occurrences and must not become persistent state.
B5.3b therefore keeps the existing persistent cache for CAP_REPORT values and
adds a bounded event journal for CAP_EVENT entries.

Flow:

    node synthetic CAP_EVENT (ACK_REQ)
      -> B4.4 validated admission / dedup
      -> B5.3 internal consumer
      -> event journal (depth 8)
      -> ACK

Persistent state is not modified by CAP_EVENT.

Lab event emitter:

    ninalink-event-test --event tap
      CAP_EVENT: TAP[0] ENUM8=2

    ninalink-event-test --event fall
      CAP_EVENT: FALL[0] BOOL=true

Bridge inspection:

    ninalink-events --node 0xAD64D423

Cache reset without destroying the B5.2 discovery registry:

    ninalink-cache-clear

Hardware gate temporarily disables B5.2 auto-discovery to isolate event ingestion, then uses one normal CAP_REPORT followed by one synthetic TAP event. Auto-discovery is re-enabled after evidence collection.
Expected state cache after the pair:

    Values=2
    Frame updates=2
    Reports=1
    Events=1
    Last message=CAP_EVENT

BATTERY_VOLTAGE and TEMPERATURE remain the two persistent values. TAP appears
only in the event history as ENUM8=2. Event history must report one entry and
zero drops/evictions.
