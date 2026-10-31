/* Needed for nanosleep, clock_gettime, and O_CLOEXEC under -std=c17. */
#define _XOPEN_SOURCE 700

#include "si7021_i2c.h"
#include "si7021.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CMD_MEASURE_RH_NO_HOLD  0xF5
#define CMD_READ_TEMP_FROM_RH   0xE0
#define NS_PER_MS               1000000LL

int si7021_open(const char *bus)
{
    int fd = open(bus, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, SI7021_I2C_ADDR) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

void si7021_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

/* Monotonic clock in milliseconds. Returns 0, or -1 on failure. */
static int mono_ms(int64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    *out = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / NS_PER_MS;
    return 0;
}

/* Sleep `ms` milliseconds, resuming with the time left after a signal. */
static int sleep_ms(long ms)
{
    struct timespec req;
    struct timespec rem;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * NS_PER_MS;
    while (nanosleep(&req, &rem) != 0) {
        if (errno != EINTR) {
            return -1;
        }
        req = rem;
    }
    return 0;
}

/* Send a one-byte command. Returns true if the whole byte was written. */
static bool send_cmd(int fd, uint8_t cmd)
{
    ssize_t n;
    do {
        n = write(fd, &cmd, 1);
    } while (n < 0 && errno == EINTR);
    return n == 1;
}

/*
 * Read `len` bytes in one I2C transfer. Returns `len` on success, the
 * short count for a short read, or -1 with errno set.
 */
static ssize_t read_reply(int fd, uint8_t *buf, size_t len)
{
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

/* A NACK of the address: the sensor is still converting. The Pi's
 * i2c-bcm2835 driver reports it as EREMOTEIO; other drivers use ENXIO. */
static bool is_nack(int err)
{
    return err == EREMOTEIO || err == ENXIO;
}

sample_status_t si7021_read(int fd, float *rh_pct, float *temp_c)
{
    if (rh_pct == NULL || temp_c == NULL) {
        return SAMPLE_ERR_READ;
    }

    if (!send_cmd(fd, CMD_MEASURE_RH_NO_HOLD)) {
        return SAMPLE_ERR_READ;
    }
    int64_t start_ms;
    if (mono_ms(&start_ms) != 0) {
        return SAMPLE_ERR_READ;
    }
    if (sleep_ms(SI7021_FIRST_WAIT_MS) != 0) {
        return SAMPLE_ERR_READ;
    }

    uint8_t rh_buf[3];
    for (;;) {
        ssize_t n = read_reply(fd, rh_buf, sizeof rh_buf);
        if (n == (ssize_t)sizeof rh_buf) {
            break;
        }
        if (n >= 0 || !is_nack(errno)) {
            return SAMPLE_ERR_READ;
        }
        int64_t now_ms;
        if (mono_ms(&now_ms) != 0) {
            return SAMPLE_ERR_READ;
        }
        if (now_ms - start_ms >= SI7021_TIMEOUT_MS) {
            return SAMPLE_ERR_TIMEOUT;
        }
        if (sleep_ms(SI7021_RETRY_MS) != 0) {
            return SAMPLE_ERR_READ;
        }
    }

    float rh = 0.0f;
    if (si7021_parse_rh(rh_buf, &rh) != SAMPLE_OK) {
        return SAMPLE_ERR_READ;
    }

    /* The temperature is already stored from the humidity measurement, so
     * there is nothing to wait for; a NACK here is an error. */
    if (!send_cmd(fd, CMD_READ_TEMP_FROM_RH)) {
        return SAMPLE_ERR_READ;
    }
    uint8_t temp_buf[2];
    if (read_reply(fd, temp_buf, sizeof temp_buf) != (ssize_t)sizeof temp_buf) {
        return SAMPLE_ERR_READ;
    }
    float t = 0.0f;
    if (si7021_parse_temp(temp_buf, &t) != SAMPLE_OK) {
        return SAMPLE_ERR_READ;
    }

    *rh_pct = rh;
    *temp_c = t;
    return SAMPLE_OK;
}
