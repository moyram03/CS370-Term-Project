#include "ds18b20.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SCRATCHPAD_BYTES 9

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
