#ifndef NRFCLAW_BOARD_CONFIG_H
#define NRFCLAW_BOARD_CONFIG_H

#define NRFCLAW_BOARD_NAME       "NINASENSE"
#define NRFCLAW_BOARD_TYPE       1U
#define NRFCLAW_HW_REV_MAJOR     1U
#define NRFCLAW_HW_REV_MINOR     1U

/* Validated NINASENSE Rev 1.1 pin map. */
#define P_LORA_SCK       2
#define P_LORA_NSS       3
#define P_LORA_MOSI      4
#define P_LORA_MISO      5
#define P_LORA_BUSY      6
#define P_LORA_RXEN      7
#define P_LORA_TXEN      8
#define P_LORA_DIO1      9
#define P_LORA_DIO2      10
#define P_HALL1          11
#define P_LIS_SCL        13
#define P_HALL2          14
#define P_FREE_D15       15
#define P_FREE_D16       16
#define P_LIS_CS         17
#define P_LIS_SDA        18
#define P_LORA_NRST      19
#define P_DS18           20
#define P_BUTTON         21
#define P_LIS_INT1       22
#define P_LIS_INT2       23
#define P_BAT            28
#define P_FREE_A29       29
#define P_FREE_A30       30
#define P_FREE_A31       31
#define P_SERIAL_TX_DEFAULT P_FREE_D15
#define P_SERIAL_RX_DEFAULT P_FREE_D16

#define NRFCLAW_BOARD_HAS_LORA       1
#define NRFCLAW_BOARD_HAS_BATTERY    1
#define NRFCLAW_BOARD_HAS_DS18B20    1
#define NRFCLAW_BOARD_HAS_LIS2DH12   1
#define NRFCLAW_BOARD_HAS_HALL       1

/* Experimental: compiled in, idle until explicitly used. */
#define NRFCLAW_EXPERIMENTAL_VIB_HEALTH 1
#define NRFCLAW_EXPERIMENTAL_VIB_AUTO   1

/* Pack 02 native Home Assistant / Application BLE defaults.
 * Consumption and RF behavior must be validated independently per board. */
#define NRFCLAW_HA_NATIVE_ENABLE       1
#define NRFCLAW_HA_ADV_FAST_INTERVAL_MS   100U
#define NRFCLAW_HA_ADV_FAST_WINDOW_MS     3000U
#define NRFCLAW_HA_ADV_NORMAL_INTERVAL_MS 500U
#define NRFCLAW_HA_ADV_NORMAL_WINDOW_MS   30000U
#define NRFCLAW_HA_ADV_SLOW_INTERVAL_MS   2000U
/* Backward-compatible alias for older code/docs. */
#define NRFCLAW_HA_ADV_INTERVAL_MS      NRFCLAW_HA_ADV_SLOW_INTERVAL_MS
#define NRFCLAW_HA_ADV_TX_POWER_DBM     4

#endif
