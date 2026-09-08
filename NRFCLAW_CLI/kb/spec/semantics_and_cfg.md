# Semantic and CFG rules

## State change
For requests such as "when GPIO changes", sample an initial previous state, sample current state in the loop, compare current vs previous, execute changed-side effects only on the changed outcome, update previous=current on the changed path, then continue monitoring.

## Threshold counters
Constants must be loaded into registers. Increment uses register.add with a register containing 1. A threshold comparison is semantically "after increment" when it is reachable from the increment in execution CFG, even if the compare action occurs earlier lexically and is reached through a backward jump.

## Loop semantics
`loop.actions` repeats forever. The semantic CFG contains an implicit edge from the end of the loop body to its beginning. A relay `lora.receive -> buffer.prepend -> ble.advertise_buffer` inside a loop is therefore continuous without an explicit jump.

## Branch truth table
`register.compare(dst,a,b,EQ)` => dst=1 if equal else 0. `jump_if_zero(dst,L)` takes L when not equal. Reason about taken/fall-through outcomes, not visual adjacency.

## Buffer model
There is one current BUFFER. receive operations populate it; prepend mutates it; set_literal replaces it; format operations replace it with formatted output; send/advertise operations consume current BUFFER.
