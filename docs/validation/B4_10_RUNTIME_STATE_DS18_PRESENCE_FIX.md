# B4.10 — Runtime-state / DS18B20 presence fix

Hardware tests exposed two semantic problems.

1. DS18B20 could advertise PRESENT on an empty connector and return 0.000 C.
   Presence is revalidated at init using READ ROM (0x33), requiring family
   0x28 and a valid ROM CRC. An all-zero scratchpad is rejected.

2. ACCELERATION_X/Y/Z advertised ENABLED whenever LIS2DH was physically
   present. They now advertise ENABLED only while LIS2DH mode != OFF.

Expected F07138 without DS18B20:
- ds18b20: Present=no
- B4.10 page 0: TEMPERATURE state=SUPPORTED

With LIS2DH OFF:
- ACCELERATION_X/Y/Z state=SUPPORTED|PRESENT

With MOTION configured:
- MOTION state=SUPPORTED|PRESENT|ENABLED
- ACCELERATION_X/Y/Z state=SUPPORTED|PRESENT|ENABLED
