#ifndef APPLOG_H
#define APPLOG_H

#include <stddef.h>
#include <stdint.h>

#include "../common/sample.h"

/*
 * Append-only log of sensor readings (Mechanism D).
 *
 * On-disk format: the file is a sequence of fixed-size records, with
 * nothing else in it (no header):
 *
 *   offset  size  field
 *   0       4     magic     APPLOG_MAGIC, marks the start of a record
 *   4       24    sample    a sample_t, copied as raw bytes
 *   28      4     crc32     checksum of bytes 0..27
 *
 * Records are only ever added at the end. If power is lost mid-write, the
 * last record can be cut short or contain garbage. applog_open() detects
 * this, because the length is not a multiple of APPLOG_RECORD_SIZE or the
 * checksum does not match, and truncates the file back to the last good
 * record.
 *
 * Durability: applog_append() only hands data to the OS. Nothing is
 * guaranteed to be on the SD card until applog_sync() calls fsync(). The
 * hub calls applog_sync() every 5 minutes, so a power cut can lose up to
 * about 5 minutes of readings. Because writes only ever go to the end of
 * the file, records that were already synced are normally left untouched,
 * but this depends on the filesystem and SD card behaving correctly; a
 * hardware or filesystem fault can still damage older data.
 *
 * Functions return 0 on success and -1 on failure with errno set, unless
 * noted otherwise.
 */

#define APPLOG_MAGIC       0x4D4F4C44u  /* "MOLD" in ASCII */
#define APPLOG_RECORD_SIZE 32  /* 4 (magic) + sizeof(sample_t) + 4 (crc32) */

_Static_assert(APPLOG_RECORD_SIZE == 4 + sizeof(sample_t) + 4,
               "APPLOG_RECORD_SIZE must match the record layout");

/* Opaque handle; the struct is defined in applog.c. */
typedef struct applog applog_t;

/* What applog_open() found while checking the existing file. */
typedef struct {
    size_t records_ok;       /* valid records kept */
    size_t bytes_truncated;  /* bytes removed from the end (0 if the file was clean) */
} applog_recovery_t;

/*
 * Open the log at `path`, creating it if needed. Recovery always runs
 * first, so the file is valid before anything new is appended. If
 * `recovery` is not NULL, it is filled in with what was found.
 * Returns NULL on failure.
 */
applog_t *applog_open(const char *path, applog_recovery_t *recovery);

/* Add one reading to the end of the log. */
int applog_append(applog_t *log, const sample_t *sample);

/* Force everything appended so far onto storage (fsync). */
int applog_sync(applog_t *log);

/* Sync, then close the file and free the handle. Accepts NULL. */
int applog_close(applog_t *log);

/*
 * Read every valid record in the log at `path`, oldest first, calling
 * `visit` once per record. `visit` returns 0 to keep going or 1 to stop
 * early. Stops at the first bad record without modifying the file.
 * Returns 0 after reading everything, 1 if `visit` stopped it early, and
 * -1 on I/O error.
 */
typedef int (*applog_visit_fn)(const sample_t *sample, void *ctx);
int applog_read_all(const char *path, applog_visit_fn visit, void *ctx);

#endif /* APPLOG_H */
