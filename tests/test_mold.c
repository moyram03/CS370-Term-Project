#include "../src/analysis/mold.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Tests for the mold analysis math. CHECK() reports a failure and keeps
 * going, so one run shows every problem. Exit status is 0 only if every
 * check passed. Doubles are compared with NEAR() and a tolerance.
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

static void test_sat_vapor(void)
{
    NEAR(mold_sat_vapor_hpa(0.0), 6.112, 1e-9);
    NEAR(mold_sat_vapor_hpa(20.0), 23.33, 0.01);
    NEAR(mold_sat_vapor_hpa(-10.0), 2.870, 0.001);
}

static void test_dew_point(void)
{
    NEAR(mold_dew_point_c(24.0, 60.0), 15.75, 0.01);
    NEAR(mold_dew_point_c(20.0, 50.0), 9.26, 0.01);
    NEAR(mold_dew_point_c(20.0, 100.0), 20.0, 1e-9);
}

static void test_surface_rh(void)
{
    NEAR(mold_surface_rh(24.0, 60.0, 18.0), 86.7, 0.05);
    NEAR(mold_surface_rh(24.0, 60.0, 24.0), 60.0, 1e-9);
    /* Unclamped this would be about 145.7. */
    NEAR(mold_surface_rh(24.0, 60.0, 10.0), 100.0, 1e-9);
}

static void test_rh_clamping(void)
{
    double low = mold_dew_point_c(20.0, 0.0);
    double high = mold_dew_point_c(20.0, 105.0);
    CHECK(isfinite(low));
    CHECK(low < -50.0);  /* very dry air has a very low dew point */
    CHECK(isfinite(high));
    NEAR(high, 20.0, 1e-9);  /* over-range is treated as 100 %RH */

    double s_low = mold_surface_rh(24.0, 0.0, 18.0);
    double s_high = mold_surface_rh(24.0, 105.0, 24.0);
    CHECK(isfinite(s_low));
    NEAR(s_low, 0.0, 1e-9);
    CHECK(isfinite(s_high));
    NEAR(s_high, 100.0, 1e-9);
}

/* ---- mold_init / mold_update ---- */

#define STEP_S        30.0   /* time between updates in these tests */
#define STEPS_PER_HR  120    /* 3600 s / STEP_S */

static mold_input_t make_input(double air_t, double air_rh, double surface_t)
{
    mold_input_t in;
    memset(&in, 0, sizeof in);
    in.air_temp_c = air_t;
    in.air_rh_pct = air_rh;
    in.surface_temp_c = surface_t;
    in.have_air = true;
    in.have_surface = true;
    return in;
}

/* Run `n` updates of STEP_S each; returns the last return code. */
static int step(mold_state_t *s, const mold_input_t *in, int n,
                mold_result_t *out)
{
    int rc = 0;
    for (int i = 0; i < n; i++) {
        rc = mold_update(s, in, STEP_S, out);
    }
    return rc;
}

static void test_init(void)
{
    mold_state_t s;
    s.wet_hours = 99.0;
    s.level = MOLD_RISK;
    mold_init(&s);
    NEAR(s.wet_hours, 0.0, 1e-12);
    CHECK(s.level == MOLD_OK);
}

static void test_no_air(void)
{
    mold_state_t s;
    mold_init(&s);
    s.wet_hours = 1.5;
    s.level = MOLD_WATCH;
    mold_state_t s_before = s;

    mold_result_t out;
    memset(&out, 0xAB, sizeof out);
    mold_result_t out_before = out;

    mold_input_t in = make_input(24.0, 60.0, 18.0);
    in.have_air = false;
    CHECK(mold_update(&s, &in, STEP_S, &out) == -1);
    CHECK(memcmp(&s, &s_before, sizeof s) == 0);
    CHECK(memcmp(&out, &out_before, sizeof out) == 0);
}

static void test_accumulate_1x(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(24.0, 60.0, 18.0);  /* surface RH about 86.7 */
    CHECK(step(&s, &in, STEPS_PER_HR, &out) == 0);
    NEAR(out.surface_rh_pct, 86.7, 0.05);
    CHECK(out.favorable);
    CHECK(!out.condensation);
    CHECK(!out.degraded);
    NEAR(out.dew_point_c, 15.75, 0.01);
    NEAR(s.wet_hours, 1.0, 1e-6);
    NEAR(out.wet_hours, s.wet_hours, 1e-12);
    CHECK(out.level == MOLD_OK);
    CHECK(s.level == MOLD_OK);
}

static void test_accumulate_2x(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(24.0, 60.0, 16.0);  /* surface RH about 98.4 */
    CHECK(step(&s, &in, STEPS_PER_HR, &out) == 0);
    NEAR(out.surface_rh_pct, 98.4, 0.05);
    CHECK(out.favorable);
    CHECK(!out.condensation);  /* 16 C is still above the 15.75 C dew point */
    NEAR(s.wet_hours, MOLD_WET_RATE_MULTIPLIER, 1e-6);
}

static void test_condensation(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(24.0, 60.0, 15.0);  /* below the dew point */
    CHECK(step(&s, &in, STEPS_PER_HR, &out) == 0);
    CHECK(out.condensation);
    NEAR(out.surface_rh_pct, 100.0, 1e-9);  /* clamped */
    CHECK(out.favorable);
    NEAR(s.wet_hours, MOLD_WET_RATE_MULTIPLIER, 1e-6);
}

static void test_dry_decay(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t wet = make_input(24.0, 60.0, 18.0);
    mold_input_t dry = make_input(24.0, 60.0, 24.0);  /* surface RH 60 */

    CHECK(step(&s, &wet, 2 * STEPS_PER_HR, &out) == 0);
    NEAR(s.wet_hours, 2.0, 1e-6);

    CHECK(step(&s, &dry, STEPS_PER_HR, &out) == 0);
    CHECK(!out.favorable);
    NEAR(s.wet_hours, 2.0 - MOLD_DRY_DECAY_PER_HOUR, 1e-6);  /* 1.5 now */

    /* Drying never goes below zero. */
    mold_init(&s);
    CHECK(step(&s, &dry, STEPS_PER_HR, &out) == 0);
    NEAR(s.wet_hours, 0.0, 1e-12);
}

static void test_too_cold(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(6.0, 85.0, 3.0);  /* surface RH clamps to 100 */
    CHECK(step(&s, &in, STEPS_PER_HR, &out) == 0);
    CHECK(out.surface_rh_pct >= MOLD_SURFACE_RH_WET_PCT);
    CHECK(!out.favorable);  /* below MOLD_TEMP_MIN_C */
    NEAR(s.wet_hours, 0.0, 1e-12);
}

static void test_dt_clamp(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(24.0, 60.0, 18.0);  /* 1x */
    CHECK(mold_update(&s, &in, 3600.0, &out) == 0);
    NEAR(s.wet_hours, MOLD_MAX_DT_S / 3600.0, 1e-9);
}

static void test_dt_nonpositive(void)
{
    mold_state_t s;
    mold_init(&s);
    s.wet_hours = 1.0;
    mold_result_t out;
    mold_input_t wet = make_input(24.0, 60.0, 18.0);
    mold_input_t dry = make_input(24.0, 60.0, 24.0);

    CHECK(mold_update(&s, &wet, 0.0, &out) == 0);
    NEAR(s.wet_hours, 1.0, 1e-12);
    CHECK(mold_update(&s, &wet, -30.0, &out) == 0);
    NEAR(s.wet_hours, 1.0, 1e-12);
    CHECK(mold_update(&s, &dry, 0.0, &out) == 0);
    NEAR(s.wet_hours, 1.0, 1e-12);
    CHECK(mold_update(&s, &dry, -30.0, &out) == 0);
    NEAR(s.wet_hours, 1.0, 1e-12);
    NEAR(out.wet_hours, 1.0, 1e-12);  /* out is still filled in */
}

static void test_degraded(void)
{
    mold_state_t s;
    mold_init(&s);
    mold_result_t out;
    mold_input_t in = make_input(24.0, 85.0, 999.0);  /* surface value ignored */
    in.have_surface = false;
    CHECK(step(&s, &in, STEPS_PER_HR, &out) == 0);
    CHECK(out.degraded);
    NEAR(out.surface_rh_pct, 85.0, 1e-9);
    CHECK(out.favorable);
    CHECK(!out.condensation);
    NEAR(s.wet_hours, 1.0, 1e-6);  /* 85 is below the 90 threshold: 1x */

    /* Air at 100 %RH puts the stand-in surface at the dew point, but
     * condensation is still not reported without a surface reading. */
    in = make_input(24.0, 100.0, 0.0);
    in.have_surface = false;
    CHECK(mold_update(&s, &in, STEP_S, &out) == 0);
    CHECK(out.degraded);
    CHECK(!out.condensation);
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
    RUN(test_sat_vapor);
    RUN(test_dew_point);
    RUN(test_surface_rh);
    RUN(test_rh_clamping);
    RUN(test_init);
    RUN(test_no_air);
    RUN(test_accumulate_1x);
    RUN(test_accumulate_2x);
    RUN(test_condensation);
    RUN(test_dry_decay);
    RUN(test_too_cold);
    RUN(test_dt_clamp);
    RUN(test_dt_nonpositive);
    RUN(test_degraded);

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures == 0) {
        return 0;
    }
    return 1;
}
