/* Needed for opendir, readdir, and open under -std=c17. */
#define _XOPEN_SOURCE 700

#include "ds18b20.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCRATCHPAD_BYTES 9
#define DEVICE_PREFIX    "28-"
#define NAME_BUF_SIZE    256  /* a directory entry name is at most 255 bytes */

/* Blanks that may trail a line: not the newline itself. */
static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Value of one hex digit, or -1 if `c` is not one. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/*
 * True if text[start, end) is exactly nine hex bytes separated by blanks
 * and every one is 00. Anything else (other byte count, non-hex text,
 * any nonzero byte) is false.
 */
static bool scratchpad_all_zero(const char *start, const char *end)
{
    int count = 0;
    const char *p = start;
    while (true) {
        while (p < end && is_blank(*p)) {
            p++;
        }
        if (p >= end) {
            break;
        }
        if (p + 2 > end) {
            return false;
        }
        int hi = hex_value(p[0]);
        int lo = hex_value(p[1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        if (hi != 0 || lo != 0) {
            return false;
        }
        p += 2;
        if (p < end && !is_blank(*p)) {
            return false;
        }
        count++;
    }
    return count == SCRATCHPAD_BYTES;
}

sample_status_t ds18b20_parse(const char *text, float *temp_c)
{
    if (text == NULL || temp_c == NULL) {
        return SAMPLE_ERR_READ;
    }

    /* Line 1 must end in "YES", ignoring trailing blanks. */
    const char *newline = strchr(text, '\n');
    if (newline == NULL) {
        return SAMPLE_ERR_READ;
    }
    const char *line1_end = newline;
    while (line1_end > text && is_blank(line1_end[-1])) {
        line1_end--;
    }
    if (line1_end - text < 3 || strncmp(line1_end - 3, "YES", 3) != 0) {
        return SAMPLE_ERR_READ;
    }

    /* Line 2 must have "t=" followed by a number and nothing else. */
    const char *line2 = newline + 1;
    const char *t_eq = strstr(line2, "t=");
    if (t_eq == NULL) {
        return SAMPLE_ERR_READ;
    }
    const char *digits = t_eq + 2;
    if (*digits != '-' && !isdigit((unsigned char)*digits)) {
        return SAMPLE_ERR_READ;
    }
    char *num_end = NULL;
    errno = 0;
    long milli_c = strtol(digits, &num_end, 10);
    if (errno != 0 || num_end == digits) {
        return SAMPLE_ERR_READ;
    }
    while (*num_end != '\0') {
        if (!isspace((unsigned char)*num_end)) {
            return SAMPLE_ERR_READ;
        }
        num_end++;
    }

    if (milli_c == DS18B20_POWER_ON_MILLI_C) {
        return SAMPLE_ERR_READ;
    }
    if (milli_c < DS18B20_MIN_C * 1000L || milli_c > DS18B20_MAX_C * 1000L) {
        return SAMPLE_ERR_READ;
    }
    if (scratchpad_all_zero(line2, t_eq)) {
        return SAMPLE_ERR_READ;
    }

    *temp_c = (float)milli_c / 1000.0f;
    return SAMPLE_OK;
}

int ds18b20_find(const char *w1_dir, char *path_out, size_t len)
{
    if (w1_dir == NULL || path_out == NULL || len == 0) {
        return -1;
    }
    path_out[0] = '\0';

    DIR *dir = opendir(w1_dir);
    if (dir == NULL) {
        return -1;
    }

    /* Scan every entry and keep the smallest "28-*" name. */
    char best[NAME_BUF_SIZE];
    bool have_best = false;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, DEVICE_PREFIX, strlen(DEVICE_PREFIX)) == 0) {
            size_t name_len = strlen(entry->d_name);
            if (name_len < sizeof best &&
                (!have_best || strcmp(entry->d_name, best) < 0)) {
                memcpy(best, entry->d_name, name_len + 1);
                have_best = true;
            }
        }
        errno = 0;
    }
    bool listing_failed = (errno != 0);  /* readdir returns NULL on error too */

    /* Nothing useful to do if closing a directory we only read fails. */
    closedir(dir);

    if (listing_failed || !have_best) {
        return -1;
    }

    int n = snprintf(path_out, len, "%s/%s/w1_slave", w1_dir, best);
    if (n < 0 || (size_t)n >= len) {
        path_out[0] = '\0';
        return -1;
    }
    return 0;
}

sample_status_t ds18b20_read(const char *slave_path, float *temp_c)
{
    if (slave_path == NULL || temp_c == NULL) {
        return SAMPLE_ERR_READ;
    }

    int fd = open(slave_path, O_RDONLY);
    if (fd < 0) {
        return SAMPLE_ERR_READ;
    }

    char buf[DS18B20_READ_MAX_BYTES + 1];
    size_t used = 0;
    bool at_eof = false;
    bool failed = false;
    while (!at_eof && !failed) {
        if (used >= DS18B20_READ_MAX_BYTES) {
            failed = true;  /* too big to be a w1_slave file */
        } else {
            ssize_t n = read(fd, buf + used, DS18B20_READ_MAX_BYTES - used);
            if (n < 0) {
                if (errno != EINTR) {
                    failed = true;
                }
            } else if (n == 0) {
                at_eof = true;
            } else {
                used += (size_t)n;
            }
        }
    }

    /* The data is already read, so a failed close of a read-only fd does
     * not change the result. */
    close(fd);

    if (failed) {
        return SAMPLE_ERR_READ;
    }
    buf[used] = '\0';
    return ds18b20_parse(buf, temp_c);
}
