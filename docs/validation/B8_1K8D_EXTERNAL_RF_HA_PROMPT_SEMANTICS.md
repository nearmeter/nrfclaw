# B8.1k8d — Natural External-RF → Home Assistant Semantics

Natural sensor names now select the already-existing runtime RF profiles and
provision a NINASENSE external-RF Home Assistant bridge.

```text
prompt -> exact runtime profile -> persist EXTERNAL_RF -> persist HA BRIDGE
       -> reboot -> LLCC68 ingest -> RFVM -> virtual NinaLink node -> HA
```

Runtime prompt profiles: `1,2,3,4,12,13,14,18,19,23,30,33,34,35,37`.
WH1080/WH3080 requires 868.3 or 915 MHz. Profiles 24, 25, 27 and 28 are rejected
(mixed W-MBus, deferred W-MBus, retired/reserved NetAtmo, Manchester-blocked
Oil STANDARD).

Examples:

```text
At boot, use this NINASENSE as a Home Assistant RF bridge for Ecowitt WH51 sensors.
At boot, receive Ambient WH31E sensors and expose their readings in Home Assistant.
At boot, receive Fine Offset WH1080 sensors on 915 MHz and expose them in Home Assistant.
At boot, receive WH31L lightning sensors and show them in Home Assistant.
At boot, receive ThermoPro TP28B dual-probe thermometers and expose both probes in Home Assistant.
At boot, receive Wireless M-Bus Mode C Format B meters and expose decoded data in Home Assistant.

Configure este NINASENSE para receber sensores WH31E e mostrar no Home Assistant.
Use meu WH51 como sensor externo e publique no Home Assistant.
Receba o sensor de raios WH31L e mostre no Home Assistant.
Use o TP28B e mostre as duas sondas no Home Assistant.
Configure como gateway para medidores Wireless M-Bus Mode C Format B.
```

Developer form: `At boot, receive external RF profile 3 and expose it in Home Assistant.`

The RF profile registry remains authoritative for PHY/framing/RFVM. This is a
host/compiler-only gate: no firmware C, RFVM, wire ABI or HA entity mapping
changes. `prompt --upload` is required because raw VM bytecode cannot select
EXTERNAL_RF ownership.
