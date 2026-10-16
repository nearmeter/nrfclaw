# B8.1k6g — RF Virtual-Node Inventory → HA Projection Corrective

Evidence before the fix:

- profile 37 RF/RFVM/NinaLink path passes;
- virtual node is `0xF825614E`;
- Application inventory returns valid/complete/count=1 for `0x0900`;
- external model omitted B7.2 inventory fields;
- HA preview returned `devices=[]`.

Root cause: `read_inventory(node_id)` remained implemented but was no
longer attached to each node by the current external-model builder.

This corrective restores that attachment in both the CLI and packaged
Home Assistant external-model builders. No firmware/radio/RFVM/NDP ABI
change is made. `HARDWARE_VALIDATED` remains false pending a real meter.
