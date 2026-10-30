/* Needed for sigaction, pselect, and clock_gettime under -std=c17. */
#define _XOPEN_SOURCE 700

#include "ds18b20.h"

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
 * DS18B20 sensor process (Mechanism E). Every interval it finds the
 * sensor, reads it, and prints one line per reading to stdout. Failed
 * reads are recorded too, with an error status. The connection to the hub
 * comes later; for now the line is the output.
 *
 * Exit status: 0 on a clean stop (SIGINT or SIGTERM); 1 on a fatal system
 * error; 2 on bad command-line arguments. With --once it is 0 if the
 * reading was OK and 1 if it was an error.
 */

#define DEFAULT_W1_DIR      "/sys/bus/w1/devices"
#define DEFAULT_INTERVAL_S  30.0
#define MAX_INTERVAL_S      86400.0
#define PATH_LEN            512
#define NS_PER_S            1000000000LL

typedef struct {
    const char *w1_dir;
    double      interval_s;
    bool        once;
} options_t;

typedef struct {
    char     path[PATH_LEN];  /* w1_slave path, valid only if have_path */
    bool     have_path;       /* false forces a fresh ds18b20_find() */
    bool     failing;         /* last reading was an error (for log lines) */
    uint32_t seq;             /* next sequence number */
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
    fprintf(stderr, "ds18b20_reader: %s: %s\n", what, strerror(saved));
    exit(1);
}

static void usage(FILE *to)
{
    fprintf(to,
            "usage: ds18b20_reader [--w1-dir DIR] [--interval SECONDS] [--once]\n"
            "  --w1-dir DIR        1-Wire device folder (default %s)\n"
            "  --interval SECONDS  time between readings, > 0 (default %.0f)\n"
            "  --once              take one reading and exit\n",
            DEFAULT_W1_DIR, DEFAULT_INTERVAL_S);
}

/* Returns 0 to run, 1 if --help was shown, -1 on a bad argument. */
static int parse_args(int argc, char **argv, options_t *opts)
{
    opts->w1_dir = DEFAULT_W1_DIR;
    opts->interval_s = DEFAULT_INTERVAL_S;
    opts->once = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0) {
            opts->once = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 1;
        } else if (strcmp(argv[i], "--w1-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ds18b20_reader: --w1-dir needs a value\n");
                return -1;
            }
            i++;
            opts->w1_dir = argv[i];
        } else if (strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ds18b20_reader: --interval needs a value\n");
                return -1;
            }
            i++;
            char *end = NULL;
            errno = 0;
            double v = strtod(argv[i], &end);
            if (errno != 0 || end == argv[i] || *end != '\0' ||
                !(v > 0.0) || v > MAX_INTERVAL_S) {
                fprintf(stderr,
                        "ds18b20_reader: --interval must be a number above 0 "
                        "and at most %.0f: %s\n", MAX_INTERVAL_S, argv[i]);
                return -1;
            }
            opts->interval_s = v;
        } else {
            fprintf(stderr, "ds18b20_reader: unknown argument: %s\n", argv[i]);
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

/* Print one sample as a line on stdout. Returns 0, or -1 if stdout failed. */
static int print_sample(const sample_t *s)
{
    int rc;
    if (s->status == SAMPLE_OK) {
        rc = printf("ts=%lld seq=%u sensor=ds18b20 status=%s value=%.3f\n",
                    (long long)s->timestamp_ms, (unsigned)s->seq,
                    status_name(s->status), (double)s->value);
    } else {
        rc = printf("ts=%lld seq=%u sensor=ds18b20 status=%s value=-\n",
                    (long long)s->timestamp_ms, (unsigned)s->seq,
                    status_name(s->status));
    }
    if (rc < 0) {
        return -1;
    }
    if (fflush(stdout) != 0) {
        return -1;
    }
    return 0;
}

/*
 * Take one reading and fill `s`. If there is no remembered device path it
 * is looked up first, and after any failure the path is forgotten so the
 * next reading looks again (the sensor may come back under another name
 * after being replugged). Errors and recoveries are logged to stderr once
 * each, not on every reading. Returns 0, or -1 if the clock failed.
 */
static int take_reading(const options_t *opts, reader_t *r, sample_t *s)
{
    float temp_c = 0.0f;
    sample_status_t status = SAMPLE_ERR_READ;
    const char *why = NULL;

    if (!r->have_path) {
        if (ds18b20_find(opts->w1_dir, r->path, sizeof r->path) == 0) {
            r->have_path = true;
        } else {
            why = "no DS18B20 (28-*) found in the 1-Wire folder";
        }
    }
    if (r->have_path) {
        status = ds18b20_read(r->path, &temp_c);
        if (status != SAMPLE_OK) {
            why = "reading or parsing w1_slave failed";
        }
    }

    if (status == SAMPLE_OK) {
        if (r->failing) {
            fprintf(stderr, "ds18b20_reader: sensor recovered\n");
        }
        r->failing = false;
    } else {
        if (!r->failing) {
            fprintf(stderr, "ds18b20_reader: sensor error: %s\n", why);
        }
        r->failing = true;
        r->have_path = false;
        temp_c = 0.0f;
    }

    int64_t now_ms;
    if (realtime_ms(&now_ms) != 0) {
        return -1;
    }
    memset(s, 0, sizeof *s);
    s->timestamp_ms = now_ms;
    s->seq = r->seq;
    s->value = temp_c;
    s->sensor = SENSOR_DS18B20_TEMP;
    s->status = (uint8_t)status;
    r->seq++;  /* counts every reading, including failed ones */
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

    int64_t deadline_ns;
    if (mono_ns(&deadline_ns) != 0) {
        die("clock_gettime(CLOCK_MONOTONIC)");
    }

    int exit_status = 0;
    while (!stop_requested) {
        sample_t sample;
        if (take_reading(&opts, &reader, &sample) != 0) {
            die("clock_gettime(CLOCK_REALTIME)");
        }
        if (print_sample(&sample) != 0) {
            die("writing to stdout");
        }

        if (opts.once) {
            if (sample.status != SAMPLE_OK) {
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

    if (stop_requested) {
        fprintf(stderr, "ds18b20_reader: stopping\n");
    }
    return exit_status;
}
