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

/*
 * Level after `wet_hours`, given the current level. A level is entered at
 * its ENTER threshold and left only below its EXIT threshold, so the level
 * does not flap around one value. RISK can fall straight to OK if wet hours
 * are already below the WATCH exit threshold.
 */
static mold_level_t next_level(mold_level_t current, double wet_hours)
{
    mold_level_t level = current;
    if (wet_hours >= MOLD_RISK_ENTER_HOURS) {
        level = MOLD_RISK;
    } else if (level == MOLD_RISK) {
        if (wet_hours < MOLD_RISK_EXIT_HOURS) {
            level = MOLD_WATCH;
        }
    } else if (level == MOLD_OK && wet_hours >= MOLD_WATCH_ENTER_HOURS) {
        level = MOLD_WATCH;
    }
    if (level == MOLD_WATCH && wet_hours < MOLD_WATCH_EXIT_HOURS) {
        level = MOLD_OK;
    }
    return level;
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

void mold_init(mold_state_t *s)
{
    s->wet_hours = 0.0;
    s->level = MOLD_OK;
}

int mold_update(mold_state_t *s, const mold_input_t *in, double dt_s,
                mold_result_t *out)
{
    if (!in->have_air) {
        return -1;
    }

    /* Without a surface reading, the air temperature stands in for it. */
    bool degraded = !in->have_surface;
    double surface_t_c;
    if (degraded) {
        surface_t_c = in->air_temp_c;
    } else {
        surface_t_c = in->surface_temp_c;
    }

    double dew_point_c = mold_dew_point_c(in->air_temp_c, in->air_rh_pct);
    double surface_rh = mold_surface_rh(in->air_temp_c, in->air_rh_pct,
                                        surface_t_c);

    /* Air at 100 %RH would put a stand-in surface exactly at the dew point,
     * which says nothing about a real surface, so never report it then. */
    bool condensation = !degraded && surface_t_c <= dew_point_c;
    bool favorable = surface_rh >= MOLD_SURFACE_RH_FAVORABLE_PCT &&
                     surface_t_c >= MOLD_TEMP_MIN_C &&
                     surface_t_c <= MOLD_TEMP_MAX_C;

    double dt_h = clamp(dt_s, 0.0, MOLD_MAX_DT_S) / 3600.0;
    double wet_hours = s->wet_hours;
    if (favorable) {
        if (surface_rh >= MOLD_SURFACE_RH_WET_PCT) {
            wet_hours += dt_h * MOLD_WET_RATE_MULTIPLIER;
        } else {
            wet_hours += dt_h;
        }
    } else {
        wet_hours -= MOLD_DRY_DECAY_PER_HOUR * dt_h;
        if (wet_hours < 0.0) {
            wet_hours = 0.0;
        }
    }
    s->wet_hours = wet_hours;
    s->level = next_level(s->level, wet_hours);

    out->dew_point_c = dew_point_c;
    out->surface_rh_pct = surface_rh;
    out->condensation = condensation;
    out->favorable = favorable;
    out->degraded = degraded;
    out->level = s->level;
    out->wet_hours = wet_hours;
    return 0;
}
