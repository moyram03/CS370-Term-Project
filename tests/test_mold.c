#include "../src/analysis/mold.h"

#include <math.h>
#include <stdio.h>

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

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures == 0) {
        return 0;
    }
    return 1;
}
