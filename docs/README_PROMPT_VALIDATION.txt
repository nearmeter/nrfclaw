PROMPT: Read the temperature every 30 seconds and send it over LoRa.
SEMANTIC: deterministic hybrid-v32 route=compositional family=lora-temperature-stream
PSEUDO-CODE:
PROGRAM BOOT
LABEL LOOP
DS18 READ -> R0
FORMAT BUFFER "TEMP=" + R0 as CELSIUS
LORA SEND BUFFER
WAIT 30s
JMP LOOP
END
PROGRAM BOOT
INTENT lora-temperature-stream
  LABEL LOOP
  DS18 READ -> R0
  FORMAT BUFFER "TEMP=" + R0 as CELSIUS
  LORA SEND BUFFER
  WAIT 30s
  JMP LOOP
  END
WARNINGS
  - No explicit dynamic payload prefix was supplied; using "TEMP=".
  - Temperature is read from DS18B20 immediately before each LoRa transmission.
BYTECODE (21 bytes): 5d 00 63 05 54 45 4d 50 3d 00 03 64 11 1e 00 00 00 0b ec ff 00
SCHEDULE ABI: BOOT mode=4 dow_mask=0x00 arg0=0 arg1=0
SCHEDULE WIRE (12 bytes): 04 00 00 00 00 00 00 00 00 00 00 00
Validation: OK (deterministic compiler; no device upload requested)
PASS

PROMPT: Read temperature and battery, format both values in one message, and send it over LoRa every 10 minutes, repeating continuously.
SEMANTIC: deterministic hybrid-v32 route=compositional family=temp-battery-periodic-lora
PSEUDO-CODE:
PROGRAM BOOT
LABEL LOOP
DS18 READ -> R0
BAT READ -> R1
FORMAT BUFFER "T=" + R0 AS CELSIUS + ",B=" + R1 AS VOLTS
LORA SEND BUFFER
WAIT 600s
JMP LOOP
END
PROGRAM BOOT
INTENT temp-battery-periodic-lora
  LABEL LOOP
  DS18 READ -> R0
  BAT READ -> R1
  FORMAT BUFFER "T=" + R0 AS CELSIUS + ",B=" + R1 AS VOLTS
  LORA SEND BUFFER
  WAIT 600s
  JMP LOOP
  END
WARNINGS
  - Both sensors are sampled immediately before each LoRa transmission.
BYTECODE (26 bytes): 5d 00 30 01 88 02 54 3d 00 03 03 2c 42 3d 01 02 64 11 58 02 00 00 0b e7 ff 00
SCHEDULE ABI: BOOT mode=4 dow_mask=0x00 arg0=0 arg1=0
SCHEDULE WIRE (12 bytes): 04 00 00 00 00 00 00 00 00 00 00 00
Validation: OK (deterministic compiler; no device upload requested)
PASS

PROMPT: At boot, disable everything to test minimum power consumption.
SEMANTIC: deterministic hybrid-v32 route=semantic-goal family=minimum-power-test
SEMANTIC GOAL: MINIMUM_POWER_TEST preserve=P0.21
PSEUDO-CODE:
PROGRAM BOOT
SYSTEM MINIMUM POWER
END
PROGRAM BOOT
INTENT minimum-power-test
  SYSTEM MINIMUM POWER
  END
WARNINGS
  - Minimum-power test defaults to BOOT so current can be measured after reset with NUS disconnected.
  - P0.21 programming/wake remains enabled by firmware policy.
BYTECODE (2 bytes): 84 00
SCHEDULE ABI: BOOT mode=4 dow_mask=0x00 arg0=0 arg1=0
SCHEDULE WIRE (12 bytes): 04 00 00 00 00 00 00 00 00 00 00 00
Validation: OK (deterministic compiler; no device upload requested)
PASS

PROMPT: At boot, enable tracking every 5 seconds.
SEMANTIC: deterministic hybrid-v32 route=legacy-b8 family=tracking
PSEUDO-CODE:
PROGRAM BOOT
TRACKING CONFIG interval=5000ms tx=+4dBm
TRACKING ROTATION 10800s
TRACKING START
END
PROGRAM BOOT
INTENT tracking
  TRACKING CONFIG interval=5000ms tx=+4dBm
  TRACKING ROTATION 10800s
  TRACKING START
  END
BYTECODE (11 bytes): 78 88 13 04 7c 30 2a 00 00 79 00
SCHEDULE ABI: BOOT mode=4 dow_mask=0x00 arg0=0 arg1=0
SCHEDULE WIRE (12 bytes): 04 00 00 00 00 00 00 00 00 00 00 00
Validation: OK (deterministic compiler; no device upload requested)
PASS

PROMPT: At boot, activate VIB_AUTO on the machine with learning 2 hours and initial delay 1 minute; if vibration becomes abnormal, notify Home Assistant.
SEMANTIC: deterministic hybrid-v32 route=compositional family=vib-auto-ha-anomaly
PSEUDO-CODE:
PROGRAM BOOT
BLE NDP ON
VIB_AUTO CONFIG learning=7200s initial=60s
VIB_AUTO START
LABEL vib_alarm_loop
WAIT_EVENT VIB_AUTO.ALARM
HA EVENT cap=13 op=4 value=R7
JMP vib_alarm_loop
END
PROGRAM BOOT
INTENT vib-auto-ha-anomaly
  PROGRAM BOOT
  BLE NDP ON
  VIB_AUTO CONFIG learning=7200s initial=60s
  VIB_AUTO START
  LABEL vib_alarm_loop
  WAIT_EVENT VIB_AUTO.ALARM
  HA EVENT cap=13 op=4 value=R7
  JMP vib_alarm_loop
  END
WARNINGS
  - Natural vibration-difference wording maps to VIB_AUTO.ALARM.
  - VIB_AUTO profile uses learning=7200s and initial=60s; explicit prompt values override the 3600s/60s defaults.
  - Home Assistant notification is emitted through the Application/NDP event mailbox.
BYTECODE (25 bytes): 70 02 83 20 1c 00 00 3c 00 00 00 80 00 12 43 06 07 68 0d 04 07 0b f5 ff 00
SCHEDULE ABI: BOOT mode=4 dow_mask=0x00 arg0=0 arg1=0
SCHEDULE WIRE (12 bytes): 04 00 00 00 00 00 00 00 00 00 00 00
Validation: OK (deterministic compiler; no device upload requested)
PASS

