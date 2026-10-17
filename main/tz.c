#include <ctype.h>
#include <stddef.h>
#include <string.h>

#include "tz.h"

typedef struct {
    int     month;   /* 1-12 */
    int     week;    /* 1-4, or 5 for the last */
    int     wday;    /* 0 Sunday - 6 Saturday */
    int32_t time_s;  /* local time of day it happens at, in the time it ends */
} tz_rule_t;

typedef struct {
    int32_t   std_east;
    int32_t   dst_east;
    bool      has_dst;
    tz_rule_t start;  /* into daylight time */
    tz_rule_t end;    /* back to standard time */
} tz_t;

static bool parse_name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        const char *e = strchr(s + 1, '>');
        if (!e || e == s + 1) {
            return false;
        }
        for (const char *c = s + 1; c < e; c++) {
            if (!isalnum((unsigned char)*c) && *c != '+' && *c != '-') {
                return false;
            }
        }
        *p = e + 1;
        return true;
    }
    int n = 0;
    while (isalpha((unsigned char)s[n])) {
        n++;
    }
    if (n < 3) {
        return false;
    }
    *p = s + n;
    return true;
}

static bool parse_uint(const char **p, int max_digits, int *out)
{
    int v = 0, n = 0;
    while (n < max_digits && isdigit((unsigned char)**p)) {
        v = v * 10 + (**p - '0');
        (*p)++;
        n++;
    }
    if (n == 0) {
        return false;
    }
    *out = v;
    return true;
}

/* [+-]h[h...][:mm[:ss]], in seconds. `max_h` bounds the hours. */
static bool parse_hms(const char **p, int max_h, int32_t *out)
{
    int sign = 1;
    if (**p == '+' || **p == '-') {
        sign = **p == '-' ? -1 : 1;
        (*p)++;
    }
    int h = 0, m = 0, s = 0;
    if (!parse_uint(p, 3, &h) || h > max_h) {
        return false;
    }
    if (**p == ':') {
        (*p)++;
        if (!parse_uint(p, 2, &m) || m > 59) {
            return false;
        }
        if (**p == ':') {
            (*p)++;
            if (!parse_uint(p, 2, &s) || s > 59) {
                return false;
            }
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

static bool parse_rule(const char **p, tz_rule_t *r)
{
    if (**p != 'M') {
        return false;
    }
    (*p)++;
    if (!parse_uint(p, 2, &r->month) || r->month < 1 || r->month > 12 || **p != '.') {
        return false;
    }
    (*p)++;
    if (!parse_uint(p, 1, &r->week) || r->week < 1 || r->week > 5 || **p != '.') {
        return false;
    }
    (*p)++;
    if (!parse_uint(p, 1, &r->wday) || r->wday > 6) {
        return false;
    }
    r->time_s = 7200;
    if (**p == '/') {
        (*p)++;
        /* RFC 8536 allows -167 to 167 hours here, for rules like "the Saturday
         * before the last Sunday at 24:00". */
        if (!parse_hms(p, 167, &r->time_s)) {
            return false;
        }
    }
    return true;
}

static bool parse(const char *s, tz_t *tz)
{
    if (!s || !*s || strlen(s) > ZBC_TZ_MAX_LEN) {
        return false;
    }
    const char *p = s;
    int32_t     west;
    if (!parse_name(&p) || !parse_hms(&p, 24, &west)) {
        return false;
    }
    memset(tz, 0, sizeof(*tz));
    tz->std_east = -west;
    tz->dst_east = -west;
    if (!*p) {
        return true;
    }
    if (!parse_name(&p)) {
        return false;
    }
    tz->has_dst  = true;
    tz->dst_east = tz->std_east + 3600;
    if (*p && *p != ',') {
        if (!parse_hms(&p, 24, &west)) {
            return false;
        }
        tz->dst_east = -west;
    }
    if (*p != ',') {
        return false;
    }
    p++;
    if (!parse_rule(&p, &tz->start) || *p != ',') {
        return false;
    }
    p++;
    return parse_rule(&p, &tz->end) && !*p;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* Days from 1970-01-01 to a date of the proleptic Gregorian calendar. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int year_of_days(int64_t z)
{
    z += 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;
    const int     m   = (int)(mp < 10 ? mp + 3 : mp - 9);
    return (int)(yoe + era * 400 + (m <= 2));
}

static int days_in_month(int y, int m)
{
    static const int DAYS[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        return 29;
    }
    return DAYS[m - 1];
}

/* The moment a rule fires in year `y`, in UTC, given the offset in force until
 * then. */
static int64_t rule_utc(int y, const tz_rule_t *r, int32_t east_before)
{
    const int64_t first = days_from_civil(y, r->month, 1);
    const int     wday1 = (int)((first % 7 + 11) % 7);   /* 1970-01-01 was a Thursday */
    int           day   = 1 + (r->wday - wday1 + 7) % 7 + 7 * (r->week - 1);
    while (day > days_in_month(y, r->month)) {
        day -= 7;
    }
    return (first + day - 1) * 86400 + r->time_s - east_before;
}

bool zbc_tz_valid(const char *posix)
{
    tz_t tz;
    return parse(posix, &tz);
}

int32_t zbc_tz_offset(const char *posix, int64_t utc_s)
{
    tz_t tz;
    if (!parse(posix, &tz)) {
        return 0;
    }
    if (!tz.has_dst) {
        return tz.std_east;
    }
    const int     y     = year_of_days(floor_div(utc_s + tz.std_east, 86400));
    const int64_t start = rule_utc(y, &tz.start, tz.std_east);
    const int64_t end   = rule_utc(y, &tz.end, tz.dst_east);
    /* The southern hemisphere's daylight time spans the new year, so its start
     * comes after its end within one year. */
    const bool dst = start < end ? (utc_s >= start && utc_s < end) : !(utc_s >= end && utc_s < start);
    return dst ? tz.dst_east : tz.std_east;
}
