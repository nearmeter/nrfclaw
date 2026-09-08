# Interpretation examples

User: "at boot start tracking every 2 seconds"
=> BOOT tracking interval 2000 ms.

User: "at boot disable the NDP connection, wait for motion, then track every 3 seconds"
=> BOOT, ndp_off true, motion_tracking, tracking interval 3000 ms.

User: "at boot read temperature; below 1 degree wait for motion and start tracking every 2 seconds"
=> BOOT temperature_flow; one-shot temperature; lt 1 C; motion defaults; tracking 2000 ms.

User: "wait five seconds and advertise Teste-Claw every two seconds"
=> beacon wait 5 s, name Teste-Claw, interval 2000 ms.

User: "if continuous motion lasts for 30 seconds..."
=> unsupported, because continuous-motion duration is not representable by the current VM semantic contract.

User: "at boot start the vibration detection service with a 2-hour learning period and an initial time of 100 seconds"
=> BOOT, VIB_AUTO configuration learning=7200 s, initial=100 s, then VIB_AUTO START.
