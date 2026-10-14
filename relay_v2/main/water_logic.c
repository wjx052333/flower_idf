/**
 * water_logic.c — Watering strategy configuration and decision logic
 *
 * See water_logic.h. No ESP-IDF dependencies: this file is compiled both into
 * the firmware and into the host-side test binary.
 */
#include "water_logic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEF_SURFACE_THRESH_PCT 50
#define DEF_SURFACE_DUR_S      60
#define DEF_DEEP_THRESH_PCT    10
#define DEF_DEEP_DUR_S         1200

#define NOON_HOUR              12

/* Read by water_strategy_eval() on the 30 s status-timer task and written by
 * water_config_apply_query() on the HTTP task. The write is a single struct
 * assignment, so the worst case is one decision seeing a half-updated config,
 * which is harmless here. No lock is used because this file must stay free of
 * RTOS dependencies so it can also be built for the host test binary. */
static water_config_t s_cfg = {
    .strategy           = WATER_STRATEGY_NOON,
    .surface_thresh_pct = DEF_SURFACE_THRESH_PCT,
    .surface_dur_s      = DEF_SURFACE_DUR_S,
    .deep_thresh_pct    = DEF_DEEP_THRESH_PCT,
    .deep_dur_s         = DEF_DEEP_DUR_S,
};

static bool   s_noon_done_today = false;
static time_t s_last_auto_water = 0;

water_config_t *water_config(void)
{
    return &s_cfg;
}

void water_config_reset(void)
{
    s_cfg.strategy           = WATER_STRATEGY_NOON;
    s_cfg.surface_thresh_pct = DEF_SURFACE_THRESH_PCT;
    s_cfg.surface_dur_s      = DEF_SURFACE_DUR_S;
    s_cfg.deep_thresh_pct    = DEF_DEEP_THRESH_PCT;
    s_cfg.deep_dur_s         = DEF_DEEP_DUR_S;
}

const char *water_strategy_name(uint8_t strategy)
{
    switch (strategy) {
    case WATER_STRATEGY_NOON:    return "中午12点";
    case WATER_STRATEGY_SURFACE: return "浇表面";
    case WATER_STRATEGY_DEEP:    return "干了再浇透";
    default:                     return "未知";
    }
}

/* ── Query parsing ───────────────────────────────────────────────────────── */
static bool set_pct(const char *key, long v, uint16_t *dst, char *err, size_t errlen)
{
    if (v < 0 || v > 100) {
        snprintf(err, errlen, "%s: 0-100 required", key);
        return false;
    }
    *dst = (uint16_t)v;
    return true;
}

static bool set_dur(const char *key, long v, uint16_t *dst, char *err, size_t errlen)
{
    if (v < 1 || v > 65535) {
        snprintf(err, errlen, "%s: 1-65535 required", key);
        return false;
    }
    *dst = (uint16_t)v;
    return true;
}

static bool apply_pair(water_config_t *cfg, char *pair, char *err, size_t errlen)
{
    char *eq = strchr(pair, '=');
    if (eq == NULL) {
        snprintf(err, errlen, "bad parameter");
        return false;
    }
    *eq = '\0';
    const char *key = pair;
    const char *val = eq + 1;

    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0') {
        snprintf(err, errlen, "%s: not a number", key);
        return false;
    }

    if (strcmp(key, "strategy") == 0) {
        if (v < 0 || v >= WATER_STRATEGY_COUNT) {
            snprintf(err, errlen, "strategy: 0-%d required", WATER_STRATEGY_COUNT - 1);
            return false;
        }
        cfg->strategy = (uint8_t)v;
        return true;
    }
    if (strcmp(key, "surface_thresh_pct") == 0)
        return set_pct(key, v, &cfg->surface_thresh_pct, err, errlen);
    if (strcmp(key, "surface_dur_s") == 0)
        return set_dur(key, v, &cfg->surface_dur_s, err, errlen);
    if (strcmp(key, "deep_thresh_pct") == 0)
        return set_pct(key, v, &cfg->deep_thresh_pct, err, errlen);
    if (strcmp(key, "deep_dur_s") == 0)
        return set_dur(key, v, &cfg->deep_dur_s, err, errlen);

    snprintf(err, errlen, "%s: unknown field", key);
    return false;
}

bool water_config_apply_query(const char *query, char *err, size_t errlen)
{
    if (errlen > 0)
        err[0] = '\0';
    if (query == NULL || query[0] == '\0')
        return true;

    char buf[192];
    if (strlen(query) >= sizeof(buf)) {
        snprintf(err, errlen, "query too long");
        return false;
    }
    strcpy(buf, query);

    /* Parse into a copy so a bad field anywhere leaves the live configuration
     * untouched, and so the commit below is a single struct assignment. */
    water_config_t next = s_cfg;
    bool ok = true;
    char *p = buf;
    while (*p != '\0') {
        char *amp = strchr(p, '&');
        if (amp != NULL)
            *amp = '\0';
        if (*p != '\0' && !apply_pair(&next, p, err, errlen)) {
            ok = false;
            break;
        }
        if (amp == NULL)
            break;
        p = amp + 1;
    }
    if (!ok)
        return false;

    s_cfg = next;
    return true;
}

int water_config_to_json(char *buf, size_t n)
{
    return snprintf(buf, n,
        "{\"strategy\":%u,\"strategy_name\":\"%s\","
        "\"surface_thresh_pct\":%u,\"surface_dur_s\":%u,"
        "\"deep_thresh_pct\":%u,\"deep_dur_s\":%u,"
        "\"check_window\":\"%02d:00-%02d:00\",\"cooldown_s\":%d}",
        (unsigned)s_cfg.strategy,
        water_strategy_name(s_cfg.strategy),
        (unsigned)s_cfg.surface_thresh_pct,
        (unsigned)s_cfg.surface_dur_s,
        (unsigned)s_cfg.deep_thresh_pct,
        (unsigned)s_cfg.deep_dur_s,
        WATER_CHECK_START_HOUR, WATER_CHECK_END_HOUR, WATER_COOLDOWN_S);
}

/* ── Decision ────────────────────────────────────────────────────────────── */
static uint32_t noon_duration_ms(float humidity)
{
    if      (humidity > 80.0f) return  360000;  /*  6 min */
    else if (humidity > 70.0f) return  480000;  /*  8 min */
    else if (humidity > 60.0f) return  600000;  /* 10 min */
    else if (humidity > 50.0f) return  900000;  /* 15 min */
    else                       return 1200000;  /* 20 min */
}

void water_strategy_reset_state(void)
{
    s_noon_done_today = false;
    s_last_auto_water = 0;
}

water_decision_t water_strategy_eval(time_t now, float humidity)
{
    water_decision_t d = { .water = false, .duration_ms = 0 };

    if ((int64_t)now < WATER_TIME_VALID_MIN)
        return d;   /* clock not synced yet */

    struct tm t = {0};
    localtime_r(&now, &t);

    /* Midnight tick clears the once-a-day flag for the noon strategy */
    if (t.tm_hour == 0 && t.tm_min == 0)
        s_noon_done_today = false;

    if (s_cfg.strategy == WATER_STRATEGY_NOON) {
        if (t.tm_hour != NOON_HOUR || t.tm_min != 0 || s_noon_done_today)
            return d;
        /* Any watering starts the shared cooldown, regardless of strategy, so
         * switching to SURFACE/DEEP right after noon cannot water again. */
        s_noon_done_today = true;
        s_last_auto_water = now;
        d.water           = true;
        d.duration_ms     = noon_duration_ms(humidity);
        return d;
    }

    if (s_cfg.strategy != WATER_STRATEGY_SURFACE &&
        s_cfg.strategy != WATER_STRATEGY_DEEP)
        return d;

    if (t.tm_hour < WATER_CHECK_START_HOUR || t.tm_hour >= WATER_CHECK_END_HOUR)
        return d;
    if (now - s_last_auto_water < WATER_COOLDOWN_S)
        return d;

    bool     surface = (s_cfg.strategy == WATER_STRATEGY_SURFACE);
    float    thresh  = (float)(surface ? s_cfg.surface_thresh_pct : s_cfg.deep_thresh_pct);
    uint32_t dur_s   = surface ? s_cfg.surface_dur_s : s_cfg.deep_dur_s;

    if (humidity >= thresh)
        return d;

    s_last_auto_water = now;
    d.water           = true;
    d.duration_ms     = dur_s * 1000u;
    return d;
}
