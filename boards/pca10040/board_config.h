#ifndef NRFCLAW_BOARD_CONFIG_H
#define NRFCLAW_BOARD_CONFIG_H

#define NRFCLAW_BOARD_NAME       "PCA10040"
#define NRFCLAW_BOARD_TYPE       2U
#define NRFCLAW_HW_REV_MAJOR     1U
#define NRFCLAW_HW_REV_MINOR     0U

/*
 * Nordic nRF52 DK compile profile.  Core/BLE build target only in Pack 01.
 * External sensor/radio wiring below is a safe compile mapping, NOT a
 * hardware validation statement.  Functional pin maps will be validated
 * per-board in later board profiles/tests.
 */
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
#define P_HALL2          12
#define P_BUTTON         13
#define P_LIS_SCL        14
#define P_LIS_SDA        15
#define P_LIS_CS         16
#define P_LIS_INT1       17
#define P_LIS_INT2       18
#define P_LORA_NRST      19
#define P_DS18           20
#define P_BAT            28
#define P_FREE_D15       22
#define P_FREE_D16       23
#define P_FREE_A29       29
#define P_FREE_A30       30
#define P_FREE_A31       31
#define P_SERIAL_TX_DEFAULT P_FREE_D15
#define P_SERIAL_RX_DEFAULT P_FREE_D16

#define NRFCLAW_BOARD_HAS_LORA       0
#define NRFCLAW_BOARD_HAS_BATTERY    0
#define NRFCLAW_BOARD_HAS_DS18B20    0
#define NRFCLAW_BOARD_HAS_LIS2DH12   0
#define NRFCLAW_BOARD_HAS_HALL       0

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
