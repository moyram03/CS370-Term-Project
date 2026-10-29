#ifndef SAMPLE_H
#define SAMPLE_H

#include <stdint.h>

/*
 * One reading from one sensor. This struct is passed between the sensor
 * processes and the hub, and it is also written byte-for-byte into the
 * storage log, so it only uses fixed-width types and has an exact size.
 */

/* Which sensor produced the reading. */
typedef enum {
    SENSOR_SI7021_HUMIDITY = 1,  /* value is relative humidity, in %RH */
    SENSOR_DS18B20_TEMP    = 2,  /* value is surface temperature, in degrees C */
    SENSOR_SI7021_TEMP     = 3   /* value is air temperature, in degrees C */
} sensor_id_t;

/* Whether the reading is valid. Failures are recorded too, so the log
 * shows when a sensor went down and when it recovered. */
typedef enum {
    SAMPLE_OK          = 0,  /* value is valid */
    SAMPLE_ERR_READ    = 1,  /* sensor returned an error or bad data */
    SAMPLE_ERR_TIMEOUT = 2   /* sensor did not answer in time */
} sample_status_t;

typedef struct {
    int64_t  timestamp_ms;  /* wall-clock time, milliseconds since the Unix epoch */
    uint32_t seq;           /* per-sensor-process counter: starts at 0 when the process
                               starts and goes up by 1 per reading. A gap means readings
                               were lost; a drop back to 0 means the process restarted. */
    float    value;         /* only meaningful when status == SAMPLE_OK */
    uint8_t  sensor;        /* a sensor_id_t */
    uint8_t  status;        /* a sample_status_t */
    uint8_t  reserved[6];   /* always 0; fills the gap the compiler would pad anyway */
} sample_t;

/* The log format depends on this size; fail the build if it ever changes. */
_Static_assert(sizeof(sample_t) == 24, "sample_t must be 24 bytes");

#endif /* SAMPLE_H */
