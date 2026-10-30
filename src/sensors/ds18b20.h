#ifndef DS18B20_H
#define DS18B20_H

#include <stddef.h>

#include "../common/sample.h"

/*
 * DS18B20 temperature sensor support: a parser for the Linux 1-Wire
 * driver's w1_slave text, plus helpers to find the device and read its
 * file. ds18b20_parse() is pure string work with no file I/O, so it can be
 * tested without hardware. ds18b20_find() and ds18b20_read() touch the
 * filesystem, so they can be tested against a fake directory.
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

/*
 * Find the DS18B20 in the 1-Wire device folder `w1_dir` (normally
 * /sys/bus/w1/devices) and write the path of its w1_slave file into
 * `path_out`, which has room for `len` bytes. DS18B20 devices are the
 * entries named "28-*"; everything else, such as the "00-*" ghost entries
 * the driver sometimes creates, is ignored. If there are several, the one
 * whose name sorts first is used, so the choice does not depend on the
 * order the filesystem lists them in. Whether the w1_slave file exists is
 * not checked here; ds18b20_read() reports that.
 *
 * Returns 0 on success. Returns -1 and sets `path_out[0]` to '\0' (when it
 * can) if `w1_dir` cannot be read, there is no "28-*" entry, an argument is
 * NULL or `len` is 0, or the path does not fit in `len` bytes.
 */
int ds18b20_find(const char *w1_dir, char *path_out, size_t len);

/*
 * Read the w1_slave file at `slave_path` and parse it with
 * ds18b20_parse(). The file is read into a fixed buffer, and a file of
 * DS18B20_READ_MAX_BYTES or more counts as an error. Returns SAMPLE_ERR_READ
 * if the file cannot be opened or read, is too large, or fails to parse;
 * `*temp_c` is left unchanged in that case. Reading a real w1_slave takes
 * about 750 ms, because the sensor does a temperature conversion first.
 */
#define DS18B20_READ_MAX_BYTES 255

sample_status_t ds18b20_read(const char *slave_path, float *temp_c);

#endif /* DS18B20_H */
