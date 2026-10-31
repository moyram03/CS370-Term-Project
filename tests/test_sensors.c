/* Needed for mkdtemp under -std=c17. */
#define _XOPEN_SOURCE 700

#include "../src/sensors/ds18b20.h"
#include "../src/sensors/si7021.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Tests for the DS18B20 w1_slave parser, device finder, and file reader,
 * and the Si7021 checksum and reply conversion. CHECK() reports a failure and
 * keeps going, so one run shows every problem. Exit status is 0 only if
 * every check passed. Doubles are compared with NEAR() and a tolerance.
 * The find/read tests build a fake 1-Wire folder inside a temporary
 * directory.
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

/* True if `a` is within `tol` of `b`. */
static int near(double a, double b, double tol)
{
    return fabs(a - b) <= tol;
}

#define NEAR(a, b, tol) CHECK(near((a), (b), (tol)))

/* Real-looking scratchpads (the CRC bytes are correct for each). */
#define PAD_23125 "72 01 4b 46 7f ff 0e 10 57"  /* 23.125 C */
#define PAD_M1250 "ec ff 4b 46 7f ff 0c 10 82"  /* -1.25 C */
#define PAD_ZERO  "00 00 4b 46 7f ff 0c 10 c8"  /* 0 C, nonzero config bytes */

#define SENTINEL (-999.0f)

/* Parse `text` expecting success; check the value. */
#define EXPECT_OK(text, expected)                                         \
    do {                                                                  \
        float t_ = SENTINEL;                                              \
        CHECK(ds18b20_parse((text), &t_) == SAMPLE_OK);                   \
        NEAR(t_, (expected), 1e-4);                                       \
    } while (0)

/* Parse `text` expecting SAMPLE_ERR_READ; check the output is untouched. */
#define EXPECT_ERR(text)                                                  \
    do {                                                                  \
        float t_ = SENTINEL;                                              \
        CHECK(ds18b20_parse((text), &t_) == SAMPLE_ERR_READ);             \
        CHECK(t_ == SENTINEL);                                            \
    } while (0)

/* Build a two-line text with the 23.125 C scratchpad on both lines.
 * `verdict` is the end of line 1 ("YES"), `tail` is the end of line 2. */
static const char *make_text(const char *verdict, const char *tail)
{
    static char buf[256];
    snprintf(buf, sizeof buf, PAD_23125 " : crc=57 %s\n" PAD_23125 " %s",
             verdict, tail);
    return buf;
}

static void test_parse_ok(void)
{
    EXPECT_OK(make_text("YES", "t=23125\n"), 23.125);
    EXPECT_OK(PAD_M1250 " : crc=82 YES\n" PAD_M1250 " t=-1250\n", -1.25);
    EXPECT_OK(PAD_ZERO " : crc=c8 YES\n" PAD_ZERO " t=0\n", 0.0);

    /* Line endings and trailing text that real files can have. */
    EXPECT_OK(make_text("YES", "t=23125"), 23.125);                 /* no final newline */
    EXPECT_OK(PAD_23125 " : crc=57 YES\r\n" PAD_23125 " t=23125\r\n", 23.125);
    EXPECT_OK(make_text("YES  ", "t=23125  \n"), 23.125);          /* trailing blanks */

    /* Line 2 without scratchpad bytes is still accepted. */
    EXPECT_OK("x : crc=00 YES\nt=23125\n", 23.125);
    /* Only one nonzero byte among zeros is a real scratchpad. */
    EXPECT_OK("00 00 00 00 00 00 00 00 01 : crc=00 YES\n"
              "00 00 00 00 00 00 00 00 01 t=0\n", 0.0);
}

static void test_parse_limits(void)
{
    EXPECT_OK(make_text("YES", "t=-55000\n"), -55.0);
    EXPECT_OK(make_text("YES", "t=125000\n"), 125.0);
    EXPECT_ERR(make_text("YES", "t=-55001\n"));
    EXPECT_ERR(make_text("YES", "t=125001\n"));
}

static void test_parse_crc_bad(void)
{
    EXPECT_ERR(make_text("NO", "t=23125\n"));
    EXPECT_ERR(make_text("yes", "t=23125\n"));     /* case matters */
    EXPECT_ERR(make_text("YESX", "t=23125\n"));    /* must end in YES */
    EXPECT_ERR(make_text("", "t=23125\n"));
}

static void test_parse_power_on(void)
{
    EXPECT_ERR(make_text("YES", "t=85000\n"));
    EXPECT_OK(make_text("YES", "t=84999\n"), 84.999);
    EXPECT_OK(make_text("YES", "t=85001\n"), 85.001);
}

static void test_parse_out_of_range(void)
{
    EXPECT_ERR(make_text("YES", "t=200000\n"));
    EXPECT_ERR(make_text("YES", "t=-60000\n"));
    EXPECT_ERR(make_text("YES", "t=99999999999999999999\n"));   /* overflows */
    EXPECT_ERR(make_text("YES", "t=-99999999999999999999\n"));
}

static void test_parse_malformed_t(void)
{
    EXPECT_ERR(PAD_23125 " : crc=57 YES\n" PAD_23125 "\n");       /* no t= */
    EXPECT_ERR(make_text("YES", "t=\n"));
    EXPECT_ERR(make_text("YES", "t=-\n"));
    EXPECT_ERR(make_text("YES", "t= 23125\n"));
    EXPECT_ERR(make_text("YES", "t=+23125\n"));
    EXPECT_ERR(make_text("YES", "t=abc\n"));
    EXPECT_ERR(make_text("YES", "t=23125abc\n"));                 /* garbage after */
    EXPECT_ERR(make_text("YES", "t=231.25\n"));
    EXPECT_ERR(make_text("YES", "t=23125\nextra line\n"));
    EXPECT_ERR(make_text("YES", "t=23125 t=23125\n"));            /* second t= */
}

static void test_parse_short_input(void)
{
    EXPECT_ERR(PAD_23125 " : crc=57 YES\n");                      /* one line */
    EXPECT_ERR(PAD_23125 " : crc=57 YES");                        /* no newline */
    EXPECT_ERR("");
    EXPECT_ERR("\n");
    EXPECT_ERR("\n\n");

    float t = SENTINEL;
    CHECK(ds18b20_parse(NULL, &t) == SAMPLE_ERR_READ);
    CHECK(t == SENTINEL);
    CHECK(ds18b20_parse(make_text("YES", "t=23125\n"), NULL) == SAMPLE_ERR_READ);
}

static void test_parse_all_zero_scratchpad(void)
{
    /* Data line stuck low: CRC 00, the kernel says YES, but it is not real. */
    EXPECT_ERR("00 00 00 00 00 00 00 00 00 : crc=00 YES\n"
               "00 00 00 00 00 00 00 00 00 t=0\n");
    /* Rejected no matter what t= says. */
    EXPECT_ERR("00 00 00 00 00 00 00 00 00 : crc=00 YES\n"
               "00 00 00 00 00 00 00 00 00 t=23125\n");
    /* A real 0 C reading is fine. */
    EXPECT_OK(PAD_ZERO " : crc=c8 YES\n" PAD_ZERO " t=0\n", 0.0);
}

/* ---- ds18b20_find / ds18b20_read against a fake 1-Wire folder ---- */

#define GOOD_W1 PAD_23125 " : crc=57 YES\n" PAD_23125 " t=23125\n"
#define COLD_W1 PAD_M1250 " : crc=82 YES\n" PAD_M1250 " t=-1250\n"
#define NO_CRC_W1 PAD_23125 " : crc=57 NO\n" PAD_23125 " t=23125\n"

/* The ghost entries a Pi's 1-Wire driver showed next to the real sensor. */
#define GHOST_A "00-400000000000"
#define GHOST_B "00-800000000000"

static char tmpdir[] = "/tmp/test_sensors.XXXXXX";
static char w1[256];  /* the fake 1-Wire folder for the current test */

static void make_dir(const char *path)
{
    CHECK(mkdir(path, 0755) == 0);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f != NULL) {
        CHECK(fputs(content, f) >= 0);
        CHECK(fclose(f) == 0);
    }
}

/* Start a fresh, empty fake 1-Wire folder named `name` and point `w1` at it. */
static void use_w1(const char *name)
{
    snprintf(w1, sizeof w1, "%s/%s", tmpdir, name);
    make_dir(w1);
}

/* Add the entry `entry` to the fake folder. If `content` is not NULL the
 * entry gets a w1_slave file containing it. */
static void add_entry(const char *entry, const char *content)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", w1, entry);
    make_dir(path);
    if (content != NULL) {
        snprintf(path, sizeof path, "%s/%s/w1_slave", w1, entry);
        write_file(path, content);
    }
}

/* Remove a directory tree (files, symlinks, and directories). */
static void remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir != NULL) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
                char child[512];
                snprintf(child, sizeof child, "%s/%s", path, e->d_name);
                struct stat st;
                if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                    remove_tree(child);
                } else {
                    unlink(child);
                }
            }
        }
        closedir(dir);
    }
    rmdir(path);
}

static void test_find_and_read(void)
{
    use_w1("ok");
    add_entry("28-0000075a1b2c", GOOD_W1);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == 0);
    char expected[512];
    snprintf(expected, sizeof expected, "%s/28-0000075a1b2c/w1_slave", w1);
    CHECK(strcmp(path, expected) == 0);

    float t = SENTINEL;
    CHECK(ds18b20_read(path, &t) == SAMPLE_OK);
    NEAR(t, 23.125, 1e-4);
}

static void test_find_ghosts_only(void)
{
    use_w1("ghosts");
    add_entry(GHOST_A, NULL);
    add_entry(GHOST_B, NULL);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == -1);
    CHECK(path[0] == '\0');
}

static void test_find_ignores_ghosts(void)
{
    use_w1("mixed");
    add_entry(GHOST_A, NULL);
    add_entry("28-0000075a1b2c", GOOD_W1);
    add_entry(GHOST_B, NULL);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == 0);
    CHECK(strstr(path, "/28-0000075a1b2c/w1_slave") != NULL);
}

static void test_find_picks_smallest(void)
{
    use_w1("two");
    add_entry("28-bbbbbbbbbbbb", GOOD_W1);
    add_entry("28-aaaaaaaaaaaa", COLD_W1);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == 0);
    CHECK(strstr(path, "/28-aaaaaaaaaaaa/w1_slave") != NULL);
    float t = SENTINEL;
    CHECK(ds18b20_read(path, &t) == SAMPLE_OK);
    NEAR(t, -1.25, 1e-4);
}

static void test_find_missing_dir(void)
{
    char missing[300];
    snprintf(missing, sizeof missing, "%s/no_such_folder", tmpdir);
    char path[512];
    CHECK(ds18b20_find(missing, path, sizeof path) == -1);
    CHECK(path[0] == '\0');

    /* A file is not a folder either. */
    char file[300];
    snprintf(file, sizeof file, "%s/just_a_file", tmpdir);
    write_file(file, "x\n");
    CHECK(ds18b20_find(file, path, sizeof path) == -1);
}

static void test_find_bad_args(void)
{
    use_w1("args");
    add_entry("28-0000075a1b2c", GOOD_W1);

    char expected[512];
    snprintf(expected, sizeof expected, "%s/28-0000075a1b2c/w1_slave", w1);
    size_t need = strlen(expected) + 1;  /* including the NUL */

    char path[512];
    CHECK(ds18b20_find(NULL, path, sizeof path) == -1);
    CHECK(ds18b20_find(w1, NULL, sizeof path) == -1);
    CHECK(ds18b20_find(w1, path, 0) == -1);
    CHECK(ds18b20_find(w1, path, 10) == -1);          /* far too small */
    CHECK(ds18b20_find(w1, path, need - 1) == -1);    /* one byte short */
    CHECK(path[0] == '\0');                           /* never a cut-off path */
    CHECK(ds18b20_find(w1, path, need) == 0);         /* exact fit */
    CHECK(strcmp(path, expected) == 0);
}

static void test_read_crc_no(void)
{
    use_w1("crcno");
    add_entry("28-0000075a1b2c", NO_CRC_W1);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == 0);
    float t = SENTINEL;
    CHECK(ds18b20_read(path, &t) == SAMPLE_ERR_READ);
    CHECK(t == SENTINEL);
}

static void test_read_deleted(void)
{
    use_w1("deleted");
    add_entry("28-0000075a1b2c", GOOD_W1);

    char path[512];
    CHECK(ds18b20_find(w1, path, sizeof path) == 0);
    CHECK(unlink(path) == 0);  /* the sensor vanishes after being found */
    float t = SENTINEL;
    CHECK(ds18b20_read(path, &t) == SAMPLE_ERR_READ);
    CHECK(t == SENTINEL);
}

static void test_read_unreadable(void)
{
    use_w1("unreadable");
    add_entry("28-0000075a1b2c", GOOD_W1);

    char dirpath[512];
    snprintf(dirpath, sizeof dirpath, "%s/28-0000075a1b2c", w1);
    float t = SENTINEL;
    CHECK(ds18b20_read(dirpath, &t) == SAMPLE_ERR_READ);       /* a directory */
    CHECK(t == SENTINEL);

    /* A file at or over the size limit is rejected, not cut off. */
    char big[DS18B20_READ_MAX_BYTES + 100];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    char bigpath[512];
    snprintf(bigpath, sizeof bigpath, "%s/big", w1);
    write_file(bigpath, big);
    CHECK(ds18b20_read(bigpath, &t) == SAMPLE_ERR_READ);
    CHECK(t == SENTINEL);

    char missing[512];
    snprintf(missing, sizeof missing, "%s/nothing_here", w1);
    CHECK(ds18b20_read(missing, &t) == SAMPLE_ERR_READ);
    CHECK(ds18b20_read(NULL, &t) == SAMPLE_ERR_READ);
    CHECK(ds18b20_read(bigpath, NULL) == SAMPLE_ERR_READ);
    CHECK(t == SENTINEL);
}

/* ---- Si7021 checksum and reply conversion ---- */

/* Fill `buf` with an RH reply for the 16-bit `code`, with a correct CRC. */
static void rh_reply(unsigned code, uint8_t buf[3])
{
    buf[0] = (uint8_t)(code >> 8);
    buf[1] = (uint8_t)(code & 0xFF);
    buf[2] = si7021_crc8(buf, 2);
}

/* Parse an RH reply for `code` expecting success; check the value. */
#define EXPECT_RH_OK(code, expected)                                      \
    do {                                                                  \
        uint8_t b_[3];                                                    \
        rh_reply((code), b_);                                             \
        float r_ = SENTINEL;                                              \
        CHECK(si7021_parse_rh(b_, &r_) == SAMPLE_OK);                     \
        NEAR(r_, (expected), 0.01);                                       \
    } while (0)

/* Parse a temperature reply (MSB, LSB) expecting success. */
#define EXPECT_TEMP_OK(msb, lsb, expected)                                \
    do {                                                                  \
        const uint8_t b_[2] = { (msb), (lsb) };                           \
        float t_ = SENTINEL;                                              \
        CHECK(si7021_parse_temp(b_, &t_) == SAMPLE_OK);                   \
        NEAR(t_, (expected), 0.01);                                       \
    } while (0)

/* Parse a temperature reply (MSB, LSB) expecting SAMPLE_ERR_READ. */
#define EXPECT_TEMP_ERR(msb, lsb)                                         \
    do {                                                                  \
        const uint8_t b_[2] = { (msb), (lsb) };                           \
        float t_ = SENTINEL;                                              \
        CHECK(si7021_parse_temp(b_, &t_) == SAMPLE_ERR_READ);             \
        CHECK(t_ == SENTINEL);                                            \
    } while (0)

static void test_si7021_crc8(void)
{
    /* Computed from the stated algorithm (polynomial 0x31, initial value
     * 0x00); to be confirmed against live sensor readings in the I2C
     * slice. */
    const uint8_t a[] = { 0x68, 0x3A };
    CHECK(si7021_crc8(a, sizeof a) == 0x7C);
    const uint8_t b[] = { 0x72, 0xB0 };
    CHECK(si7021_crc8(b, sizeof b) == 0x5D);
    const uint8_t zeros[] = { 0x00, 0x00 };
    CHECK(si7021_crc8(zeros, sizeof zeros) == 0x00);

    CHECK(si7021_crc8(a, 0) == 0x00);       /* empty input: the initial value */
    CHECK(si7021_crc8(NULL, 2) == 0x00);    /* NULL counts as empty */
}

static void test_si7021_rh_ok(void)
{
    EXPECT_RH_OK(0x72B0, 50.00);
    /* The two low bits are status bits and must not change the result. */
    EXPECT_RH_OK(0x72B1, 50.00);
    EXPECT_RH_OK(0x72B2, 50.00);
    EXPECT_RH_OK(0x72B3, 50.00);
}

static void test_si7021_rh_clamp(void)
{
    uint8_t buf[3];
    float rh = SENTINEL;

    rh_reply(0x0000, buf);  /* exactly -6 %RH */
    CHECK(si7021_parse_rh(buf, &rh) == SAMPLE_OK);
    NEAR(rh, 0.0, 1e-9);

    rh = SENTINEL;
    rh_reply(0xFFFC, buf);  /* about 119 %RH */
    CHECK(si7021_parse_rh(buf, &rh) == SAMPLE_OK);
    NEAR(rh, 100.0, 1e-9);
}

static void test_si7021_rh_crc(void)
{
    uint8_t good[3];
    rh_reply(0x72B0, good);
    float rh = SENTINEL;
    CHECK(si7021_parse_rh(good, &rh) == SAMPLE_OK);

    /* A CRC-8 catches every single-bit error: flip each of the 24 bits. */
    for (int bit = 0; bit < 24; bit++) {
        uint8_t bad[3];
        memcpy(bad, good, sizeof bad);
        bad[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        rh = SENTINEL;
        CHECK(si7021_parse_rh(bad, &rh) == SAMPLE_ERR_READ);
        CHECK(rh == SENTINEL);
    }
}

static void test_si7021_temp_ok(void)
{
    EXPECT_TEMP_OK(0x68, 0xAC, 25.00);
    /* Low status bits are ignored here too. */
    EXPECT_TEMP_OK(0x68, 0xAD, 25.00);
    EXPECT_TEMP_OK(0x68, 0xAE, 25.00);
    EXPECT_TEMP_OK(0x68, 0xAF, 25.00);
}

static void test_si7021_temp_range(void)
{
    EXPECT_TEMP_ERR(0x00, 0x00);  /* -46.85 C */
    EXPECT_TEMP_ERR(0xFF, 0xFC);  /* about 128.9 C */

    /* Just inside and just outside each limit. */
    EXPECT_TEMP_OK(0x09, 0xFC, -39.997);   /* -40 C limit */
    EXPECT_TEMP_ERR(0x09, 0xF8);           /* -40.007 C */
    EXPECT_TEMP_OK(0xFA, 0x5C, 124.998);   /* 125 C limit */
    EXPECT_TEMP_ERR(0xFA, 0x60);           /* 125.009 C */
}

static void test_si7021_null_args(void)
{
    uint8_t rh_buf[3];
    rh_reply(0x72B0, rh_buf);
    const uint8_t temp_buf[2] = { 0x68, 0xAC };
    float out = SENTINEL;

    CHECK(si7021_parse_rh(NULL, &out) == SAMPLE_ERR_READ);
    CHECK(si7021_parse_rh(rh_buf, NULL) == SAMPLE_ERR_READ);
    CHECK(si7021_parse_temp(NULL, &out) == SAMPLE_ERR_READ);
    CHECK(si7021_parse_temp(temp_buf, NULL) == SAMPLE_ERR_READ);
    CHECK(out == SENTINEL);
}

static void run(const char *name, void (*test)(void))
{
    int before = failures;
    test();
    if (failures == before) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s\n", name);
    }
}

#define RUN(test) run(#test, test)

int main(void)
{
    if (mkdtemp(tmpdir) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    RUN(test_parse_ok);
    RUN(test_parse_limits);
    RUN(test_parse_crc_bad);
    RUN(test_parse_power_on);
    RUN(test_parse_out_of_range);
    RUN(test_parse_malformed_t);
    RUN(test_parse_short_input);
    RUN(test_parse_all_zero_scratchpad);
    RUN(test_find_and_read);
    RUN(test_find_ghosts_only);
    RUN(test_find_ignores_ghosts);
    RUN(test_find_picks_smallest);
    RUN(test_find_missing_dir);
    RUN(test_find_bad_args);
    RUN(test_read_crc_no);
    RUN(test_read_deleted);
    RUN(test_read_unreadable);
    RUN(test_si7021_crc8);
    RUN(test_si7021_rh_ok);
    RUN(test_si7021_rh_clamp);
    RUN(test_si7021_rh_crc);
    RUN(test_si7021_temp_ok);
    RUN(test_si7021_temp_range);
    RUN(test_si7021_null_args);

    remove_tree(tmpdir);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures == 0) {
        return 0;
    }
    return 1;
}
