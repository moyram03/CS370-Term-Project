/* Needed for sigaction, pselect, and clock_gettime under -std=c17. */
#define _XOPEN_SOURCE 700

#include "si7021.h"
#include "si7021_i2c.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

/*
 * Si7021 sensor process (Mechanism E). Every interval it reads the sensor
 * and prints two lines to stdout: the humidity sample, then the air
 * temperature sample. Failed reads are recorded too, with an error status.
 * The connection to the hub comes later; for now the lines are the output.
 *
 * Exit status: 0 on a clean stop (SIGINT or SIGTERM); 1 on a fatal system
 * error; 2 on bad command-line arguments. With --once it is 0 if both
 * samples were OK and 1 if either was an error.
 */

#define DEFAULT_BUS         "/dev/i2c-1"
#define DEFAULT_INTERVAL_S  30.0
#define MAX_INTERVAL_S      86400.0
#define NS_PER_S            1000000000LL

typedef struct {
    const char *bus;
    double      interval_s;
    bool        once;
} options_t;

typedef struct {
    int      fd;       /* open bus descriptor, or -1 to force a reopen */
    bool     failing;  /* last reading was an error (for log lines) */
    uint32_t seq;      /* next sequence number, shared by both sensors */
} reader_t;

/* Set by the signal handler; checked by the main loop. */
static volatile sig_atomic_t stop_requested;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

/* Print `what` and the current errno to stderr, then exit with status 1. */
static void die(const char *what)
{
    int saved = errno;
    fprintf(stderr, "si7021_reader: %s: %s\n", what, strerror(saved));
    exit(1);
}

static void usage(FILE *to)
{
    fprintf(to,
            "usage: si7021_reader [--bus DEVICE] [--interval SECONDS] [--once]\n"
            "  --bus DEVICE        I2C bus device (default %s)\n"
            "  --interval SECONDS  time between readings, > 0 (default %.0f)\n"
            "  --once              take one reading and exit\n",
            DEFAULT_BUS, DEFAULT_INTERVAL_S);
}

/* Returns 0 to run, 1 if --help was shown, -1 on a bad argument. */
static int parse_args(int argc, char **argv, options_t *opts)
{
    opts->bus = DEFAULT_BUS;
    opts->interval_s = DEFAULT_INTERVAL_S;
    opts->once = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            opts->once = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 1;
        } else if (strcmp(argv[i], "--bus") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "si7021_reader: --bus needs a value\n");
                return -1;
            }
            i++;
            opts->bus = argv[i];
        } else if (strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "si7021_reader: --interval needs a value\n");
                return -1;
            }
            i++;
            char *end = NULL;
            errno = 0;
            double v = strtod(argv[i], &end);
            if (errno != 0 || end == argv[i] || *end != '\0' ||
                !(v > 0.0) || v > MAX_INTERVAL_S) {
                fprintf(stderr,
                        "si7021_reader: --interval must be a number above 0 "
                        "and at most %.0f: %s\n", MAX_INTERVAL_S, argv[i]);
                return -1;
            }
            opts->interval_s = v;
        } else {
            fprintf(stderr, "si7021_reader: unknown argument: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

/* Monotonic clock in nanoseconds, used for scheduling. */
static int mono_ns(int64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    *out = (int64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
    return 0;
}

/* Wall-clock time in milliseconds since the Unix epoch, for the sample. */
static int realtime_ms(int64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    *out = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    return 0;
}

/*
 * Sleep until the monotonic time `deadline_ns`, or until a stop signal
 * arrives. SIGINT and SIGTERM are blocked the rest of the time, and
 * pselect() unblocks them atomically for the sleep, so a signal can never
 * slip in between checking the flag and going to sleep. Returns 0, or -1
 * on a system error.
 */
static int sleep_until(int64_t deadline_ns, const sigset_t *unblocked)
{
    while (!stop_requested) {
        int64_t now;
        if (mono_ns(&now) != 0) {
            return -1;
        }
        if (now >= deadline_ns) {
            return 0;
        }
        int64_t left = deadline_ns - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(left / NS_PER_S);
        ts.tv_nsec = (long)(left % NS_PER_S);
        if (pselect(0, NULL, NULL, NULL, &ts, unblocked) < 0 && errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static const char *status_name(uint8_t status)
{
    switch (status) {
    case SAMPLE_OK:
        return "OK";
    case SAMPLE_ERR_READ:
        return "ERR_READ";
    case SAMPLE_ERR_TIMEOUT:
        return "ERR_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

static const char *sensor_name(uint8_t sensor)
{
    switch (sensor) {
    case SENSOR_SI7021_HUMIDITY:
        return "si7021_rh";
    case SENSOR_SI7021_TEMP:
        return "si7021_temp";
    default:
        return "unknown";
    }
}

/* Print one sample as a line on stdout. Returns 0, or -1 if stdout failed. */
static int print_sample(const sample_t *s)
{
    int rc;
    if (s->status == SAMPLE_OK) {
        rc = printf("ts=%lld seq=%u sensor=%s status=%s value=%.3f\n",
                    (long long)s->timestamp_ms, (unsigned)s->seq,
                    sensor_name(s->sensor), status_name(s->status),
                    (double)s->value);
    } else {
        rc = printf("ts=%lld seq=%u sensor=%s status=%s value=-\n",
                    (long long)s->timestamp_ms, (unsigned)s->seq,
                    sensor_name(s->sensor), status_name(s->status));
    }
    if (rc < 0) {
        return -1;
    }
    if (fflush(stdout) != 0) {
        return -1;
    }
    return 0;
}

/* Fill `s` with one sample and advance the shared sequence counter. */
static void make_sample(reader_t *r, sensor_id_t sensor, sample_status_t status,
                        float value, int64_t now_ms, sample_t *s)
{
    memset(s, 0, sizeof *s);
    s->timestamp_ms = now_ms;
    s->seq = r->seq;
    s->value = value;
    s->sensor = (uint8_t)sensor;
    s->status = (uint8_t)status;
    r->seq++;  /* counts every sample, including failed ones */
}

/*
 * Take one reading and fill `rh` and `temp`. The bus is opened if needed,
 * and closed after any failure so the next reading opens it again. Both
 * samples get the same status: the temperature comes from the humidity
 * measurement, so when that fails there is no temperature to report.
 * Errors and recoveries are logged to stderr once each, not on every
 * reading. Returns 0, or -1 if the clock failed.
 */
static int take_reading(const options_t *opts, reader_t *r,
                        sample_t *rh, sample_t *temp)
{
    float rh_pct = 0.0f;
    float temp_c = 0.0f;
    sample_status_t status = SAMPLE_ERR_READ;
    const char *why = NULL;

    if (r->fd < 0) {
        r->fd = si7021_open(opts->bus);
        if (r->fd < 0) {
            why = "open failed";
        }
    }
    if (r->fd >= 0) {
        status = si7021_read(r->fd, &rh_pct, &temp_c);
        if (status == SAMPLE_ERR_TIMEOUT) {
            why = "timeout waiting for the humidity conversion";
        } else if (status != SAMPLE_OK) {
            why = "read, CRC, or range error";
        }
    }

    if (status == SAMPLE_OK) {
        if (r->failing) {
            fprintf(stderr, "si7021_reader: sensor recovered\n");
        }
        r->failing = false;
    } else {
        if (!r->failing) {
            if (r->fd < 0) {
                fprintf(stderr, "si7021_reader: sensor error: %s (%s): %s\n",
                        why, opts->bus, strerror(errno));
            } else {
                fprintf(stderr, "si7021_reader: sensor error: %s\n", why);
            }
        }
        r->failing = true;
        si7021_close(r->fd);
        r->fd = -1;
        rh_pct = 0.0f;
        temp_c = 0.0f;
    }

    int64_t now_ms;
    if (realtime_ms(&now_ms) != 0) {
        return -1;
    }
    make_sample(r, SENSOR_SI7021_HUMIDITY, status, rh_pct, now_ms, rh);
    make_sample(r, SENSOR_SI7021_TEMP, status, temp_c, now_ms, temp);
    return 0;
}

int main(int argc, char **argv)
{
    options_t opts;
    int parsed = parse_args(argc, argv, &opts);
    if (parsed > 0) {
        return 0;
    }
    if (parsed < 0) {
        usage(stderr);
        return 2;
    }

    /* Stop on SIGINT/SIGTERM; ignore SIGPIPE so a closed stdout shows up
     * as a write error instead of killing the process silently. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        die("sigaction(SIGINT)");
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        die("sigaction(SIGTERM)");
    }
    struct sigaction ignore;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, NULL) != 0) {
        die("sigaction(SIGPIPE)");
    }

    /* Keep the stop signals blocked except while sleeping (see sleep_until). */
    sigset_t block;
    sigset_t unblocked;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &block, &unblocked) != 0) {
        die("sigprocmask");
    }

    int64_t interval_ns = (int64_t)(opts.interval_s * (double)NS_PER_S);
    if (interval_ns < 1) {
        interval_ns = 1;
    }

    reader_t reader;
    memset(&reader, 0, sizeof reader);
    reader.fd = -1;

    int64_t deadline_ns;
    if (mono_ns(&deadline_ns) != 0) {
        die("clock_gettime(CLOCK_MONOTONIC)");
    }

    int exit_status = 0;
    while (!stop_requested) {
        sample_t rh;
        sample_t temp;
        if (take_reading(&opts, &reader, &rh, &temp) != 0) {
            die("clock_gettime(CLOCK_REALTIME)");
        }
        if (print_sample(&rh) != 0 || print_sample(&temp) != 0) {
            die("writing to stdout");
        }

        if (opts.once) {
            if (rh.status != SAMPLE_OK || temp.status != SAMPLE_OK) {
                exit_status = 1;
            }
            break;
        }

        /* Keep a steady cadence; if a reading ran long, start the next one
         * right away instead of firing a burst to catch up. */
        deadline_ns += interval_ns;
        int64_t now;
        if (mono_ns(&now) != 0) {
            die("clock_gettime(CLOCK_MONOTONIC)");
        }
        if (deadline_ns < now) {
            deadline_ns = now;
        }
        if (sleep_until(deadline_ns, &unblocked) != 0) {
            die("pselect");
        }
    }

    si7021_close(reader.fd);
    if (stop_requested) {
        fprintf(stderr, "si7021_reader: stopping\n");
    }
    return exit_status;
}
