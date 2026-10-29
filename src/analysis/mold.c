#include "mold.h"

#include <math.h>

/* Magnus formula constants, shared by the saturation and dew point math. */
#define MAGNUS_A 17.62
#define MAGNUS_B 243.12  /* degrees C */

/* Limit `x` to the range lo..hi. */
static double clamp(double x, double lo, double hi)
{
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

double mold_sat_vapor_hpa(double t_c)
{
    return 6.112 * exp(MAGNUS_A * t_c / (MAGNUS_B + t_c));
}

double mold_dew_point_c(double t_c, double rh_pct)
{
    double rh = clamp(rh_pct, MOLD_RH_MIN_PCT, 100.0);
    double g = log(rh / 100.0) + MAGNUS_A * t_c / (MAGNUS_B + t_c);
    return MAGNUS_B * g / (MAGNUS_A - g);
}

double mold_surface_rh(double air_t_c, double air_rh_pct, double surface_t_c)
{
    double rh = clamp(air_rh_pct, 0.0, 100.0);
    double surface_rh = rh * mold_sat_vapor_hpa(air_t_c) /
                        mold_sat_vapor_hpa(surface_t_c);
    return clamp(surface_rh, 0.0, 100.0);
}
