# nRFClaw semantic-agent contract

The agent translates free language into a constrained semantic plan. It NEVER emits bytecode or numeric opcodes. The deterministic compiler owns bytecode generation and validation.

Legacy plan JSON may use schedule BOOT|MANUAL. Semantic IR v1 program.mode supports MANUAL|BOOT|AT|EVERY|WEEKLY; AT/EVERY/WEEKLY carry program.schedule metadata. Return only fields needed by the selected intent. Set ndp_off=true only when the user explicitly asks to disable NDP/Application advertising before the program action.

Supported intents in R3.8.15 MVP: tracking, motion_tracking, temperature_flow, beacon.


## Schedule ABI (R3.8.20c2g3a-r1b)
Schedule is authenticated PROGRAM METADATA, not a VM opcode/action. Wire record is 12 bytes: mode:u8, dow_mask:u8, reserved:u16, arg0:u32, arg1:u32. Modes: MANUAL=0, AT=1, EVERY=2, WEEKLY=3, BOOT=4. AT: arg0 absolute UTC epoch. EVERY: arg0 interval seconds, arg1 anchor UTC epoch. WEEKLY: Monday=bit0..Sunday=bit6 in dow_mask, arg0 UTC seconds since midnight. Agents must represent schedules in program.mode/program.schedule and never claim schedule.at_time/RTC is missing merely because no action.op exists.

### Schedule-clause oracle
Explicit schedule language is a semantic requirement. `at boot`/startup/reset execution MUST lower to BOOT; explicit AT/EVERY/WEEKLY MUST preserve their mode-specific fields. A COMPILE result that preserves VM actions but changes or drops schedule metadata is invalid and must be repaired/rejected. Relative periodic action loops such as `send every 30 seconds` remain VM CFG unless the user explicitly requests program-level scheduling.
