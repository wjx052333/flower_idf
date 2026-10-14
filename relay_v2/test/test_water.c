/**
 * test_water.c — host-side tests for water_logic.c
 *
 * Build & run: sh relay_v2/test/run_tests.sh
 * Needs a C compiler with POSIX setenv()/localtime_r() — MinGW-w64, gcc or
 * clang all work.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "water_logic.h"

static int s_failures = 0;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            s_failures++;                                    \
        }                                                    \
    } while (0)

/* time_t for a local (CST-8) wall-clock time. */
static time_t lt(int year, int mon, int day, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year  = year - 1900;
    t.tm_mon   = mon - 1;
    t.tm_mday  = day;
    t.tm_hour  = hour;
    t.tm_min   = min;
    t.tm_isdst = 0;
    return mktime(&t);
}

static void reset_all(uint8_t strategy)
{
    water_config_reset();
    water_config()->strategy = strategy;
    water_strategy_reset_state();
}

static void test_defaults_and_json(void)
{
    water_config_reset();
    water_config_t *c = water_config();
    CHECK(c->strategy == WATER_STRATEGY_NOON, "default strategy %u", c->strategy);
    CHECK(c->surface_thresh_pct == 50, "default surface thresh %u", c->surface_thresh_pct);
    CHECK(c->surface_dur_s == 60, "default surface dur %u", c->surface_dur_s);
    CHECK(c->deep_thresh_pct == 10, "default deep thresh %u", c->deep_thresh_pct);
    CHECK(c->deep_dur_s == 1200, "default deep dur %u", c->deep_dur_s);

    char json[256];
    int n = water_config_to_json(json, sizeof(json));
    CHECK(n > 0 && n < (int)sizeof(json), "json length %d", n);
    CHECK(strstr(json, "\"strategy\":0") != NULL, "json strategy: %s", json);
    CHECK(strstr(json, "\"strategy_name\":\"") != NULL, "json name: %s", json);
    CHECK(strstr(json, "\"surface_thresh_pct\":50") != NULL, "json s_thresh: %s", json);
    CHECK(strstr(json, "\"surface_dur_s\":60") != NULL, "json s_dur: %s", json);
    CHECK(strstr(json, "\"deep_thresh_pct\":10") != NULL, "json d_thresh: %s", json);
    CHECK(strstr(json, "\"deep_dur_s\":1200") != NULL, "json d_dur: %s", json);
    CHECK(strstr(json, "\"check_window\":\"08:00-16:00\"") != NULL, "json window: %s", json);
    CHECK(strstr(json, "\"cooldown_s\":1800") != NULL, "json cooldown: %s", json);
}

static void test_query_apply(void)
{
    char err[64];

    water_config_reset();
    CHECK(water_config_apply_query("strategy=2&surface_thresh_pct=45&surface_dur_s=90"
                                   "&deep_thresh_pct=5&deep_dur_s=600", err, sizeof(err)),
          "valid query rejected: %s", err);
    water_config_t *c = water_config();
    CHECK(c->strategy == WATER_STRATEGY_DEEP, "strategy %u", c->strategy);
    CHECK(c->surface_thresh_pct == 45, "s_thresh %u", c->surface_thresh_pct);
    CHECK(c->surface_dur_s == 90, "s_dur %u", c->surface_dur_s);
    CHECK(c->deep_thresh_pct == 5, "d_thresh %u", c->deep_thresh_pct);
    CHECK(c->deep_dur_s == 600, "d_dur %u", c->deep_dur_s);

    /* A partial update leaves the other fields alone */
    CHECK(water_config_apply_query("surface_thresh_pct=70", err, sizeof(err)),
          "partial query rejected: %s", err);
    CHECK(c->surface_thresh_pct == 70 && c->surface_dur_s == 90,
          "partial update s_thresh=%u s_dur=%u", c->surface_thresh_pct, c->surface_dur_s);

    /* An empty query is a no-op */
    CHECK(water_config_apply_query("", err, sizeof(err)), "empty query rejected");

    /* Out-of-range and malformed values are rejected */
    CHECK(!water_config_apply_query("strategy=3", err, sizeof(err)), "strategy=3 accepted");
    CHECK(!water_config_apply_query("surface_thresh_pct=101", err, sizeof(err)), "101%% accepted");
    CHECK(!water_config_apply_query("deep_dur_s=0", err, sizeof(err)), "0 s accepted");
    CHECK(!water_config_apply_query("nonsense=1", err, sizeof(err)), "unknown key accepted");
    CHECK(!water_config_apply_query("surface_dur_s=abc", err, sizeof(err)), "non-number accepted");

    /* A bad field anywhere in the query rolls the whole query back */
    CHECK(!water_config_apply_query("surface_thresh_pct=80&strategy=9", err, sizeof(err)),
          "bad trailing field accepted");
    CHECK(c->strategy == WATER_STRATEGY_DEEP && c->surface_thresh_pct == 70,
          "rollback failed: strategy=%u s_thresh=%u", c->strategy, c->surface_thresh_pct);

    /* A query longer than the internal buffer is rejected */
    char big[300];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    CHECK(!water_config_apply_query(big, err, sizeof(err)), "oversized query accepted");

    /* A pair without '=' is rejected */
    CHECK(!water_config_apply_query("strategy", err, sizeof(err)), "pair without = accepted");

    /* Empty segments between '&' are skipped */
    CHECK(water_config_apply_query("surface_thresh_pct=60&&deep_dur_s=300", err, sizeof(err)),
          "empty segment rejected: %s", err);
}

static void test_noon_strategy(void)
{
    reset_all(WATER_STRATEGY_NOON);

    /* Clock not synced yet */
    CHECK(!water_strategy_eval(0, 10.0f).water, "watered with an un-synced clock");

    /* Either side of noon does nothing */
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 11, 59), 10.0f).water, "watered at 11:59");
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 12, 1), 10.0f).water, "watered at 12:01");

    /* Noon waters once, and the ladder picks the duration */
    water_decision_t d = water_strategy_eval(lt(2026, 6, 28, 12, 0), 90.0f);
    CHECK(d.water, "did not water at 12:00");
    CHECK(d.duration_ms == 360000, "humidity 90%% gave %u ms", d.duration_ms);
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 12, 0), 90.0f).water,
          "watered twice on the same day");

    /* Ladder boundaries */
    reset_all(WATER_STRATEGY_NOON);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 12, 0), 80.0f).duration_ms == 480000,
          "humidity 80%% ladder");
    reset_all(WATER_STRATEGY_NOON);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 12, 0), 50.0f).duration_ms == 1200000,
          "humidity 50%% ladder");

    /* The next day's midnight tick re-arms it */
    water_strategy_tick(lt(2026, 6, 29, 0, 0));
    CHECK(!water_strategy_eval(lt(2026, 6, 29, 0, 0), 10.0f).water, "watered at 00:00");
    CHECK(water_strategy_eval(lt(2026, 6, 29, 12, 0), 10.0f).water,
          "did not water the next day");
}

static void test_midnight_rollover(void)
{
    /* A bare rollover re-arms the noon strategy, with no eval() involved.
     * This is the regression guard: the clear used to live inside eval(), so a
     * tick that bailed out early (mutex timeout, unreadable sensor) skipped it
     * and lost the whole day's watering. */
    reset_all(WATER_STRATEGY_NOON);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 12, 0), 10.0f).water, "noon did not water");
    water_strategy_tick(lt(2026, 6, 29, 0, 0));
    CHECK(water_strategy_eval(lt(2026, 6, 29, 12, 0), 10.0f).water,
          "a bare midnight rollover did not re-arm the noon strategy");

    /* eval() alone must NOT re-arm — the rollover is the tick's job */
    reset_all(WATER_STRATEGY_NOON);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 12, 0), 10.0f).water, "noon did not water");
    CHECK(!water_strategy_eval(lt(2026, 6, 29, 0, 0), 10.0f).water, "watered at 00:00");
    CHECK(!water_strategy_eval(lt(2026, 6, 29, 12, 0), 10.0f).water,
          "eval() re-armed the noon flag on its own");

    /* No case for "a non-midnight tick leaves the flag alone": it is not
     * observable. A NOON eval outside the 12:00 minute returns no-water from
     * the hour check before it ever reads the flag, and a clear at any other
     * time of day cannot change an outcome, because the next eval that does
     * read the flag is the following noon — already past a real midnight. */
}

static void test_clock_guard(void)
{
    /* time() == 0 is 1970-01-01 08:00 in CST-8 — inside the watering window,
     * so the window does not suppress this. (Like every fresh state, the
     * cooldown would suppress it too, so this case alone does not prove the
     * guard is what did it — the case below does.) */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval(0, 10.0f).water, "watered with an un-synced clock");

    /* 1000000000 is 2001-09-09 09:46:40 in CST-8 — inside the window (hour 9),
     * still below WATER_TIME_VALID_MIN, and far enough past the zero-initialised
     * cooldown that the cooldown does NOT suppress it. Only the guard does. */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval((time_t)1000000000, 10.0f).water,
          "watered before the clock was synced");

    /* Past the guard, the same call does water — confirming the guard, and not
     * some other condition, suppressed the call above.
     * 1700013600 is 2023-11-15 10:00:00 CST-8 (hour 10), inside 08:00-16:00,
     * and >= WATER_TIME_VALID_MIN (1700000000). */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval((time_t)1700013600, 10.0f).water,
          "did not water once the clock was valid");
}

static void test_window_boundaries(void)
{
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 7, 59), 10.0f).water, "watered at 07:59");

    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 8, 0), 10.0f).water, "did not water at 08:00");

    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 15, 59), 10.0f).water, "did not water at 15:59");

    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 16, 0), 10.0f).water, "watered at 16:00");
}

static void test_threshold_and_duration(void)
{
    reset_all(WATER_STRATEGY_SURFACE);
    water_decision_t d = water_strategy_eval(lt(2026, 6, 28, 10, 0), 49.9f);
    CHECK(d.water && d.duration_ms == 60000, "surface water=%d dur=%u", d.water, d.duration_ms);

    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 10, 0), 50.0f).water, "watered at exactly 50%%");

    reset_all(WATER_STRATEGY_DEEP);
    d = water_strategy_eval(lt(2026, 6, 28, 10, 0), 9.9f);
    CHECK(d.water && d.duration_ms == 1200000, "deep water=%d dur=%u", d.water, d.duration_ms);

    reset_all(WATER_STRATEGY_DEEP);
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 10, 0), 10.0f).water, "watered at exactly 10%%");

    /* Edited values are honoured */
    reset_all(WATER_STRATEGY_SURFACE);
    water_config()->surface_thresh_pct = 70;
    water_config()->surface_dur_s      = 30;
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 10, 0), 70.0f).water,
          "watered at the custom threshold");
    d = water_strategy_eval(lt(2026, 6, 28, 10, 0), 69.0f);
    CHECK(d.water && d.duration_ms == 30000, "custom duration water=%d dur=%u",
          d.water, d.duration_ms);
}

static void test_cooldown(void)
{
    time_t t0 = lt(2026, 6, 28, 10, 0);

    /* The decision consumes the cooldown immediately */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(t0, 10.0f).water, "first trigger did not water");
    CHECK(!water_strategy_eval(t0 + 60, 10.0f).water, "watered one minute later");

    /* 29 minutes is still inside the cooldown */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(t0, 10.0f).water, "first trigger did not water");
    CHECK(!water_strategy_eval(t0 + 29 * 60, 10.0f).water, "watered after 29 min");

    /* 30 minutes is allowed again */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(t0, 10.0f).water, "first trigger did not water");
    CHECK(water_strategy_eval(t0 + 30 * 60, 10.0f).water, "did not water after 30 min");

    /* The cooldown is shared across strategies */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(water_strategy_eval(t0, 10.0f).water, "first trigger did not water");
    water_config()->strategy = WATER_STRATEGY_DEEP;
    CHECK(!water_strategy_eval(t0 + 60, 5.0f).water, "cooldown not shared across strategies");

    /* A decision not to water does not start the cooldown */
    reset_all(WATER_STRATEGY_SURFACE);
    CHECK(!water_strategy_eval(t0, 90.0f).water, "watered at 90%% humidity");
    CHECK(water_strategy_eval(t0 + 60, 10.0f).water,
          "cooldown started even though nothing was watered");

    /* Noon also starts the shared cooldown, so switching strategy right after
     * noon does not water again immediately */
    reset_all(WATER_STRATEGY_NOON);
    CHECK(water_strategy_eval(lt(2026, 6, 28, 12, 0), 10.0f).water, "noon did not water");
    water_config()->strategy = WATER_STRATEGY_SURFACE;
    CHECK(!water_strategy_eval(lt(2026, 6, 28, 12, 5), 10.0f).water,
          "watered again five minutes after noon");
}

static void test_strategy_names(void)
{
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_NOON), "中午12点") == 0, "noon name");
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_SURFACE), "浇表面") == 0, "surface name");
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_DEEP), "干了再浇透") == 0, "deep name");
}

static void test_humidity_validity(void)
{
    CHECK(water_humidity_valid(0.0f), "0%% rejected");
    CHECK(water_humidity_valid(50.0f), "50%% rejected");
    CHECK(water_humidity_valid(100.0f), "100%% rejected");
    CHECK(!water_humidity_valid(-0.1f), "negative humidity accepted");
    CHECK(!water_humidity_valid(100.1f), "humidity above 100%% accepted");
    CHECK(!water_humidity_valid(-2.9f), "stuck-high sensor value accepted");
    CHECK(!water_humidity_valid(144.1f), "stuck-low sensor value accepted");
}

int main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    test_defaults_and_json();
    test_query_apply();
    test_noon_strategy();
    test_midnight_rollover();
    test_clock_guard();
    test_window_boundaries();
    test_threshold_and_duration();
    test_cooldown();
    test_strategy_names();
    test_humidity_validity();

    if (s_failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", s_failures);
    return 1;
}
