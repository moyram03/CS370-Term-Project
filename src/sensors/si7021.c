#include "si7021.h"

#define CRC_POLY    0x31u    /* x^8 + x^5 + x^4 + 1 */
#define STATUS_MASK 0xFFFCu  /* clears the 2 status bits at the bottom of the code */

/* The 16-bit code from an MSB, LSB pair, with the status bits cleared. */
static unsigned raw_code(uint8_t msb, uint8_t lsb)
{
    return (((unsigned)msb << 8) | lsb) & STATUS_MASK;
}

uint8_t si7021_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ CRC_POLY);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

sample_status_t si7021_parse_rh(const uint8_t buf[3], float *rh_pct)
{
    if (buf == NULL || rh_pct == NULL) {
        return SAMPLE_ERR_READ;
    }
    if (si7021_crc8(buf, 2) != buf[2]) {
        return SAMPLE_ERR_READ;
    }

    double rh = 125.0 * (double)raw_code(buf[0], buf[1]) / 65536.0 - 6.0;
    if (rh < 0.0) {
        rh = 0.0;
    }
    if (rh > 100.0) {
        rh = 100.0;
    }
    *rh_pct = (float)rh;
    return SAMPLE_OK;
}

sample_status_t si7021_parse_temp(const uint8_t buf[2], float *temp_c)
{
    if (buf == NULL || temp_c == NULL) {
        return SAMPLE_ERR_READ;
    }

    double t = 175.72 * (double)raw_code(buf[0], buf[1]) / 65536.0 - 46.85;
    if (t < SI7021_TEMP_MIN_C || t > SI7021_TEMP_MAX_C) {
        return SAMPLE_ERR_READ;
    }
    *temp_c = (float)t;
    return SAMPLE_OK;
}
