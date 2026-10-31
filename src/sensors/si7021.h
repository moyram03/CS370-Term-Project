#ifndef SI7021_H
#define SI7021_H

#include <stddef.h>
#include <stdint.h>

#include "../common/sample.h"

/*
 * Si7021 humidity/temperature sensor support: the checksum and the
 * conversion of raw I2C replies into readings. Pure functions on byte
 * buffers, with no I2C or other I/O, so they can be tested without
 * hardware. The I2C access is in si7021_i2c.h.
 *
 * The sensor measures humidity first (command 0xE5, or 0xF5 for the no-hold
 * variant, which returns the same reply of 3 bytes: MSB, LSB, CRC). It then keeps the temperature from that same measurement,
 * which is read with command 0xE0 (reply of 2 bytes: MSB, LSB, no CRC).
 * In both replies the two low bits of the 16-bit code are status bits, not
 * part of the value, so they are masked off before converting.
 */

#define SI7021_TEMP_MIN_C  (-40)  /* lowest temperature the sensor is specified for, degrees C */
#define SI7021_TEMP_MAX_C  125    /* highest temperature the sensor is specified for, degrees C */

/*
 * CRC-8 of `data`: polynomial 0x31 (x^8 + x^5 + x^4 + 1), initial value
 * 0x00, most significant bit first, no reflection, no final XOR. Returns 0
 * (the initial value) for an empty input. A NULL `data` is treated as
 * empty, because a CRC cannot signal an error.
 */
uint8_t si7021_crc8(const uint8_t *data, size_t len);

/*
 * Convert a humidity reply `buf` (MSB, LSB, CRC) to percent relative
 * humidity: RH = 125 * code / 65536 - 6, where code is the 16-bit value
 * with its low 2 status bits cleared. The result is clamped to 0..100,
 * because the sensor can read a little below 0 or above 100 near the ends
 * of its range, and neither is a fault.
 *
 * Returns SAMPLE_OK and sets `*rh_pct`. Returns SAMPLE_ERR_READ, leaving
 * `*rh_pct` unchanged, if the CRC does not match the first two bytes or if
 * either argument is NULL.
 */
sample_status_t si7021_parse_rh(const uint8_t buf[3], float *rh_pct);

/*
 * Convert a temperature reply `buf` (MSB, LSB, no CRC) to degrees C:
 * T = 175.72 * code / 65536 - 46.85, with the low 2 status bits of code
 * cleared. This is the reply to command 0xE0, "temperature from the last
 * RH measurement". This reply has no CRC, so the only check on it is that
 * the result is inside SI7021_TEMP_MIN_C..SI7021_TEMP_MAX_C, inclusive.
 * Unlike humidity, an out-of-range temperature is rejected, not clamped:
 * it means the reply is garbage.
 *
 * Returns SAMPLE_OK and sets `*temp_c`. Returns SAMPLE_ERR_READ, leaving
 * `*temp_c` unchanged, if the result is out of range or either argument is
 * NULL.
 */
sample_status_t si7021_parse_temp(const uint8_t buf[2], float *temp_c);

#endif /* SI7021_H */
