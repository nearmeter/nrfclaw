# Semantic schema v1

Examples of valid shapes:

{"schema_version":1,"supported":true,"schedule":"BOOT","ndp_off":false,"intent":"tracking","tracking":{"interval_ms":2000,"tx_power_dbm":4}}

{"schema_version":1,"supported":true,"schedule":"BOOT","ndp_off":true,"intent":"motion_tracking","motion":{"threshold_mg":250,"duration_ms":100},"tracking":{"interval_ms":2000,"tx_power_dbm":4}}

{"schema_version":1,"supported":true,"schedule":"BOOT","ndp_off":true,"intent":"temperature_flow","temperature":{"operator":"lt","threshold_c":1,"interval_ms":null},"motion":{"threshold_mg":250,"duration_ms":100},"tracking":{"interval_ms":2000,"tx_power_dbm":4}}

{"schema_version":1,"supported":true,"schedule":"BOOT","ndp_off":false,"intent":"beacon","beacon":{"name":"Teste-Claw","interval_ms":2000,"tx_power_dbm":4,"wait_s":5}}

If semantics are not representable, return: {"schema_version":1,"supported":false,"error":"reason"}.
