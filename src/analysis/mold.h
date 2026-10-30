#ifndef MOLD_H
#define MOLD_H

#include <stdbool.h>

/*
 * Mold-risk analysis.
 *
 * Pure computation: no I/O, no clocks, and no knowledge of sensor IDs or
 * sample_t. The hub reads the latest good value from each sensor, fills in
 * a mold_input_t with plain numbers, and calls mold_update() once per
 * reading cycle. The caller owns the mold_state_t and keeps it between
 * calls.
 *
 * The idea: mold grows on a surface, not in the air. The DS18B20 measures
 * the surface temperature. The Si7021 measures the air temperature and
 * relative humidity. The air's moisture content, applied to the cooler
 * surface, gives the relative humidity at the surface. The longer that
 * stays high, the higher the risk.
 *
 * Every threshold below is a starting value to be tuned once we have real
 * data from the 48-hour run.
 *
 * Inputs must be finite (no NaN or infinity). This module does not check:
 * the hub only passes readings whose status is SAMPLE_OK, so rejecting bad
 * data is the hub's job.
 */

/* ---- Constants (all starting values to tune) ---- */

/* Surface RH at or above this (within the temperature range) is favorable
 * for mold growth. Starting value to tune. */
#define MOLD_SURFACE_RH_FAVORABLE_PCT  80.0

/* Surface RH at or above this (while favorable) is "very wet": wet hours
 * accumulate faster. Starting value to tune. */
#define MOLD_SURFACE_RH_WET_PCT        90.0

/* Wet-hour accumulation multiplier used while surface RH is at or above
 * MOLD_SURFACE_RH_WET_PCT. Starting value to tune. */
#define MOLD_WET_RATE_MULTIPLIER       2.0

/* Surface temperatures from MOLD_TEMP_MIN_C to MOLD_TEMP_MAX_C (inclusive)
 * allow mold growth. Outside this range conditions are never favorable.
 * Starting values to tune. */
#define MOLD_TEMP_MIN_C                5.0
#define MOLD_TEMP_MAX_C                40.0

/* Wet hours removed per hour of non-favorable conditions. Slower decay
 * than accumulation, because a surface stays damp after the air dries.
 * Starting value to tune. */
#define MOLD_DRY_DECAY_PER_HOUR        0.5

/* Level thresholds on wet hours. A level is entered when wet hours reach
 * its ENTER value and left only when wet hours fall below its EXIT value,
 * so the level does not flap around a single threshold. Starting values
 * to tune. */
#define MOLD_WATCH_ENTER_HOURS         6.0
#define MOLD_WATCH_EXIT_HOURS          3.0
#define MOLD_RISK_ENTER_HOURS          24.0
#define MOLD_RISK_EXIT_HOURS           12.0

/* Largest time step, in seconds, that mold_update() will credit. A longer
 * gap (a crash, a restart, a sensor outage) is clamped to this so wet time
 * nobody measured is not counted. Starting value to tune. */
#define MOLD_MAX_DT_S                  300.0

/* Smallest RH, in %RH, that mold_dew_point_c() will use. log(0) is
 * undefined, so 0 %RH is treated as this instead. */
#define MOLD_RH_MIN_PCT                0.01

/* ---- Types ---- */

/* Overall assessment, lowest to highest. */
typedef enum {
    MOLD_OK    = 0,  /* conditions are not a concern */
    MOLD_WATCH = 1,  /* damp for a while; keep an eye on it */
    MOLD_RISK  = 2   /* damp long enough that mold growth is likely */
} mold_level_t;

/* One cycle's measurements. The caller fills this from the latest good
 * reading from each sensor. */
typedef struct {
    double air_temp_c;      /* air temperature, degrees C (Si7021); valid if have_air */
    double air_rh_pct;      /* air relative humidity, %RH (Si7021); valid if have_air */
    double surface_temp_c;  /* surface temperature, degrees C (DS18B20); valid if have_surface */
    bool   have_air;        /* false if the Si7021 has no good reading */
    bool   have_surface;    /* false if the DS18B20 has no good reading */
} mold_input_t;

/* State carried between calls. Initialize with mold_init(). */
typedef struct {
    double       wet_hours;  /* accumulated damp time, in hours; never below 0 */
    mold_level_t level;      /* current level, kept for the threshold hysteresis */
} mold_state_t;

/* What one mold_update() call worked out. */
typedef struct {
    double       dew_point_c;     /* dew point of the air, degrees C */
    double       surface_rh_pct;  /* estimated RH at the surface, 0 to 100 */
    bool         condensation;    /* surface is at or below the dew point */
    bool         favorable;       /* surface RH >= favorable threshold and
                                     surface temperature within the mold range */
    bool         degraded;        /* no surface reading: air temperature was used
                                     in its place, so surface RH equals air RH
                                     and condensation is never reported */
    mold_level_t level;           /* level after this update */
    double       wet_hours;       /* wet hours after this update */
} mold_result_t;

/* ---- Functions ---- */

/*
 * Saturation vapor pressure of water at `t_c` degrees C, in hPa, using the
 * Magnus formula: 6.112 * exp(17.62 * T / (243.12 + T)).
 */
double mold_sat_vapor_hpa(double t_c);

/*
 * Dew point, in degrees C, of air at `t_c` degrees C and `rh_pct` %RH
 * (Magnus formula, inverted). `rh_pct` is clamped to
 * MOLD_RH_MIN_PCT..100, so 0 %RH gives a very low dew point instead of an
 * error, and a reading above 100 %RH gives a dew point equal to `t_c`.
 */
double mold_dew_point_c(double t_c, double rh_pct);

/*
 * Relative humidity at a surface at `surface_t_c`, given air at `air_t_c`
 * and `air_rh_pct`. The air's vapor pressure stays the same, so the result
 * is air RH * e_s(air) / e_s(surface), clamped to 0..100. A surface cooler
 * than the air therefore sees a higher RH than the air does. `air_rh_pct`
 * is also clamped to 0..100 first, so an over-range reading cannot inflate
 * the result.
 */
double mold_surface_rh(double air_t_c, double air_rh_pct, double surface_t_c);

/* Set `s` to its starting state: no wet hours, level MOLD_OK. */
void mold_init(mold_state_t *s);

/*
 * Process one cycle. `dt_s` is the time since the previous call, in
 * seconds; it is clamped to 0..MOLD_MAX_DT_S and converted to hours.
 *
 * Each call:
 *   - works out the dew point and the surface RH. If `in->have_surface` is
 *     false, the air temperature stands in for the surface temperature and
 *     the result is marked degraded;
 *   - decides whether conditions are favorable;
 *   - if favorable, adds dt hours to wet_hours (times
 *     MOLD_WET_RATE_MULTIPLIER when surface RH >= MOLD_SURFACE_RH_WET_PCT);
 *     if not, subtracts MOLD_DRY_DECAY_PER_HOUR * dt hours, down to a
 *     minimum of 0;
 *   - updates the level from wet_hours using the ENTER/EXIT thresholds.
 *     RISK can drop straight to OK if wet_hours is already below
 *     MOLD_WATCH_EXIT_HOURS. The level is re-evaluated on every successful
 *     call, even when `dt_s` is 0.
 *
 * Returns 0 on success and fills in `out` (which must not be NULL). Returns
 * -1 if `in->have_air` is false: without air data nothing can be computed,
 * and `s` and `out` are left unchanged.
 */
int mold_update(mold_state_t *s, const mold_input_t *in, double dt_s,
                mold_result_t *out);

#endif /* MOLD_H */
