#include "nmea.h"

#include <string.h>

#define KNOT_NUM 1852 /* 1 knot = 1852 m/h = 1852/36 cm/s */

void nmea_init(nmea_t *p)
{
    memset(p, 0, sizeof(*p));
    p->ep_key = -1;
}

/* ---- numbers ---- */

/* Decimal "[-]ddd[.ddd]" scaled by 10^dec, rounded half up on the first dropped
 * digit. 1..9 integer digits, at most 12 fraction digits. Empty/other -> -1. */
static int parse_dec(const char *s, unsigned dec, bool neg_ok, int64_t *out)
{
    bool neg = false;
    if (*s == '-') {
        if (!neg_ok) {
            return -1;
        }
        neg = true;
        s++;
    }
    int nd = 0;
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        if (++nd > 9) {
            return -1;
        }
        v = v * 10 + (*s - '0');
        s++;
    }
    if (nd == 0) {
        return -1;
    }
    for (unsigned i = 0; i < dec; i++) {
        v *= 10;
    }
    if (*s == '.') {
        s++;
        unsigned i = 0;
        while (*s >= '0' && *s <= '9') {
            if (i >= 12) {
                return -1;
            }
            int d = *s - '0';
            if (i < dec) {
                int64_t scale = 1;
                for (unsigned k = i + 1; k < dec; k++) {
                    scale *= 10;
                }
                v += d * scale;
            } else if (i == dec && d >= 5) {
                v++;
            }
            i++;
            s++;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    *out = neg ? -v : v;
    return 0;
}

static uint32_t clamp_u(int64_t v, uint32_t max)
{
    if (v < 0) {
        return 0;
    }
    return v > (int64_t)max ? max : (uint32_t)v;
}

static bool all_digits(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static int dig2(const char *s)
{
    return (s[0] - '0') * 10 + (s[1] - '0');
}

/* hhmmss[.f...] -> ms of day, -1 invalid. */
static int32_t parse_time(const char *s)
{
    if (!all_digits(s, 6)) {
        return -1;
    }
    int hh = dig2(s), mm = dig2(s + 2), ss = dig2(s + 4);
    if (hh > 23 || mm > 59 || ss > 59) {
        return -1;
    }
    int ms = 0;
    s += 6;
    if (*s == '.') {
        s++;
        int scale = 100;
        while (*s >= '0' && *s <= '9') {
            ms += (*s - '0') * scale;
            scale /= 10;
            s++;
        }
    }
    if (*s != '\0') {
        return -1;
    }
    return ((hh * 60 + mm) * 60 + ss) * 1000 + ms;
}

static int days_in_month(int year, int month)
{
    static const uint8_t dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int d = dm[month - 1];
    if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        d = 29;
    }
    return d;
}

int32_t nmea_days_from_civil(int year, int month, int day)
{
    if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month)) {
        return -1;
    }
    /* Civil-from-days inverse with March as the first month of the shifted year. */
    int y = year - (month <= 2 ? 1 : 0);
    int era = y / 400; /* y >= 1969 so no negative division */
    int yoe = y - era * 400;
    int mp = (month + 9) % 12; /* March = 0 */
    int doy = (153 * mp + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - 719468);
}

#define NMEA_MIN_YEAR 2025
#define NMEA_DATE_STALE (-2) /* well-formed but year < NMEA_MIN_YEAR: the clock is not trusted */

/* ddmmyy -> days since epoch, -1 invalid, NMEA_DATE_STALE for year < NMEA_MIN_YEAR. */
static int32_t parse_date(const char *s)
{
    if (!all_digits(s, 6) || s[6] != '\0') {
        return -1;
    }
    int year = 2000 + dig2(s + 4);
    if (year < NMEA_MIN_YEAR) { /* GPS week-rollover clones report 2005/2006 dates */
        return NMEA_DATE_STALE;
    }
    return nmea_days_from_civil(year, dig2(s + 2), dig2(s));
}

/* [d]ddmm.mmmm + hemisphere -> degrees * 1e7. int_digits = 4 (lat) or 5 (lon). */
static int parse_coord(const char *s, const char *hemi, int int_digits, int32_t max_deg,
                       char neg_h, char pos_h, int32_t *out)
{
    if (hemi[0] == '\0' || hemi[1] != '\0' || (hemi[0] != neg_h && hemi[0] != pos_h)) {
        return -1;
    }
    int n = 0;
    while (s[n] >= '0' && s[n] <= '9') {
        n++;
    }
    if (n != int_digits || (s[n] != '\0' && s[n] != '.')) {
        return -1;
    }
    int deg = 0;
    for (int i = 0; i < int_digits - 2; i++) {
        deg = deg * 10 + (s[i] - '0');
    }
    int64_t min_e7;
    if (parse_dec(s + int_digits - 2, 7, false, &min_e7) != 0 ||
        min_e7 >= 60LL * 10000000LL || deg > max_deg || (deg == max_deg && min_e7 != 0)) {
        return -1;
    }
    int64_t v = (int64_t)deg * 10000000LL + (min_e7 + 30) / 60;
    *out = (int32_t)(hemi[0] == neg_h ? -v : v);
    return 0;
}

/* ---- sentences ---- */

static bool fld_empty(const char *f)
{
    return f[0] == '\0';
}

/* Fields: 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 speed, 8 course, 9 date,
 * (12 mode, NMEA 2.3). Returns the time key. */
static int32_t decode_rmc(char *const *f, size_t nf, rec_gps_fix_t *o)
{
    memset(o, 0, sizeof(*o));
    int32_t tod = fld_empty(f[1]) ? -1 : parse_time(f[1]);
    bool active = f[2][0] == 'A' && f[2][1] == '\0';
    if (nf > 12 && f[12][0] == 'N') {
        active = false;
    }
    int32_t days = fld_empty(f[9]) ? -1 : parse_date(f[9]);
    if (active && tod >= 0 && days != NMEA_DATE_STALE) {
        o->flags |= GPS_FLAG_TIME_VALID;
    }
    if (active && days >= 0) {
        o->flags |= GPS_FLAG_DATE_VALID;
    }
    if ((o->flags & (GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID)) ==
        (GPS_FLAG_TIME_VALID | GPS_FLAG_DATE_VALID)) {
        o->utc_unix_ms = ((int64_t)days * 86400 + tod / 1000) * 1000 + tod % 1000;
    }
    int32_t lat, lon;
    if (active && parse_coord(f[3], f[4], 4, 90, 'S', 'N', &lat) == 0 &&
        parse_coord(f[5], f[6], 5, 180, 'W', 'E', &lon) == 0) {
        o->lat_e7 = lat;
        o->lon_e7 = lon;
        o->flags |= GPS_FLAG_POS_VALID;
    }
    int64_t v;
    if (active && parse_dec(f[7], 2, false, &v) == 0) {
        o->speed_cmps = clamp_u((v * KNOT_NUM + 1800) / 3600, 65535);
    }
    if (active && parse_dec(f[8], 2, false, &v) == 0 && v <= 36000) {
        o->course_cdeg = (uint16_t)v;
    }
    return tod;
}

/* Fields: 1 time, 2 lat, 3 N/S, 4 lon, 5 E/W, 6 quality, 7 sats, 8 hdop, 9 altitude,
 * 10 altitude unit. */
static int32_t decode_gga(char *const *f, rec_gps_fix_t *o)
{
    memset(o, 0, sizeof(*o));
    int32_t tod = fld_empty(f[1]) ? -1 : parse_time(f[1]);
    int64_t v;
    if (parse_dec(f[6], 0, false, &v) == 0) {
        o->fix_quality = (uint8_t)clamp_u(v, 255);
    }
    bool fixed = o->fix_quality > 0;
    if (fixed && tod >= 0) {
        o->flags |= GPS_FLAG_TIME_VALID;
    }
    int32_t lat, lon;
    if (fixed && parse_coord(f[2], f[3], 4, 90, 'S', 'N', &lat) == 0 &&
        parse_coord(f[4], f[5], 5, 180, 'W', 'E', &lon) == 0) {
        o->lat_e7 = lat;
        o->lon_e7 = lon;
        o->flags |= GPS_FLAG_POS_VALID;
    }
    if (parse_dec(f[7], 0, false, &v) == 0) {
        o->sats = (uint8_t)clamp_u(v, 255);
    }
    if (parse_dec(f[8], 2, false, &v) == 0) {
        o->hdop_x100 = (uint16_t)clamp_u(v, 65535);
    }
    if (fixed && f[10][0] == 'M' && f[10][1] == '\0' && parse_dec(f[9], 2, true, &v) == 0) {
        if (v > INT32_MAX) {
            v = INT32_MAX;
        } else if (v < INT32_MIN) {
            v = INT32_MIN;
        }
        o->alt_cm = (int32_t)v;
        o->flags |= GPS_FLAG_ALT_VALID;
    }
    return tod;
}

/* ---- epochs ---- */

static void emit_epoch(nmea_t *p, nmea_cb_t cb, void *ctx)
{
    if (!p->ep_active) {
        return;
    }
    nmea_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = NMEA_EV_FIX;
    ev.have_rmc = p->ep_have_rmc;
    ev.have_gga = p->ep_have_gga;
    const rec_gps_fix_t *r = &p->ep_rmc;
    const rec_gps_fix_t *g = &p->ep_gga;
    rec_gps_fix_t *o = &ev.fix;
    o->flags = (uint8_t)(r->flags | g->flags);
    o->utc_unix_ms = r->utc_unix_ms;
    if (r->flags & GPS_FLAG_POS_VALID) {
        o->lat_e7 = r->lat_e7;
        o->lon_e7 = r->lon_e7;
    } else if (g->flags & GPS_FLAG_POS_VALID) {
        o->lat_e7 = g->lat_e7;
        o->lon_e7 = g->lon_e7;
    }
    o->alt_cm = g->alt_cm;
    o->speed_cmps = r->speed_cmps;
    o->course_cdeg = r->course_cdeg;
    o->hdop_x100 = g->hdop_x100;
    o->sats = g->sats;
    o->fix_quality = g->fix_quality;
    p->ep_active = false;
    p->ep_have_rmc = false;
    p->ep_have_gga = false;
    if (cb != NULL) {
        cb(&ev, ctx);
    }
}

void nmea_flush(nmea_t *p, nmea_cb_t cb, void *ctx)
{
    emit_epoch(p, cb, ctx);
}

/* Add one decoded sentence to the epochs. */
static void add_to_epoch(nmea_t *p, bool is_rmc, int32_t key, const rec_gps_fix_t *fix,
                         nmea_cb_t cb, void *ctx)
{
    if (p->ep_active && (key != p->ep_key || (is_rmc ? p->ep_have_rmc : p->ep_have_gga))) {
        emit_epoch(p, cb, ctx);
    }
    if (is_rmc && cb != NULL) {
        nmea_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = NMEA_EV_RMC;
        ev.fix = *fix;
        ev.have_rmc = true;
        cb(&ev, ctx);
    }
    if (!p->ep_active) {
        p->ep_active = true;
        p->ep_key = key;
        memset(&p->ep_rmc, 0, sizeof(p->ep_rmc));
        memset(&p->ep_gga, 0, sizeof(p->ep_gga));
    }
    if (is_rmc) {
        p->ep_rmc = *fix;
        p->ep_have_rmc = true;
    } else {
        p->ep_gga = *fix;
        p->ep_have_gga = true;
    }
    if (p->ep_have_rmc && p->ep_have_gga) {
        emit_epoch(p, cb, ctx);
    }
}

/* ---- lines ---- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static void process_line(nmea_t *p, nmea_cb_t cb, void *ctx)
{
    char *l = p->line;
    size_t n = p->len;
    l[n] = '\0';
    /* "$" body "*HH" */
    if (n < 4 || l[n - 3] != '*') {
        p->bad_lines++;
        return;
    }
    int h1 = hexval(l[n - 2]), h2 = hexval(l[n - 1]);
    if (h1 < 0 || h2 < 0) {
        p->bad_lines++;
        return;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < n - 3; i++) {
        if ((unsigned char)l[i] < 0x20 || (unsigned char)l[i] > 0x7E) {
            p->bad_lines++;
            return;
        }
        sum ^= (uint8_t)l[i];
    }
    if (sum != (uint8_t)(h1 * 16 + h2)) {
        p->bad_lines++;
        return;
    }
    l[n - 3] = '\0';

    /* Split the body on commas in place. */
    static char empty[1] = {0};
    char *f[NMEA_MAX_FIELDS];
    size_t nf = 0;
    char *s = l + 1;
    f[nf++] = s;
    for (; *s != '\0'; s++) {
        if (*s == ',') {
            *s = '\0';
            if (nf < NMEA_MAX_FIELDS) {
                f[nf++] = s + 1;
            }
        }
    }
    for (size_t i = nf; i < NMEA_MAX_FIELDS; i++) {
        f[i] = empty;
    }
    const char *a = f[0];
    bool known = strlen(a) == 5 && a[0] == 'G' && (a[1] == 'P' || a[1] == 'N');
    bool is_rmc = known && strcmp(a + 2, "RMC") == 0;
    bool is_gga = known && strcmp(a + 2, "GGA") == 0;
    if (!is_rmc && !is_gga) {
        p->good_lines++;
        return;
    }
    if ((is_rmc && nf < 10) || (is_gga && nf < 10)) {
        p->bad_lines++;
        return;
    }
    p->good_lines++;
    rec_gps_fix_t fix;
    if (is_rmc) {
        int32_t key = decode_rmc(f, nf, &fix);
        add_to_epoch(p, true, key, &fix, cb, ctx);
    } else {
        int32_t key = decode_gga(f, &fix);
        add_to_epoch(p, false, key, &fix, cb, ctx);
    }
}

void nmea_feed(nmea_t *p, const uint8_t *data, size_t len, nmea_cb_t cb, void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '$') {
            if (p->state == NMEA_ST_LINE) {
                p->dropped_lines++; /* partial line abandoned */
            }
            /* In SKIP the overflow was already counted. */
            p->state = NMEA_ST_LINE;
            p->line[0] = '$';
            p->len = 1;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (p->state == NMEA_ST_LINE) {
                process_line(p, cb, ctx);
            }
            p->state = NMEA_ST_IDLE;
            p->len = 0;
            continue;
        }
        if (p->state == NMEA_ST_LINE) {
            if (p->len >= NMEA_MAX_LINE) {
                p->dropped_lines++;
                p->state = NMEA_ST_SKIP;
                p->len = 0;
            } else {
                p->line[p->len++] = (char)c;
            }
        }
    }
}
