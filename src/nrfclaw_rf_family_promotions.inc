/* B8.1k — LaCrosse View family RFVM programs.
 * Source contract: rtl_433_ESP lacrosse_th3/lacrosse_wr1/lacrosse_breezepro.
 */

static const nrfclaw_rfvm_insn_t b81k_lacrosse_th_program[] = {
    I(NRFCLAW_RFVM_OP_CRC8_ZERO_ANY3,8,0x31,0x00,0xB2AC),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,3,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,7),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_OK,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-400),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,5,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_HUMIDITY,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t b81k_lacrosse_wr1_program[] = {
    I(NRFCLAW_RFVM_OP_CRC8_ZERO,11,0x31,0x00,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,25),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_DIV_IMM,0,0,0,9),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_SPEED_X100_MS,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,5,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_DIRECTION_DEG_V2,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t b81k_lacrosse_breeze_program[] = {
    I(NRFCLAW_RFVM_OP_CRC8_ZERO,11,0x31,0x00,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,4,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-400),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,5,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_HUMIDITY,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,7,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,25),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_DIV_IMM,0,0,0,9),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_SPEED_X100_MS,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,8,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_DIRECTION_DEG_V2,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k_rfvm_lacrosse_th = {
    19U,8U,b81k_lacrosse_th_program
};
static const nrfclaw_rfvm_profile_t b81k_rfvm_lacrosse_wr1 = {
    23U,11U,b81k_lacrosse_wr1_program
};
static const nrfclaw_rfvm_profile_t b81k_rfvm_lacrosse_breeze = {
    18U,11U,b81k_lacrosse_breeze_program
};

/* B8.1k2 — Fine Offset WH31L / WH57 Lightning.
 * Payload starts after the 2D D4 sync word.
 */
static const nrfclaw_rfvm_insn_t b81k2_wh31l_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,0,0,0,0x57),
    I(NRFCLAW_RFVM_OP_CRC8_ZERO,8,0x31,0x00,0),
    I(NRFCLAW_RFVM_OP_SUM8_EQ,8,8,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,1,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFFFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x06),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,50),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_PERCENT,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,6,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_LIGHTNING_COUNT,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,5,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x3F),
    I(NRFCLAW_RFVM_OP_STORE_IF_NE_IMM,0,
      NRFCLAW_RF_FIELD_LIGHTNING_DISTANCE_KM,0,63),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k2_rfvm_wh31l = {
    14U,9U,b81k2_wh31l_program
};

/* B8.1k3 — Fine Offset WH1080/WH3080 FSK.
 *
 * LLCC68 sync consumes 2D D4. RFVM sees the 10 bytes after sync.
 * rtl_433 sets synthetic br[0]=0xFF and CRC init 0xFF; because
 * 0xFF XOR 0xFF = 0 before the bit loop, this is equivalent to
 * CRC-8/0x31 init 0x00 over these 10 payload bytes.
 */
static const nrfclaw_rfvm_insn_t b81k3_wh1080_program[] = {
    /* Valid FSK family message types: weather A, datetime B, UV/light 7. */
    I(NRFCLAW_RFVM_OP_MATCH_MASK8_ANY3,0,0xF0,0xA0,
      (0x70 << 8) | 0xB0),
    I(NRFCLAW_RFVM_OP_CRC8_ZERO,10,0x31,0x00,0),

    /* r7 = message type nibble. */
    I(NRFCLAW_RFVM_OP_LOAD_U8,7,0,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,7,0,0,4),

    /* Common 8-bit station/sensor ID. */
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,0,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0xFF),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    /* Weather block (type 0xA): 27 instructions. */
    I(NRFCLAW_RFVM_OP_SKIP_IF_NE_IMM,7,27,0,0x0A),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,8,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_EQ_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_OK,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,1,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_SIGN_MAG_IMM,0,0,0,0x0800),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,3,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_HUMIDITY,0,0),

    /* rtl_433: raw * 0.34 m/s -> x100 m/s = raw * 34. */
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,4,0,0),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,34),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_SPEED_X100_MS,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,5,0,0),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,34),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_GUST_X100_MS,0,0),

    /* rtl_433: rain_raw * 0.3 mm -> x10 mm = raw * 3. */
    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,6,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,3),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_RAIN_TOTAL_X10_MM,0,0),

    /* Direction table is exactly round(nibble * 22.5 degrees):
     * (n*45 + 1)/2 reproduces 0,23,45,...,338.
     */
    I(NRFCLAW_RFVM_OP_LOAD_U8,0,8,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0F),
    I(NRFCLAW_RFVM_OP_MUL_IMM,0,0,0,45),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_DIV_IMM,0,0,0,2),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_WIND_DIRECTION_DEG_V2,0,0),

    /* UV/light block (type 0x7): 5 instructions. */
    I(NRFCLAW_RFVM_OP_SKIP_IF_NE_IMM,7,5,0,0x07),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,1,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0F),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_UV_INDEX,0,0),

    /* Source: lux = 24-bit light * 0.1. Keep x10 lux. */
    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,3,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ILLUMINANCE_X10_LUX,0,0),

    /* Datetime type 0xB intentionally publishes only common device ID. */
    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k3_rfvm_wh1080_868 = {
    12U,10U,b81k3_wh1080_program
};
static const nrfclaw_rfvm_profile_t b81k3_rfvm_wh1080_915 = {
    13U,10U,b81k3_wh1080_program
};

/* B8.1k4 — ThermoPro 915 MHz core telemetry.
 *
 * TP211B checksum: rtl_433's XOR table is equivalent to reflected CRC-16
 * poly 0xA001 over six data bytes plus one zero byte, then XOR 0x411B.
 */
static const nrfclaw_rfvm_insn_t b81k4_thermopro_tp211_program[] = {
    I(NRFCLAW_RFVM_OP_MATCH8,5,0,0,0xAA),
    I(NRFCLAW_RFVM_OP_CRC16_REFLECT_ZPAD,7,6,1,0xA001),
    I(NRFCLAW_RFVM_OP_XOR_IMM,7,0,0,0x411B),
    I(NRFCLAW_RFVM_OP_MATCH_REG_BE16,7,6,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE24,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,3,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,7),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,1),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_BATTERY_OK,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,3,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-500),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t b81k4_thermopro_tp28_program[] = {
    I(NRFCLAW_RFVM_OP_SUM8_EQ,16,16,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,14,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_BCD16_LE_X10,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_BCD16_LE_X10,0,6,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE2_X10,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_insn_t b81k4_thermopro_tp86_program[] = {
    I(NRFCLAW_RFVM_OP_CRC8_CALC,7,7,0x07,0x00),
    I(NRFCLAW_RFVM_OP_XOR_IMM,7,0,0,0xDB),
    I(NRFCLAW_RFVM_OP_MATCH_REG_U8,7,7,0,0),
    I(NRFCLAW_RFVM_OP_BYTES_AND_ZERO,7,8,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,2,0,0),
    I(NRFCLAW_RFVM_OP_SHR_IMM,0,0,0,4),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-500),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE_X10,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_BE16,0,3,0,0),
    I(NRFCLAW_RFVM_OP_AND_IMM,0,0,0,0x0FFF),
    I(NRFCLAW_RFVM_OP_ADD_IMM,0,0,0,-500),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_TEMPERATURE2_X10,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k4_rfvm_thermopro_tp211 = {
    33U,8U,b81k4_thermopro_tp211_program
};
static const nrfclaw_rfvm_profile_t b81k4_rfvm_thermopro_tp28 = {
    34U,18U,b81k4_thermopro_tp28_program
};
static const nrfclaw_rfvm_profile_t b81k4_rfvm_thermopro_tp86 = {
    35U,9U,b81k4_thermopro_tp86_program
};

/* B8.1k5 — SRSmith SRS-2C-TX.
 * rtl_433 accepts on CRC-16/0x8005 init FFFF over bytes 0..7.
 * Its byte-7 CRC8 is diagnostic only, so RFVM does not reject on it.
 */
static const nrfclaw_rfvm_insn_t b81k5_srsmith_program[] = {
    I(NRFCLAW_RFVM_OP_CRC16_CALC,7,8,0,0xFFFF8005UL),
    I(NRFCLAW_RFVM_OP_MATCH_REG_BE16,7,8,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,5,0,0),
    I(NRFCLAW_RFVM_OP_XOR_IMM,0,0,0,0xFF),
    I(NRFCLAW_RFVM_OP_REVERSE8,0,0,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),

    I(NRFCLAW_RFVM_OP_LOAD_U8,0,6,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_LAST_BUTTON_CODE,0,0),

    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k5_rfvm_srsmith = {
    30U,10U,b81k5_srsmith_program
};

/* B8.1k6c — Wireless M-Bus Mode C / Format B.
 *
 * LLCC68 variable packet mode consumes the one-byte L field.
 * The radio backend reconstructs it before this RFVM runs.
 *
 * Source acceptance mirrors rtl_433 m_bus_decode_format_b:
 * CRC-16/EN-13757 poly 0x3D65, init 0, final complement.
 */
static const nrfclaw_rfvm_insn_t b81k6c_wmbus_cb_program[] = {
    I(NRFCLAW_RFVM_OP_WMBUS_B_VALIDATE,0,0,0,0),
    I(NRFCLAW_RFVM_OP_BCD32_LE,0,4,0,0),
    I(NRFCLAW_RFVM_OP_STORE,0,NRFCLAW_RF_FIELD_ID,0,0),
    I(NRFCLAW_RFVM_OP_WMBUS_DIFVIF,0,0,0,0),
    I(NRFCLAW_RFVM_OP_END,0,0,0,0)
};

static const nrfclaw_rfvm_profile_t b81k6c_rfvm_wmbus_cb = {
    37U,0U,b81k6c_wmbus_cb_program
};
