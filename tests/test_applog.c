/* Needed for mkdtemp, pwrite, ftruncate, and setrlimit under -std=c17. */
#define _XOPEN_SOURCE 700

#include "../src/store/applog.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Tests for the append-only log (Mechanism D). Each test uses its own log
 * file inside a fresh temporary directory. CHECK() reports a failure and
 * keeps going, so one run shows every problem. Exit status is 0 only if
 * every check passed.
 */

static int failures;
static int checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            fprintf(stderr, "  %s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                           \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static char tmpdir[] = "/tmp/test_applog.XXXXXX";
static char path[256];

/* Point `path` at a log file for one test, removing any leftover copy. */
static void use_log(const char *name)
{
    snprintf(path, sizeof path, "%s/%s.log", tmpdir, name);
    unlink(path);
}

static sample_t make_sample(uint32_t seq)
{
    sample_t s;
    memset(&s, 0, sizeof s);
    s.timestamp_ms = 1700000000000LL + (int64_t)seq * 30000;  /* 30 s apart */
    s.seq = seq;
    s.value = 40.0f + (float)seq;
    s.sensor = (seq % 2 == 0) ? SENSOR_SI7021_HUMIDITY : SENSOR_DS18B20_TEMP;
    s.status = SAMPLE_OK;
    return s;
}

static int same_sample(const sample_t *a, const sample_t *b)
{
    return a->timestamp_ms == b->timestamp_ms && a->seq == b->seq &&
           a->value == b->value && a->sensor == b->sensor &&
           a->status == b->status;
}

/* Create a fresh log holding records with seq 0..n-1. */
static void write_records(uint32_t n)
{
    applog_t *log = applog_open(path, NULL);
    CHECK(log != NULL);
    for (uint32_t i = 0; i < n; i++) {
        sample_t s = make_sample(i);
        CHECK(applog_append(log, &s) == 0);
    }
    CHECK(applog_close(log) == 0);
}

static off_t file_size(void)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

/* Overwrite `len` bytes at `off` in the log, to simulate damage. */
static void poke(off_t off, const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK(pwrite(fd, bytes, len, off) == (ssize_t)len);
    close(fd);
}

/* Add raw bytes to the end of the log, to simulate a cut-off write. */
static void append_raw(const void *bytes, size_t len)
{
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    CHECK(write(fd, bytes, len) == (ssize_t)len);
    close(fd);
}

/* Collects every record applog_read_all() hands back. */
#define MAX_COLLECT 64
typedef struct {
    sample_t samples[MAX_COLLECT];
    int count;
    int stop_after;  /* return 1 after this many records; 0 means never */
} collector_t;

static int collect(const sample_t *sample, void *ctx)
{
    collector_t *c = ctx;
    if (c->count < MAX_COLLECT)
        c->samples[c->count] = *sample;
    c->count++;
    return (c->stop_after != 0 && c->count >= c->stop_after) ? 1 : 0;
}

/* Read the whole log; returns applog_read_all()'s result. */
static int read_back(collector_t *c)
{
    memset(c, 0, sizeof *c);
    return applog_read_all(path, collect, c);
}

/* Check that the log holds exactly seq 0..n-1, in order. */
static void check_records(int n)
{
    collector_t c;
    CHECK(read_back(&c) == 0);
    CHECK(c.count == n);
    for (int i = 0; i < n && i < c.count; i++) {
        sample_t want = make_sample((uint32_t)i);
        CHECK(same_sample(&c.samples[i], &want));
    }
}

/* Independent CRC-32 so the format test does not just trust applog.c. */
static uint32_t ref_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* ---------------------------------------------------------------- tests */

static void test_open_new_log(void)
{
    use_log("new");
    applog_recovery_t rec = { 99, 99 };
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 0);
    CHECK(rec.bytes_truncated == 0);
    CHECK(file_size() == 0);
    CHECK(applog_close(log) == 0);
}

static void test_append_and_read_back(void)
{
    use_log("append");
    write_records(10);
    CHECK(file_size() == 10 * APPLOG_RECORD_SIZE);
    check_records(10);
}

static void test_record_layout(void)
{
    const uint8_t check_str[] = "123456789";
    CHECK(ref_crc32(check_str, 9) == 0xCBF43926u);  /* standard CRC-32 check value */

    use_log("layout");
    write_records(1);

    uint8_t rec[APPLOG_RECORD_SIZE];
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    CHECK(read(fd, rec, sizeof rec) == (ssize_t)sizeof rec);
    close(fd);

    uint32_t magic, crc;
    sample_t s;
    memcpy(&magic, rec + 0, 4);
    memcpy(&s, rec + 4, sizeof s);
    memcpy(&crc, rec + 28, 4);

    sample_t want = make_sample(0);
    CHECK(magic == APPLOG_MAGIC);
    CHECK(same_sample(&s, &want));
    CHECK(crc == ref_crc32(rec, 28));
}

static void test_reserved_zeroed(void)
{
    use_log("reserved");
    applog_t *log = applog_open(path, NULL);
    CHECK(log != NULL);
    sample_t s = make_sample(0);
    memset(s.reserved, 0xAB, sizeof s.reserved);
    CHECK(applog_append(log, &s) == 0);
    CHECK(applog_close(log) == 0);

    collector_t c;
    CHECK(read_back(&c) == 0);
    CHECK(c.count == 1);
    for (size_t i = 0; i < sizeof c.samples[0].reserved; i++)
        CHECK(c.samples[0].reserved[i] == 0);
}

static void test_reopen_clean_log(void)
{
    use_log("reopen");
    write_records(5);

    applog_recovery_t rec;
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 5);
    CHECK(rec.bytes_truncated == 0);
    CHECK(applog_close(log) == 0);
    CHECK(file_size() == 5 * APPLOG_RECORD_SIZE);
}

static void test_recover_cut_off_write(void)
{
    use_log("cutoff");
    write_records(4);
    append_raw("partial-write", 13);  /* less than one record */

    applog_recovery_t rec;
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 4);
    CHECK(rec.bytes_truncated == 13);
    CHECK(applog_close(log) == 0);
    CHECK(file_size() == 4 * APPLOG_RECORD_SIZE);
    check_records(4);
}

static void test_recover_bad_checksum(void)
{
    use_log("badcrc");
    write_records(4);
    poke(3 * APPLOG_RECORD_SIZE + 12, "X", 1);  /* change a byte inside the last sample */

    applog_recovery_t rec;
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 3);
    CHECK(rec.bytes_truncated == APPLOG_RECORD_SIZE);
    CHECK(applog_close(log) == 0);
    check_records(3);
}

static void test_recover_bad_magic(void)
{
    use_log("badmagic");
    write_records(4);
    poke(3 * APPLOG_RECORD_SIZE, "ZZZZ", 4);

    applog_recovery_t rec;
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 3);
    CHECK(rec.bytes_truncated == APPLOG_RECORD_SIZE);
    CHECK(applog_close(log) == 0);
    check_records(3);
}

/* Chosen behavior: recovery stops at the first bad record, so damage in
 * the middle also drops the good records after it. */
static void test_recover_damage_in_middle(void)
{
    use_log("middle");
    write_records(6);
    poke(2 * APPLOG_RECORD_SIZE + 12, "X", 1);  /* damage record 2 of 0..5 */

    applog_recovery_t rec;
    applog_t *log = applog_open(path, &rec);
    CHECK(log != NULL);
    CHECK(rec.records_ok == 2);
    CHECK(rec.bytes_truncated == 4 * APPLOG_RECORD_SIZE);
    CHECK(applog_close(log) == 0);
    check_records(2);
}

static void test_append_after_recovery(void)
{
    use_log("after");
    write_records(3);
    append_raw("junk", 4);

    applog_t *log = applog_open(path, NULL);
    CHECK(log != NULL);
    for (uint32_t i = 3; i < 6; i++) {
        sample_t s = make_sample(i);
        CHECK(applog_append(log, &s) == 0);
    }
    CHECK(applog_close(log) == 0);

    CHECK(file_size() == 6 * APPLOG_RECORD_SIZE);
    check_records(6);
}

/* Simulate the disk filling up mid-write by capping the file size, so the
 * write stops partway through a record. */
static void test_partial_write_is_removed(void)
{
    use_log("partial");
    write_records(3);

    applog_t *log = applog_open(path, NULL);
    CHECK(log != NULL);

    struct rlimit old, cap;
    CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0);
    cap = old;
    cap.rlim_cur = 3 * APPLOG_RECORD_SIZE + 10;  /* room for 10 more bytes only */
    signal(SIGXFSZ, SIG_IGN);  /* otherwise going over the cap kills the process */
    CHECK(setrlimit(RLIMIT_FSIZE, &cap) == 0);

    sample_t s = make_sample(3);
    errno = 0;
    CHECK(applog_append(log, &s) == -1);
    CHECK(errno == EFBIG);
    CHECK(file_size() == 3 * APPLOG_RECORD_SIZE);  /* the 10 partial bytes are gone */

    CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0);
    signal(SIGXFSZ, SIG_DFL);

    /* With space available again, appends line up on record boundaries. */
    CHECK(applog_append(log, &s) == 0);
    CHECK(applog_close(log) == 0);
    check_records(4);
}

static void test_read_all_stops_early(void)
{
    use_log("stop");
    write_records(5);

    collector_t c;
    memset(&c, 0, sizeof c);
    c.stop_after = 2;
    CHECK(applog_read_all(path, collect, &c) == 1);
    CHECK(c.count == 2);
}

static void test_read_all_missing_file(void)
{
    use_log("missing");
    collector_t c;
    errno = 0;
    CHECK(read_back(&c) == -1);
    CHECK(errno == ENOENT);
    CHECK(c.count == 0);
}

static void test_read_all_leaves_damage_alone(void)
{
    use_log("readonly");
    write_records(3);
    append_raw("junk", 4);
    off_t before = file_size();

    check_records(3);
    CHECK(file_size() == before);  /* only applog_open() repairs the file */
}

static void test_small_cases(void)
{
    CHECK(applog_close(NULL) == 0);

    use_log("sync");
    applog_t *log = applog_open(path, NULL);
    CHECK(log != NULL);
    sample_t s = make_sample(0);
    CHECK(applog_append(log, &s) == 0);
    CHECK(applog_sync(log) == 0);
    CHECK(applog_close(log) == 0);

    char bad[300];
    snprintf(bad, sizeof bad, "%s/no-such-dir/x.log", tmpdir);
    errno = 0;
    CHECK(applog_open(bad, NULL) == NULL);
    CHECK(errno == ENOENT);
}

/* ----------------------------------------------------------------- main */

#define RUN(test)                                    \
    do {                                             \
        int before = failures;                       \
        test();                                      \
        printf("%s %s\n", failures == before ? "PASS" : "FAIL", #test); \
    } while (0)

int main(void)
{
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    RUN(test_open_new_log);
    RUN(test_append_and_read_back);
    RUN(test_record_layout);
    RUN(test_reserved_zeroed);
    RUN(test_reopen_clean_log);
    RUN(test_recover_cut_off_write);
    RUN(test_recover_bad_checksum);
    RUN(test_recover_bad_magic);
    RUN(test_recover_damage_in_middle);
    RUN(test_append_after_recovery);
    RUN(test_partial_write_is_removed);
    RUN(test_read_all_stops_early);
    RUN(test_read_all_missing_file);
    RUN(test_read_all_leaves_damage_alone);
    RUN(test_small_cases);

    /* Clean up every log file the tests created, then the directory. */
    static const char *names[] = {
        "new", "append", "layout", "reserved", "reopen", "cutoff", "badcrc",
        "badmagic", "middle", "after", "partial", "stop", "readonly", "sync",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        use_log(names[i]);
    }
    rmdir(tmpdir);

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
