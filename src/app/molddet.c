/* Needed for sigaction, pselect, and clock_gettime under -std=c17. */
#define _XOPEN_SOURCE 700

#include "../analysis/mold.h"
#include "../common/sample.h"
#include "../sensors/ds18b20.h"
#include "../sensors/si7021_i2c.h"
#include "../store/applog.h"

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
 * Single-process mold detector. It stands in for the hub and the sensor
 * processes until the multi-process system exists: every interval it reads
 * the Si7021 and the DS18B20, appends three samples to the log, runs the
 * mold analysis, and prints one status line. The log is synced on its own
 * timer.
 *
 * Exit status: 0 on a clean stop (SIGINT or SIGTERM) or a finished --dump;
 * 1 on a fatal error (the log cannot be opened, a system call fails, or
 * --dump hits an I/O error); 2 on bad command-line arguments.
 */

#define DEFAULT_LOG         "./molddet.log"
#define DEFAULT_BUS         "/dev/i2c-1"
#define DEFAULT_W1_DIR      "/sys/bus/w1/devices"
#define DEFAULT_INTERVAL_S  30.0
#define DEFAULT_SYNC_S      300.0
#define MAX_SECONDS         86400.0
#define NS_PER_S            1000000000LL
#define PATH_BYTES          512

typedef struct {
    const char *log_path;
    const char *dump_path;  /* NULL unless --dump was given */
    double      interval_s;
    double      sync_s;
} options_t;

typedef struct {
    int      i2c_fd;                 /* open bus descriptor, or -1 to reopen */
    bool     si_failing;            /* last Si7021 reading was an error */
    char     ds_path[PATH_BYTES];   /* cached w1_slave path, "" to re-find */
    bool     ds_failing;            /* last DS18B20 reading was an error */
    bool     append_failing;        /* last append failed (for log lines) */
    bool     sync_failing;          /* last sync failed (for log lines) */
    uint32_t seq;                   /* next sequence number, shared by all three */
} app_t;

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
    fprintf(stderr, "molddet: %s: %s\n", what, strerror(saved));
    exit(1);
}

static void usage(FILE *to)
{
    fprintf(to,
            "usage: molddet [--log PATH] [--interval SECONDS] [--sync-every SECONDS]\n"
            "       molddet --dump PATH\n"
            "  --log PATH            log file (default %s)\n"
            "  --interval SECONDS    time between readings, > 0 (default %.0f)\n"
            "  --sync-every SECONDS  time between fsyncs, > 0 (default %.0f)\n"
            "  --dump PATH           print every record in the log, then exit\n",
            DEFAULT_LOG, DEFAULT_INTERVAL_S, DEFAULT_SYNC_S);
}

/* Parse a seconds value above 0 and at most MAX_SECONDS. Returns 0 or -1. */
static int parse_seconds(const char *flag, const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' ||
        !(v > 0.0) || v > MAX_SECONDS) {
        fprintf(stderr,
                "molddet: %s must be a number above 0 and at most %.0f: %s\n",
                flag, MAX_SECONDS, text);
        return -1;
    }
    *out = v;
    return 0;
}

/* Returns 0 to run, 1 if --help was shown, -1 on a bad argument. */
static int parse_args(int argc, char **argv, options_t *opts)
{
    opts->log_path = DEFAULT_LOG;
    opts->dump_path = NULL;
    opts->interval_s = DEFAULT_INTERVAL_S;
    opts->sync_s = DEFAULT_SYNC_S;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 1;
        }
        bool is_log = strcmp(argv[i], "--log") == 0;
        bool is_dump = strcmp(argv[i], "--dump") == 0;
        bool is_interval = strcmp(argv[i], "--interval") == 0;
        bool is_sync = strcmp(argv[i], "--sync-every") == 0;
        if (!is_log && !is_dump && !is_interval && !is_sync) {
            fprintf(stderr, "molddet: unknown argument: %s\n", argv[i]);
            return -1;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "molddet: %s needs a value\n", argv[i]);
            return -1;
        }
        const char *flag = argv[i];
        i++;
        if (is_log) {
            opts->log_path = argv[i];
        } else if (is_dump) {
            opts->dump_path = argv[i];
        } else if (is_interval) {
            if (parse_seconds(flag, argv[i], &opts->interval_s) != 0) {
                return -1;
            }
        } else {
            if (parse_seconds(flag, argv[i], &opts->sync_s) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

/* Monotonic clock in nanoseconds, used for scheduling and dt. */
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
    case SENSOR_DS18B20_TEMP:
        return "ds18b20_temp";
    case SENSOR_SI7021_TEMP:
        return "si7021_temp";
    default:
        return "unknown";
    }
}

static const char *level_name(mold_level_t level)
{
    switch (level) {
    case MOLD_OK:
        return "OK";
    case MOLD_WATCH:
        return "WATCH";
    case MOLD_RISK:
        return "RISK";
    default:
        return "UNKNOWN";
    }
}

/* ---- --dump ---- */

static int dump_visit(const sample_t *s, void *ctx)
{
    size_t *count = ctx;
    if (s->status == SAMPLE_OK) {
        printf("ts=%lld seq=%u sensor=%s status=%s value=%.3f\n",
               (long long)s->timestamp_ms, (unsigned)s->seq,
               sensor_name(s->sensor), status_name(s->status),
               (double)s->value);
    } else {
        printf("ts=%lld seq=%u sensor=%s status=%s value=-\n",
               (long long)s->timestamp_ms, (unsigned)s->seq,
               sensor_name(s->sensor), status_name(s->status));
    }
    (*count)++;
    return 0;
}

static int run_dump(const char *path)
{
    size_t count = 0;
    int rc = applog_read_all(path, dump_visit, &count);
    if (rc < 0) {
        fprintf(stderr, "molddet: cannot read %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("records: %zu\n", count);
    if (fflush(stdout) != 0) {
        return 1;
    }
    return 0;
}

/* ---- sensors ---- */

/* Fill `s` with one sample and advance the shared sequence counter. */
static void make_sample(app_t *app, sensor_id_t sensor, sample_status_t status,
                        float value, int64_t now_ms, sample_t *s)
{
    memset(s, 0, sizeof *s);
    s->timestamp_ms = now_ms;
    s->seq = app->seq;
    s->value = value;
    s->sensor = (uint8_t)sensor;
    s->status = (uint8_t)status;
    app->seq++;  /* counts every sample, including failed ones */
}

/*
 * Read the Si7021. The bus is opened if needed, and closed after any
 * failure so the next reading opens it again. Errors and recoveries are
 * logged once each. On failure the outputs are left at 0.
 */
static sample_status_t read_si7021(app_t *app, float *rh_pct, float *temp_c)
{
    sample_status_t status = SAMPLE_ERR_READ;
    const char *why = NULL;
    float rh = 0.0f;
    float t = 0.0f;

    if (app->i2c_fd < 0) {
        app->i2c_fd = si7021_open(DEFAULT_BUS);
        if (app->i2c_fd < 0) {
            why = "open failed";
        }
    }
    if (app->i2c_fd >= 0) {
        status = si7021_read(app->i2c_fd, &rh, &t);
        if (status == SAMPLE_ERR_TIMEOUT) {
            why = "timeout waiting for the humidity conversion";
        } else if (status != SAMPLE_OK) {
            why = "read, CRC, or range error";
        }
    }

    if (status == SAMPLE_OK) {
        if (app->si_failing) {
            fprintf(stderr, "molddet: si7021 recovered\n");
        }
        app->si_failing = false;
        *rh_pct = rh;
        *temp_c = t;
        return SAMPLE_OK;
    }

    if (!app->si_failing) {
        if (app->i2c_fd < 0) {
            fprintf(stderr, "molddet: si7021 error: %s (%s): %s\n",
                    why, DEFAULT_BUS, strerror(errno));
        } else {
            fprintf(stderr, "molddet: si7021 error: %s\n", why);
        }
    }
    app->si_failing = true;
    si7021_close(app->i2c_fd);
    app->i2c_fd = -1;
    *rh_pct = 0.0f;
    *temp_c = 0.0f;
    return status;
}

/*
 * Read the DS18B20. The device is found if needed, and the cached path is
 * dropped after any failure so the next reading finds it again (replugging
 * works). Errors and recoveries are logged once each.
 */
static sample_status_t read_ds18b20(app_t *app, float *temp_c)
{
    sample_status_t status = SAMPLE_ERR_READ;
    const char *why = NULL;
    float t = 0.0f;

    if (app->ds_path[0] == '\0') {
        if (ds18b20_find(DEFAULT_W1_DIR, app->ds_path, sizeof app->ds_path) != 0) {
            why = "no 28-* device found";
        }
    }
    if (app->ds_path[0] != '\0') {
        status = ds18b20_read(app->ds_path, &t);
        if (status != SAMPLE_OK) {
            why = "read, CRC, or range error";
        }
    }

    if (status == SAMPLE_OK) {
        if (app->ds_failing) {
            fprintf(stderr, "molddet: ds18b20 recovered\n");
        }
        app->ds_failing = false;
        *temp_c = t;
        return SAMPLE_OK;
    }

    if (!app->ds_failing) {
        fprintf(stderr, "molddet: ds18b20 error: %s (%s)\n", why, DEFAULT_W1_DIR);
    }
    app->ds_failing = true;
    app->ds_path[0] = '\0';
    *temp_c = 0.0f;
    return status;
}

/* ---- one cycle ---- */

/* Append one sample. A failure is logged (first one only) and not fatal. */
static void append_sample(app_t *app, applog_t *log, const sample_t *s)
{
    if (applog_append(log, s) != 0) {
        if (!app->append_failing) {
            fprintf(stderr, "molddet: log append failed: %s\n", strerror(errno));
        }
        app->append_failing = true;
    } else {
        if (app->append_failing) {
            fprintf(stderr, "molddet: log append recovered\n");
        }
        app->append_failing = false;
    }
}

static void print_status(bool have_surface, float surface_c,
                         const mold_input_t *in, const mold_result_t *r)
{
    printf("air=%.1fC rh=%.1f%% ", in->air_temp_c, in->air_rh_pct);
    if (have_surface) {
        printf("surface=%.1fC ", (double)surface_c);
    } else {
        printf("surface=- ");
    }
    printf("dew=%.1fC surface_rh=%.1f%% condensation=%d favorable=%d "
           "degraded=%d wet_hours=%.3f level=%s\n",
           r->dew_point_c, r->surface_rh_pct, (int)r->condensation,
           (int)r->favorable, (int)r->degraded, r->wet_hours,
           level_name(r->level));
}

/*
 * Take both readings, log the three samples, and run the analysis.
 * `last_update_ns` holds the monotonic time of the last successful
 * mold_update, valid only when `*have_last` is true.
 */
static void run_cycle(app_t *app, applog_t *log, mold_state_t *mold,
                      int64_t *last_update_ns, bool *have_last)
{
    float rh = 0.0f;
    float air = 0.0f;
    float surface = 0.0f;
    sample_status_t air_status = read_si7021(app, &rh, &air);
    sample_status_t ds_status = read_ds18b20(app, &surface);

    int64_t now_ms;
    int64_t now_ns;
    if (realtime_ms(&now_ms) != 0) {
        die("clock_gettime(CLOCK_REALTIME)");
    }
    if (mono_ns(&now_ns) != 0) {
        die("clock_gettime(CLOCK_MONOTONIC)");
    }

    sample_t samples[3];
    make_sample(app, SENSOR_SI7021_HUMIDITY, air_status, rh, now_ms, &samples[0]);
    make_sample(app, SENSOR_SI7021_TEMP, air_status, air, now_ms, &samples[1]);
    make_sample(app, SENSOR_DS18B20_TEMP, ds_status, surface, now_ms, &samples[2]);
    for (int i = 0; i < 3; i++) {
        append_sample(app, log, &samples[i]);
    }

    mold_input_t in;
    memset(&in, 0, sizeof in);
    in.have_air = air_status == SAMPLE_OK;
    in.have_surface = ds_status == SAMPLE_OK;
    in.air_temp_c = (double)air;
    in.air_rh_pct = (double)rh;
    in.surface_temp_c = (double)surface;

    double dt_s = 0.0;
    if (*have_last) {
        dt_s = (double)(now_ns - *last_update_ns) / (double)NS_PER_S;
    }

    mold_result_t result;
    if (mold_update(mold, &in, dt_s, &result) != 0) {
        printf("no air data\n");
    } else {
        *last_update_ns = now_ns;
        *have_last = true;
        print_status(in.have_surface, surface, &in, &result);
    }
    if (fflush(stdout) != 0) {
        die("writing to stdout");
    }
}

static int run_monitor(const options_t *opts)
{
    applog_recovery_t rec;
    applog_t *log = applog_open(opts->log_path, &rec);
    if (log == NULL) {
        fprintf(stderr, "molddet: cannot open log %s: %s\n",
                opts->log_path, strerror(errno));
        return 1;
    }
    printf("recovery: records_kept=%zu bytes_truncated=%zu\n",
           rec.records_ok, rec.bytes_truncated);
    if (fflush(stdout) != 0) {
        die("writing to stdout");
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

    int64_t interval_ns = (int64_t)(opts->interval_s * (double)NS_PER_S);
    if (interval_ns < 1) {
        interval_ns = 1;
    }
    int64_t sync_ns = (int64_t)(opts->sync_s * (double)NS_PER_S);
    if (sync_ns < 1) {
        sync_ns = 1;
    }

    app_t app;
    memset(&app, 0, sizeof app);
    app.i2c_fd = -1;

    mold_state_t mold;
    mold_init(&mold);
    int64_t last_update_ns = 0;
    bool have_last = false;

    int64_t start;
    if (mono_ns(&start) != 0) {
        die("clock_gettime(CLOCK_MONOTONIC)");
    }
    int64_t read_deadline = start;
    int64_t sync_deadline = start + sync_ns;

    while (!stop_requested) {
        int64_t now;
        if (mono_ns(&now) != 0) {
            die("clock_gettime(CLOCK_MONOTONIC)");
        }

        if (now >= read_deadline) {
            run_cycle(&app, log, &mold, &last_update_ns, &have_last);
            /* Keep a steady cadence; if a reading ran long, start the next
             * one right away instead of firing a burst to catch up. */
            read_deadline += interval_ns;
            if (mono_ns(&now) != 0) {
                die("clock_gettime(CLOCK_MONOTONIC)");
            }
            if (read_deadline < now) {
                read_deadline = now;
            }
        }

        if (now >= sync_deadline) {
            if (applog_sync(log) != 0) {
                if (!app.sync_failing) {
                    fprintf(stderr, "molddet: log sync failed: %s\n",
                            strerror(errno));
                }
                app.sync_failing = true;
            } else {
                if (app.sync_failing) {
                    fprintf(stderr, "molddet: log sync recovered\n");
                }
                app.sync_failing = false;
            }
            sync_deadline = now + sync_ns;
        }

        int64_t next = read_deadline;
        if (sync_deadline < next) {
            next = sync_deadline;
        }
        if (sleep_until(next, &unblocked) != 0) {
            die("pselect");
        }
    }

    si7021_close(app.i2c_fd);
    fprintf(stderr, "molddet: stopping\n");
    if (applog_close(log) != 0) {
        fprintf(stderr, "molddet: closing log failed: %s\n", strerror(errno));
    }
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

    if (opts.dump_path != NULL) {
        return run_dump(opts.dump_path);
    }
    return run_monitor(&opts);
}
