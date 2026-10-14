/**
 * water_logic.h — Watering strategy configuration and decision logic
 *
 * Pure logic only: no ESP-IDF dependencies, so it can be compiled and run on
 * a host PC (see relay_v2/test/test_water.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Humidity-triggered strategies only water inside this local-time window and
 * never more often than WATER_COOLDOWN_S. */
#define WATER_CHECK_START_HOUR 8
#define WATER_CHECK_END_HOUR   16
#define WATER_COOLDOWN_S       1800

/* Local time below which the clock is considered un-synced (2023-11-14).
 * Before NTP sync time() returns ~1970, which in CST-8 is 08:00 — inside the
 * watering window. */
#define WATER_TIME_VALID_MIN   1700000000LL

/* Accepted ranges for the configurable fields. Enforced both when parsing an
 * HTTP query and when loading from NVS. */
#define WATER_PCT_MIN  0
#define WATER_PCT_MAX  100
#define WATER_DUR_MIN  1
#define WATER_DUR_MAX  65535

typedef enum {
    WATER_STRATEGY_NOON    = 0,  /* 12:00 ladder watering, fixed parameters */
    WATER_STRATEGY_SURFACE = 1,  /* humidity < thresh → short fixed duration */
    WATER_STRATEGY_DEEP    = 2,  /* humidity < thresh → long fixed duration */
    WATER_STRATEGY_COUNT
} water_strategy_t;

typedef struct {
    uint8_t  strategy;            /* water_strategy_t */
    uint16_t surface_thresh_pct;  /* WATER_STRATEGY_SURFACE threshold, 0-100 */
    uint16_t surface_dur_s;       /* WATER_STRATEGY_SURFACE duration, 1-65535 */
    uint16_t deep_thresh_pct;     /* WATER_STRATEGY_DEEP threshold, 0-100 */
    uint16_t deep_dur_s;          /* WATER_STRATEGY_DEEP duration, 1-65535 */
} water_config_t;

typedef struct {
    bool     water;               /* true → open the relay for duration_ms */
    uint32_t duration_ms;
} water_decision_t;

/* Current configuration, always non-NULL. */
water_config_t *water_config(void);

/* Restore compile-time defaults. */
void water_config_reset(void);

/* Apply "key=value&key=value" pairs to the live configuration. An unknown key
 * or an out-of-range value leaves the whole configuration unchanged and writes
 * a message into err (which must have room for errlen bytes). */
bool water_config_apply_query(const char *query, char *err, size_t errlen);

/* Serialize the configuration as JSON. Returns the length that would have been
 * written, snprintf-style. */
int water_config_to_json(char *buf, size_t n);

/* Human-readable strategy name (UTF-8 Chinese). */
const char *water_strategy_name(uint8_t strategy);

/* Forget the once-a-day flag and the cooldown (used by tests). */
void water_strategy_reset_state(void);

/* Decide whether to water now. Pure: never touches hardware. When it returns
 * water=true it also consumes the cooldown / once-a-day flag, so a skipped
 * activation does not immediately re-trigger. */
water_decision_t water_strategy_eval(time_t now, float humidity);

/* Time-based housekeeping that must happen on every tick, before anything that
 * can fail. Clears the noon strategy's once-a-day flag at midnight. Kept
 * separate from water_strategy_eval() so a skipped or unreadable sensor tick
 * cannot leave the flag set and silently skip the next day's watering. */
void water_strategy_tick(time_t now);

/* A humidity reading outside the physically meaningful range means the sensor
 * is disconnected, shorted or otherwise faulty. Such a reading must not drive
 * a watering decision: a floating sensor reads permanently "dry" and would
 * otherwise water on every cooldown, indefinitely. */
bool water_humidity_valid(float humidity);
