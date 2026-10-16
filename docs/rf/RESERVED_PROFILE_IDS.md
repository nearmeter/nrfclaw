# Reserved RF profile IDs

## Profile 27

Profile ID **27** is reserved permanently.

It was used by the retired NetAtmo experimental runtime. B8.1k8a removes that
experiment from active RF runtime, RFVM, LLCC68 diagnostics, CLI and Home
Assistant presentation.

The stable historical ID assignment remains intentionally present so old logs,
captures, persisted references and validation material cannot be reinterpreted
as another protocol.

Rules:

- profile 27 MUST NOT appear in the active RF runtime registry;
- profile 27 MUST NOT be presented as a supported Home Assistant RF profile;
- profile 27 MUST NOT be accepted by synthetic runtime TX;
- profile 27 MUST NOT be reassigned to another protocol.
