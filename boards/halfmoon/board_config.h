#ifndef NRFCLAW_HALFMOON_BOARD_CONFIG_H
#define NRFCLAW_HALFMOON_BOARD_CONFIG_H

/*
 * HALFMOON inherits NINASENSE capabilities, HA adaptive advertising,
 * sensor enablement and defaults. Only board identity and physical pin map
 * differ here.
 */
#include "../ninasense/board_config.h"

#undef NRFCLAW_BOARD_NAME
#undef NRFCLAW_BOARD_TYPE
#undef NRFCLAW_HW_REV_MAJOR
#undef NRFCLAW_HW_REV_MINOR
#define NRFCLAW_BOARD_NAME       "HALFMOON"
#define NRFCLAW_BOARD_TYPE       3U
#define NRFCLAW_HW_REV_MAJOR     1U
#define NRFCLAW_HW_REV_MINOR     0U

#undef P_LORA_SCK
#undef P_LORA_NSS
#undef P_LORA_MOSI
#undef P_LORA_MISO
#undef P_LORA_BUSY
#undef P_LORA_RXEN
#undef P_LORA_TXEN
#undef P_LORA_DIO1
#undef P_LORA_DIO2
#undef P_HALL1
#undef P_LIS_SCL
#undef P_HALL2
#undef P_FREE_D15
#undef P_FREE_D16
#undef P_LIS_CS
#undef P_LIS_SDA
#undef P_LORA_NRST
#undef P_DS18
#undef P_BUTTON
#undef P_LIS_INT1
#undef P_LIS_INT2
#undef P_BAT
#undef P_FREE_A29
#undef P_FREE_A30
#undef P_FREE_A31
#undef P_SERIAL_TX_DEFAULT
#undef P_SERIAL_RX_DEFAULT

#define P_LORA_SCK       4
#define P_LORA_NSS       5
#define P_LORA_MOSI      6
#define P_LORA_MISO      7
#define P_LORA_BUSY      8
#define P_LORA_RXEN      9
#define P_LORA_TXEN      10
#define P_LORA_DIO1      13
#define P_LORA_DIO2      20

#define P_HALL1          12
#define P_LIS_SCL        28
#define P_HALL2          31

#define P_FREE_D15       15
#define P_FREE_D16       16
#define P_LIS_CS         25
#define P_LIS_SDA        23
#define P_LORA_NRST      21
#define P_DS18           20
#define P_BUTTON         22
#define P_LIS_INT1       29
#define P_LIS_INT2       30

#define P_BAT            2
#define P_FREE_A29       27
#define P_FREE_A30       26
#define P_FREE_A31       17
#define P_SERIAL_TX_DEFAULT P_FREE_D15
#define P_SERIAL_RX_DEFAULT P_FREE_D16

#endif
