#include "../src/sensors/ds18b20.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Tests for the DS18B20 w1_slave parser. CHECK() reports a failure and
 * keeps going, so one run shows every problem. Exit status is 0 only if
 * every check passed. Doubles are compared with NEAR() and a tolerance.
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
    RUN(test_parse_ok);
    RUN(test_parse_limits);
    RUN(test_parse_crc_bad);
    RUN(test_parse_power_on);
    RUN(test_parse_out_of_range);
    RUN(test_parse_malformed_t);
    RUN(test_parse_short_input);
    RUN(test_parse_all_zero_scratchpad);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures == 0) {
        return 0;
    }
    return 1;
}
