#ifndef DS18B20_H
#define DS18B20_H

#include "../common/sample.h"

/*
 * Parser for the DS18B20 temperature sensor's w1_slave text. Pure string
 * work: no file I/O, so it can be tested without hardware. The sensor
 * reader process reads the file and passes the text in.
 *
 * The Linux 1-Wire driver gives two lines. Each line starts with the nine
 * bytes of the sensor's scratchpad, in hex. The first line ends with the
 * kernel's CRC verdict and the second ends with the temperature in
 * thousandths of a degree C:
 *
 *   72 01 4b 46 7f ff 0e 10 57 : crc=57 YES
 *   72 01 4b 46 7f ff 0e 10 57 t=23125
 */

#define DS18B20_MIN_C  (-55)  /* lowest temperature the sensor can measure, degrees C */
#define DS18B20_MAX_C  125    /* highest temperature the sensor can measure, degrees C */

/* What the sensor reports right after power-on, before its first
 * conversion finishes: exactly 85.000 degrees C, in thousandths. */
#define DS18B20_POWER_ON_MILLI_C  85000

/*
 * Parse w1_slave `text`. On success, sets `*temp_c` to the temperature in
 * degrees C and returns SAMPLE_OK. Otherwise returns SAMPLE_ERR_READ and
 * leaves `*temp_c` unchanged. This function only returns those two values;
 * a timeout is for the reader process to report.
 *
 * SAMPLE_ERR_READ is returned when:
 *   - `text` or `temp_c` is NULL, or the text is empty or has only one line;
 *   - the first line does not end in "YES" (the kernel's CRC check failed);
 *   - the second line has no "t=", or "t=" is not followed by a number
 *     (an optional '-', then digits) and nothing but whitespace after it;
 *   - the temperature is exactly 85.000 C (the power-on value, not a real
 *     reading; a genuine 85.000 C reading is rejected too, which is fine for
 *     a washing machine area);
 *   - the temperature is outside DS18B20_MIN_C..DS18B20_MAX_C;
 *   - the nine scratchpad bytes on the second line are all 00. A data line
 *     stuck low reads as all zeros, which has CRC 00 and so passes the
 *     kernel's CRC check with "YES", but it is not a real reading. A real
 *     0 C reading still has nonzero bytes, such as the 4b 46 7f ff 0c 10
 *     configuration and reserved bytes. If the second line does not have
 *     exactly nine hex bytes before "t=", this check does not apply.
 */
sample_status_t ds18b20_parse(const char *text, float *temp_c);

#endif /* DS18B20_H */
