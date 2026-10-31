#ifndef SI7021_I2C_H
#define SI7021_I2C_H

#include "../common/sample.h"

/*
 * Si7021 I2C access through the Linux i2c-dev driver (/dev/i2c-N). Linux
 * only; the byte conversions it uses live in si7021.h, which stays pure so
 * it builds and is tested anywhere.
 *
 * A reading is: command 0xF5 (measure humidity, no hold master), wait for
 * the conversion, read the 3-byte humidity reply, then command 0xE0 and
 * read the 2-byte temperature from that same measurement. While the
 * conversion is running the sensor NACKs its address, so the humidity read
 * is retried until it answers or the time limit passes.
 */

#define SI7021_I2C_ADDR       0x40
#define SI7021_FIRST_WAIT_MS  25   /* wait after 0xF5 before the first read */
#define SI7021_RETRY_MS       5    /* time between reads while the sensor NACKs */
#define SI7021_TIMEOUT_MS     100  /* give up this long after the 0xF5 command */

/*
 * Open the I2C bus `bus` (e.g. "/dev/i2c-1") and address the Si7021 at
 * SI7021_I2C_ADDR. Returns the file descriptor, or -1 with errno set.
 */
int si7021_open(const char *bus);

/* Close a descriptor from si7021_open(). Does nothing for a negative fd. */
void si7021_close(int fd);

/*
 * Take one humidity and temperature reading. Returns SAMPLE_OK and sets
 * both `*rh_pct` and `*temp_c`. On failure both are left unchanged and the
 * result is:
 *   SAMPLE_ERR_TIMEOUT  the sensor kept NACKing the humidity read for
 *                       SI7021_TIMEOUT_MS after the command
 *   SAMPLE_ERR_READ     any other I/O failure, a short read, a bad CRC, an
 *                       out-of-range temperature, or a NULL argument
 * Interrupted system calls (EINTR) are retried.
 */
sample_status_t si7021_read(int fd, float *rh_pct, float *temp_c);

#endif /* SI7021_I2C_H */
