/* Needed for pread, fsync, ftruncate, and O_CLOEXEC under -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "applog.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Byte offsets inside one 32-byte record (see the layout in applog.h). */
#define REC_MAGIC_OFF  0
#define REC_SAMPLE_OFF 4
#define REC_CRC_OFF    (REC_SAMPLE_OFF + (int)sizeof(sample_t))  /* 28 */

struct applog {
    int   fd;
    off_t size;  /* bytes of complete, valid records; where the next one goes */
};

/*
 * Standard CRC-32 (the one used by zip and Ethernet). Computed bit by bit
 * instead of with a lookup table: slower, but it only runs on 28 bytes
 * every 30 seconds, and it is much easier to read.
 */
static uint32_t crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/*
 * Build the on-disk bytes for one sample. Multi-byte fields are stored in
 * the Pi's native byte order (little-endian), the same as the raw sample_t.
 */
static void encode_record(uint8_t rec[APPLOG_RECORD_SIZE], const sample_t *sample)
{
    uint32_t magic = APPLOG_MAGIC;
    memcpy(rec + REC_MAGIC_OFF, &magic, sizeof magic);
    memcpy(rec + REC_SAMPLE_OFF, sample, sizeof *sample);

    uint32_t crc = crc32(rec, REC_CRC_OFF);
    memcpy(rec + REC_CRC_OFF, &crc, sizeof crc);
}

/* Check one record's magic and checksum. Returns 1 and fills `out` if valid, 0 if not. */
static int decode_record(const uint8_t rec[APPLOG_RECORD_SIZE], sample_t *out)
{
    uint32_t magic, crc;
    memcpy(&magic, rec + REC_MAGIC_OFF, sizeof magic);
    memcpy(&crc, rec + REC_CRC_OFF, sizeof crc);

    if (magic != APPLOG_MAGIC || crc != crc32(rec, REC_CRC_OFF))
        return 0;

    memcpy(out, rec + REC_SAMPLE_OFF, sizeof *out);
    return 1;
}

/*
 * pread() may return fewer bytes than asked for, or fail with EINTR if a
 * signal arrives. Keep going until `len` bytes are read or the file ends.
 * Returns the number of bytes read (less than `len` only at end of file),
 * or -1 on error.
 */
static ssize_t pread_full(int fd, void *buf, size_t len, off_t off)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, (uint8_t *)buf + got, len - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

/* Same idea for write(): returns 0 once all `len` bytes are written, -1 on error. */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const uint8_t *)buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/*
 * Walk the file from the start, one record at a time, until the end of the
 * file or the first bad record. Calls `visit` on each valid record if it is
 * not NULL. Sets *good_end to the byte offset just past the last valid
 * record and *count to how many valid records were seen.
 * Returns 0 when it reached the end or a bad record, 1 if `visit` asked to
 * stop, and -1 on I/O error.
 */
static int scan_records(int fd, applog_visit_fn visit, void *ctx,
                        off_t *good_end, size_t *count)
{
    uint8_t rec[APPLOG_RECORD_SIZE];
    sample_t sample;

    *good_end = 0;
    *count = 0;

    for (;;) {
        ssize_t n = pread_full(fd, rec, sizeof rec, *good_end);
        if (n < 0)
            return -1;
        if ((size_t)n < sizeof rec)   /* end of file, or a cut-off last record */
            return 0;
        if (!decode_record(rec, &sample))
            return 0;

        *good_end += APPLOG_RECORD_SIZE;
        (*count)++;

        if (visit != NULL && visit(&sample, ctx) != 0)
            return 1;
    }
}

/*
 * A newly created file only survives a power cut once its directory entry
 * is on disk too, which needs an fsync() on the directory itself.
 */
static int sync_parent_dir(const char *path)
{
    char *copy = strdup(path);
    if (copy == NULL)
        return -1;

    int dfd = open(dirname(copy), O_RDONLY | O_CLOEXEC);
    free(copy);
    if (dfd < 0)
        return -1;

    int rc = fsync(dfd);
    int saved = errno;
    close(dfd);
    errno = saved;
    return rc;
}

applog_t *applog_open(const char *path, applog_recovery_t *recovery)
{
    /* O_APPEND makes every write() go to the current end of the file. */
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return NULL;

    struct stat st;
    off_t good_end;
    size_t count;

    if (fstat(fd, &st) < 0 || scan_records(fd, NULL, NULL, &good_end, &count) < 0)
        goto fail;

    /* Anything after the last good record is a write that power loss cut
     * short. Remove it, then fsync so the repair itself is durable. */
    if (st.st_size > good_end) {
        if (ftruncate(fd, good_end) < 0 || fsync(fd) < 0)
            goto fail;
    }

    if (sync_parent_dir(path) < 0)
        goto fail;

    applog_t *log = malloc(sizeof *log);
    if (log == NULL)
        goto fail;
    log->fd = fd;
    log->size = good_end;

    if (recovery != NULL) {
        recovery->records_ok = count;
        recovery->bytes_truncated = (size_t)(st.st_size - good_end);
    }
    return log;

fail:;
    int saved = errno;
    close(fd);
    errno = saved;
    return NULL;
}

int applog_append(applog_t *log, const sample_t *sample)
{
    /* Work on a copy so the reserved bytes are always written as zero,
     * whatever the caller left in them. */
    sample_t clean = *sample;
    memset(clean.reserved, 0, sizeof clean.reserved);

    uint8_t rec[APPLOG_RECORD_SIZE];
    encode_record(rec, &clean);

    if (write_full(log->fd, rec, sizeof rec) < 0) {
        /* Part of the record may have been written (for example, the disk
         * filled up halfway through). Cut it off so the next append still
         * starts on a record boundary. */
        int saved = errno;
        if (ftruncate(log->fd, log->size) < 0) {
            /* Leave the original error; applog_open() repairs this on restart. */
        }
        errno = saved;
        return -1;
    }

    log->size += APPLOG_RECORD_SIZE;
    return 0;
}

int applog_sync(applog_t *log)
{
    return fsync(log->fd);
}

int applog_close(applog_t *log)
{
    if (log == NULL)
        return 0;

    int rc = fsync(log->fd);
    int saved = errno;
    if (close(log->fd) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    free(log);
    errno = saved;
    return rc;
}

int applog_read_all(const char *path, applog_visit_fn visit, void *ctx)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    off_t good_end;
    size_t count;
    int rc = scan_records(fd, visit, ctx, &good_end, &count);

    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}
