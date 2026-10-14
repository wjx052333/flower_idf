# 可配置浇水策略 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `relay_v2` 里硬编码的自动浇水逻辑改成三种可在 web 上切换、参数可改、断电保留的策略。

**Architecture:** 纯逻辑抽到 `water_logic.c`（无 ESP-IDF 依赖，可在 PC 上编译测试），NVS 持久化单独放 `water_store.c`，`main.c` 只保留 GPIO 动作、HTTP 路由注册和页面。`water_strategy_eval()` 是纯决策函数，返回「浇不浇 / 浇多久」，硬件动作由 `main.c` 执行。

**Tech Stack:** ESP-IDF v6.0.1（目标 esp32）、FreeRTOS、esp_http_server、NVS、原生 JS + Chart.js 单页。

**Spec:** `relay_v2/docs/superpowers/specs/2026-10-14-watering-strategy-config-design.md`

---

## 执行前须知

**本机限制：**

1. **没有主机 C 编译器** —— Task 1 创建的 `test_water.c` 本轮无法运行。用户已确认「先写代码，测试后续补」。所有 `run_tests.sh` 相关步骤标记为 **DEFERRED**，执行者跳过即可，不要试图安装编译器。
2. **IDF 无法在本会话的 bash 里运行** —— `C:\esp\v6.0.1\esp-idf\tools\idf.py` 会拒绝 MSys/Mingw 环境。**编译验证由用户在自己的 IDF 终端里执行**，见 Task 6。执行者不要尝试 `source export.sh`。

**因此本计划的验证方式：** 每个 Task 结束后通读 `git diff` 自查；Task 6 交付给用户编译。

**参考现状：** `relay_v2/main/main.c` 1185 行，当前自动浇水逻辑在 `check_auto_water()`（第 644-680 行）与 `auto_water_duration_ms()`（第 635-642 行）。

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `relay_v2/main/water_logic.h`（新建） | 配置结构体、策略枚举、纯函数接口 |
| `relay_v2/main/water_logic.c`（新建） | 默认值、query 解析校验、JSON 序列化、策略决策 |
| `relay_v2/main/water_store.h`（新建） | NVS 持久化接口 |
| `relay_v2/main/water_store.c`（新建） | NVS 读写实现 |
| `relay_v2/test/test_water.c`（新建） | PC 端断言测试（本轮不运行） |
| `relay_v2/test/run_tests.sh`（新建） | 测试构建运行脚本 |
| `relay_v2/main/main.c`（修改） | 接线：NVS 载入、决策调用、HTTP 路由、页面 |
| `relay_v2/main/CMakeLists.txt`（修改） | 把新源文件加进 SRCS |
| `relay_v2/.gitignore`（修改） | 忽略测试二进制 |

---

## Task 1: 纯逻辑模块 water_logic

**Files:**
- Create: `relay_v2/main/water_logic.h`
- Create: `relay_v2/main/water_logic.c`
- Create: `relay_v2/test/test_water.c`
- Create: `relay_v2/test/run_tests.sh`
- Modify: `relay_v2/.gitignore`

- [ ] **Step 1: 创建 `relay_v2/main/water_logic.h`**

```c
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
```

- [ ] **Step 2: 创建 `relay_v2/main/water_logic.c`**

```c
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

    struct tm t;
    localtime_r(&now, &t);

    /* Midnight tick clears the once-a-day flag for the noon strategy */
    if (t.tm_hour == 0 && t.tm_min == 0)
        s_noon_done_today = false;

    if (s_cfg.strategy == WATER_STRATEGY_NOON) {
        if (t.tm_hour != NOON_HOUR || t.tm_min != 0 || s_noon_done_today)
            return d;
        s_noon_done_today = true;
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
```

- [ ] **Step 3: 创建 `relay_v2/test/test_water.c`**

```c
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
    CHECK(!water_strategy_eval(lt(2026, 6, 29, 0, 0), 10.0f).water, "watered at 00:00");
    CHECK(water_strategy_eval(lt(2026, 6, 29, 12, 0), 10.0f).water,
          "did not water the next day");
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
}

static void test_strategy_names(void)
{
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_NOON), "中午12点") == 0, "noon name");
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_SURFACE), "浇表面") == 0, "surface name");
    CHECK(strcmp(water_strategy_name(WATER_STRATEGY_DEEP), "干了再浇透") == 0, "deep name");
}

int main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    test_defaults_and_json();
    test_query_apply();
    test_noon_strategy();
    test_window_boundaries();
    test_threshold_and_duration();
    test_cooldown();
    test_strategy_names();

    if (s_failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", s_failures);
    return 1;
}
```

- [ ] **Step 4: 创建 `relay_v2/test/run_tests.sh`**

```sh
#!/bin/sh
# Host-side tests for water_logic.c.
# Needs a C compiler with POSIX setenv()/localtime_r(): MinGW-w64, gcc or clang.
set -e
cd "$(dirname "$0")"

OUT=test_water
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OUT=test_water.exe ;;
esac

CC=${CC:-gcc}
echo "compiling with $CC"
"$CC" -std=gnu99 -Wall -Wextra -O1 -I../main -o "$OUT" test_water.c ../main/water_logic.c

echo "running tests"
"./$OUT"
```

- [ ] **Step 5: 把测试二进制加进 `relay_v2/.gitignore`**

在文件末尾追加：

```
# host-side test binary
test/test_water
test/test_water.exe
```

- [ ] **Step 6: DEFERRED — 运行测试**

```bash
sh relay_v2/test/run_tests.sh
```

预期输出：`all tests passed`

**本机没有 C 编译器，跳过此步。** 用户会在有编译器的环境里执行。

- [ ] **Step 7: 提交**

```bash
git add relay_v2/main/water_logic.h relay_v2/main/water_logic.c \
        relay_v2/test/test_water.c relay_v2/test/run_tests.sh relay_v2/.gitignore
git commit -m "Add watering strategy config and decision logic"
```

---

## Task 2: NVS 持久化

**Files:**
- Create: `relay_v2/main/water_store.h`
- Create: `relay_v2/main/water_store.c`

- [ ] **Step 1: 创建 `relay_v2/main/water_store.h`**

```c
/**
 * water_store.h — Persist the watering configuration in NVS.
 */
#pragma once

#include <stdbool.h>

/* Load the saved configuration into water_config(). Fields that are missing or
 * out of range keep their defaults. Never fails: logs and returns on error. */
void water_store_load(void);

/* Write the whole configuration. Returns false if NVS rejected it; the
 * in-memory configuration stays in effect either way. */
bool water_store_save(void);
```

- [ ] **Step 2: 创建 `relay_v2/main/water_store.c`**

```c
/**
 * water_store.c — Persist the watering configuration in NVS.
 *
 * Uses the default NVS partition, which app_main() already initialised.
 */
#include "water_store.h"

#include "esp_log.h"
#include "nvs.h"

#include "water_logic.h"

#define TAG           "WaterStore"
#define NVS_NAMESPACE "water"

void water_store_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved water config (%s), keeping defaults",
                 esp_err_to_name(err));
        return;
    }

    water_config_t *cfg = water_config();
    uint8_t  u8;
    uint16_t u16;

    if (nvs_get_u8(h, "strategy", &u8) == ESP_OK && u8 < WATER_STRATEGY_COUNT)
        cfg->strategy = u8;
    if (nvs_get_u16(h, "s_thresh", &u16) == ESP_OK && u16 <= 100)
        cfg->surface_thresh_pct = u16;
    if (nvs_get_u16(h, "s_dur", &u16) == ESP_OK && u16 >= 1)
        cfg->surface_dur_s = u16;
    if (nvs_get_u16(h, "d_thresh", &u16) == ESP_OK && u16 <= 100)
        cfg->deep_thresh_pct = u16;
    if (nvs_get_u16(h, "d_dur", &u16) == ESP_OK && u16 >= 1)
        cfg->deep_dur_s = u16;

    nvs_close(h);

    ESP_LOGI(TAG, "Water config loaded: strategy=%u surface=%u%%/%us deep=%u%%/%us",
             (unsigned)cfg->strategy,
             (unsigned)cfg->surface_thresh_pct, (unsigned)cfg->surface_dur_s,
             (unsigned)cfg->deep_thresh_pct, (unsigned)cfg->deep_dur_s);
}

bool water_store_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return false;
    }

    const water_config_t *cfg = water_config();
    err = nvs_set_u8(h, "strategy", cfg->strategy);
    if (err == ESP_OK) err = nvs_set_u16(h, "s_thresh", cfg->surface_thresh_pct);
    if (err == ESP_OK) err = nvs_set_u16(h, "s_dur",    cfg->surface_dur_s);
    if (err == ESP_OK) err = nvs_set_u16(h, "d_thresh", cfg->deep_thresh_pct);
    if (err == ESP_OK) err = nvs_set_u16(h, "d_dur",    cfg->deep_dur_s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Water config saved");
    return true;
}
```

- [ ] **Step 3: 通读 diff 自查**

```bash
git diff --stat
```

确认两个文件都是新增，没有动到别的地方。

- [ ] **Step 4: 提交**

```bash
git add relay_v2/main/water_store.h relay_v2/main/water_store.c
git commit -m "Add NVS persistence for watering config"
```

---

## Task 3: main.c 接线 — 载入配置 + 替换决策逻辑

**Files:**
- Modify: `relay_v2/main/main.c`

- [ ] **Step 1: 加头文件**

在 `#include "ds18b20.h"` 之后插入：

```c
#include "water_logic.h"
#include "water_store.h"
```

- [ ] **Step 2: 删掉搬进 water_logic 的宏**

把这段：

```c
/* Auto watering at noon */
#define AUTO_WATER_HOUR  12  /* noon */
```

整个删除（`AUTO_WATER_HOUR` 现在只在 `water_logic.c` 里以 `NOON_HOUR` 存在）。

- [ ] **Step 3: 删掉搬进 water_logic 的状态变量**

把这段：

```c
static volatile bool            s_last_btn       = true; /* HIGH = not pressed */
static volatile bool            s_auto_water_triggered_today = false;
```

改成：

```c
static volatile bool            s_last_btn       = true; /* HIGH = not pressed */
```

- [ ] **Step 4: 替换整个自动浇水段**

把下面这一整段（当前 `main.c` 第 634-680 行）替换为：

```c
/* ── Auto watering ───────────────────────────────────────────────────────── */
static uint32_t auto_water_duration_ms(float humidity)
{
    if      (humidity > 80.0f) return  360000;  /*  6 min → 300 ml */
    else if (humidity > 70.0f) return  480000;  /*  8 min → 400 ml */
    else if (humidity > 60.0f) return  600000;  /* 10 min → 500 ml */
    else if (humidity > 50.0f) return  900000;  /* 15 min → 750 ml */
    else                       return 1200000;  /* 20 min → 1000 ml */
}

static void check_auto_water(void)
{
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    /* Reset flag at midnight */
    if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
        s_auto_water_triggered_today = false;
    }

    /* Check at noon (12:00) */
    if (timeinfo.tm_hour == AUTO_WATER_HOUR && timeinfo.tm_min == 0) {
        if (s_auto_water_triggered_today) return;

        xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(3000));
        float humidity = get_humidity();
        xSemaphoreGive(g_sensor_mutex);

        s_auto_water_triggered_today = true;

        if (!s_relay_on) {
            uint32_t dur_ms = auto_water_duration_ms(humidity);
            ESP_LOGI(TAG, "Auto watering: humidity=%.1f%%, opening relay for %u ms",
                     humidity, (unsigned)dur_ms);
            gpio_set_level(RELAY_PIN, 1);
            s_relay_on_us    = esp_timer_get_time();
            s_relay_on       = true;
            s_relay_duration = dur_ms;
            s_last_on_utc_s  = (int64_t)now;
            s_last_on_dur_ms = dur_ms;
        } else {
            ESP_LOGW(TAG, "Auto watering: relay already on, skipping");
        }
    }
}
```

替换成：

```c
/* ── Auto watering ───────────────────────────────────────────────────────── */
/* The strategy, thresholds and durations live in water_logic.c; this function
 * only reads the sensor, asks for a decision and drives the relay. */
static void check_auto_water(void)
{
    time_t now;
    time(&now);

    xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(3000));
    float humidity = get_humidity();
    xSemaphoreGive(g_sensor_mutex);

    water_decision_t d = water_strategy_eval(now, humidity);
    if (!d.water)
        return;

    if (s_relay_on) {
        ESP_LOGW(TAG, "Auto watering [%s]: relay already on, skipping %u ms",
                 water_strategy_name(water_config()->strategy),
                 (unsigned)d.duration_ms);
        return;
    }

    ESP_LOGI(TAG, "Auto watering [%s]: humidity=%.1f%%, opening relay for %u ms",
             water_strategy_name(water_config()->strategy),
             humidity, (unsigned)d.duration_ms);
    gpio_set_level(RELAY_PIN, 1);
    s_relay_on_us    = esp_timer_get_time();
    s_relay_on       = true;
    s_relay_duration = d.duration_ms;
    s_last_on_utc_s  = (int64_t)now;
    s_last_on_dur_ms = d.duration_ms;
}
```

- [ ] **Step 5: 在 app_main 里载入配置**

把：

```c
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
```

改成：

```c
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Watering strategy + thresholds, restored from NVS (defaults if absent) */
    water_store_load();
```

- [ ] **Step 6: 通读 diff 自查**

```bash
git diff relay_v2/main/main.c
```

确认：`AUTO_WATER_HOUR`、`s_auto_water_triggered_today`、`auto_water_duration_ms` 都已无残留引用（`grep -n "AUTO_WATER\|auto_water_duration_ms\|s_auto_water_triggered_today" relay_v2/main/main.c` 应无输出）。

- [ ] **Step 7: 提交**

```bash
git add relay_v2/main/main.c
git commit -m "Wire main.c to the configurable watering strategies"
```

---

## Task 4: HTTP 接口

**Files:**
- Modify: `relay_v2/main/main.c`

- [ ] **Step 1: 在 `/api/sensors` 里加 strategy 字段**

把：

```c
    char json[160];
    snprintf(json, sizeof(json),
        "{\"humidity\":%.1f,\"temperature\":%.2f,\"lux\":%.1f,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%u}",
        humidity,
        (double)(temp > -273.0f ? temp : -273.0f),
        (double)(lux >= 0.0f ? lux : -1.0f),
        (long long)s_last_on_utc_s,
        (unsigned)s_last_on_dur_ms);
```

改成：

```c
    char json[192];
    snprintf(json, sizeof(json),
        "{\"humidity\":%.1f,\"temperature\":%.2f,\"lux\":%.1f,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%u,\"strategy\":%u}",
        humidity,
        (double)(temp > -273.0f ? temp : -273.0f),
        (double)(lux >= 0.0f ? lux : -1.0f),
        (long long)s_last_on_utc_s,
        (unsigned)s_last_on_dur_ms,
        (unsigned)water_config()->strategy);
```

- [ ] **Step 2: 加 `/api/water/config` handler**

在 `handle_change_relay1()` 之后插入：

```c
/* GET /api/water/config            → current configuration as JSON
 * GET /api/water/config?strategy=1 → apply the given fields, persist, return
 *                                    the saved configuration as JSON */
static esp_err_t handle_api_water_config(httpd_req_t *req)
{
    char query[192] = {};
    esp_err_t qerr = httpd_req_get_url_query_str(req, query, sizeof(query));

    if (qerr == ESP_ERR_HTTPD_RESULT_TRUNC) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "query too long");
        return ESP_OK;
    }

    if (qerr == ESP_OK && query[0] != '\0') {
        char err[64] = {};
        if (!water_config_apply_query(query, err, sizeof(err))) {
            ESP_LOGW(TAG, "Water config rejected: %s", err);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, err);
            return ESP_OK;
        }
        if (!water_store_save()) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            httpd_resp_sendstr(req, "save failed");
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Water config updated: %s", query);
    }

    char json[256];
    water_config_to_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}
```

- [ ] **Step 3: 注册路由**

把：

```c
    static const httpd_uri_t uri_api_sensors = {
        .uri     = "/api/sensors",
        .method  = HTTP_GET,
        .handler = handle_api_sensors,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_relay);
    httpd_register_uri_handler(server, &uri_api_sensors);
```

改成：

```c
    static const httpd_uri_t uri_api_sensors = {
        .uri     = "/api/sensors",
        .method  = HTTP_GET,
        .handler = handle_api_sensors,
    };
    static const httpd_uri_t uri_api_water = {
        .uri     = "/api/water/config",
        .method  = HTTP_GET,
        .handler = handle_api_water_config,
    };
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_relay);
    httpd_register_uri_handler(server, &uri_api_sensors);
    httpd_register_uri_handler(server, &uri_api_water);
```

- [ ] **Step 4: 通读 diff 自查**

```bash
git diff relay_v2/main/main.c
```

确认 JSON 缓冲区从 160 提到 192、`ESP_ERR_HTTPD_RESULT_TRUNC` 拼写正确、路由注册了。

- [ ] **Step 5: 提交**

```bash
git add relay_v2/main/main.c
git commit -m "Add /api/water/config endpoint and strategy field in /api/sensors"
```

---

## Task 5: Web 页面策略卡片

**Files:**
- Modify: `relay_v2/main/main.c`（`STATUS_HTML`）

- [ ] **Step 1: 加 CSS**

把：

```c
    ".relay-row label{font-size:13px;color:#aaa}\n"
    "</style>\n"
```

改成：

```c
    ".relay-row label{font-size:13px;color:#aaa}\n"
    ".strat{margin-bottom:16px;padding:10px;background:#1e1e1e;border-radius:6px}\n"
    ".strat h3{margin:0 0 8px;font-size:14px;color:#ccc}\n"
    ".strat .cur{font-size:13px;color:#81c784;margin-bottom:8px}\n"
    ".strat .opts{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px}\n"
    ".strat .opts label{padding:6px 10px;background:#2a2a2a;border:1px solid #444;border-radius:6px;cursor:pointer;color:#bbb;font-size:13px}\n"
    ".strat .opts label.sel{background:#2e7d32;border-color:#2e7d32;color:#fff}\n"
    ".strat .opts input{display:none}\n"
    ".strat .row{display:flex;align-items:center;gap:6px;margin-bottom:6px;flex-wrap:wrap;font-size:13px;color:#aaa}\n"
    ".strat .row .nm{width:84px}\n"
    ".strat .row input{width:72px;padding:6px;background:#111;color:#eee;border:1px solid #444;border-radius:6px;text-align:center;font-size:13px}\n"
    ".strat button{padding:8px 16px;background:#2e7d32;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:13px}\n"
    ".strat .msg{font-size:12px;color:#888}\n"
    ".strat .hint{font-size:11px;color:#666;margin-top:8px;line-height:1.6}\n"
    "</style>\n"
```

- [ ] **Step 2: 用策略卡片替换旧的说明文字**

把：

```c
    "<div class=\"relay\" style=\"font-size:12px;color:#888\">自动浇水：每天中午12点检测湿度，阶梯式浇水：&gt;80%→6min，70-80%→8min，60-70%→10min，50-60%→15min，&lt;50%→20min</div>\n"
```

替换为：

```c
    "<div class=\"strat\">\n"
    "<h3>自动浇水策略</h3>\n"
    "<div class=\"cur\" id=\"strat-cur\">当前：加载中...</div>\n"
    "<div class=\"opts\" id=\"strat-opts\">\n"
    "  <label data-s=\"0\"><input type=\"radio\" name=\"strat\" value=\"0\">中午12点</label>\n"
    "  <label data-s=\"1\"><input type=\"radio\" name=\"strat\" value=\"1\">浇表面</label>\n"
    "  <label data-s=\"2\"><input type=\"radio\" name=\"strat\" value=\"2\">干了再浇透</label>\n"
    "</div>\n"
    "<div class=\"row\"><span class=\"nm\">浇表面</span>湿度 &lt; <input type=\"number\" id=\"s-thresh\" min=\"0\" max=\"100\"> % → 浇 <input type=\"number\" id=\"s-dur\" min=\"1\" max=\"65535\"> 秒</div>\n"
    "<div class=\"row\"><span class=\"nm\">干了再浇透</span>湿度 &lt; <input type=\"number\" id=\"d-thresh\" min=\"0\" max=\"100\"> % → 浇 <input type=\"number\" id=\"d-dur\" min=\"1\" max=\"65535\"> 秒</div>\n"
    "<div class=\"row\"><button onclick=\"saveWater()\">保存</button><span class=\"msg\" id=\"strat-msg\"></span></div>\n"
    "<div class=\"hint\">「中午12点」：每天 12:00 检查一次，阶梯式浇水 &gt;80%→6min，70-80%→8min，60-70%→10min，50-60%→15min，&lt;50%→20min（参数固定）<br>「浇表面」「干了再浇透」：08:00-16:00 之间每 30 秒检查，触发后 30 分钟内不再自动浇水</div>\n"
    "</div>\n"
```

- [ ] **Step 3: 加前端逻辑**

把：

```c
    "poll();\n"
    "setInterval(poll,IV);\n"
```

改成：

```c
    "let WC=null;\n"
    "function stratDesc(c){\n"
    "  if(c.strategy===1)return `湿度 < ${c.surface_thresh_pct}% → 浇 ${c.surface_dur_s} 秒`;\n"
    "  if(c.strategy===2)return `湿度 < ${c.deep_thresh_pct}% → 浇 ${c.deep_dur_s} 秒`;\n"
    "  return '每天 12:00 阶梯浇水';\n"
    "}\n"
    "function markStrat(id){\n"
    "  document.querySelectorAll('#strat-opts label').forEach(l=>{\n"
    "    const s=parseInt(l.dataset.s,10);\n"
    "    l.classList.toggle('sel',s===id);\n"
    "    l.querySelector('input').checked=(s===id);\n"
    "  });\n"
    "}\n"
    "function fillWater(c){\n"
    "  WC=c;\n"
    "  markStrat(c.strategy);\n"
    "  document.getElementById('s-thresh').value=c.surface_thresh_pct;\n"
    "  document.getElementById('s-dur').value=c.surface_dur_s;\n"
    "  document.getElementById('d-thresh').value=c.deep_thresh_pct;\n"
    "  document.getElementById('d-dur').value=c.deep_dur_s;\n"
    "  document.getElementById('strat-cur').textContent=`当前：${c.strategy_name}（${stratDesc(c)}）`;\n"
    "}\n"
    "async function loadWater(){\n"
    "  try{const r=await fetch('/api/water/config');fillWater(await r.json());}\n"
    "  catch(e){document.getElementById('strat-msg').textContent='读取配置失败';}\n"
    "}\n"
    "async function saveWater(){\n"
    "  const sel=document.querySelector('#strat-opts input:checked');\n"
    "  const q=new URLSearchParams({\n"
    "    strategy:sel?sel.value:'0',\n"
    "    surface_thresh_pct:document.getElementById('s-thresh').value,\n"
    "    surface_dur_s:document.getElementById('s-dur').value,\n"
    "    deep_thresh_pct:document.getElementById('d-thresh').value,\n"
    "    deep_dur_s:document.getElementById('d-dur').value});\n"
    "  const msg=document.getElementById('strat-msg');\n"
    "  msg.textContent='保存中...';\n"
    "  try{\n"
    "    const r=await fetch('/api/water/config?'+q);\n"
    "    if(!r.ok){msg.textContent='保存失败：'+await r.text();return}\n"
    "    fillWater(await r.json());\n"
    "    msg.textContent='已保存';\n"
    "  }catch(e){msg.textContent='保存失败：'+e.message}\n"
    "}\n"
    "document.querySelectorAll('#strat-opts input').forEach(i=>\n"
    "  i.addEventListener('change',()=>markStrat(parseInt(i.value,10))));\n"
    "loadWater();\n"
    "poll();\n"
    "setInterval(poll,IV);\n"
```

- [ ] **Step 4: 让轮询同步设备侧的策略变化**

在 `poll()` 里，把：

```c
    "    }else{el.textContent='继电器：从未触发'}\n"
```

改成：

```c
    "    }else{el.textContent='继电器：从未触发'}\n"
    "    if(WC&&d.strategy!==WC.strategy){WC.strategy=d.strategy;fillWater(WC);}\n"
```

- [ ] **Step 5: 通读 diff 自查**

```bash
git diff relay_v2/main/main.c
```

逐条确认：

1. 新增的 C 字符串里所有 JS/HTML 双引号都写成 `\"`，没有漏转义（漏转义会直接编译失败）
2. 模板字符串 `${...}` 都写成 `` ` `` 包裹，且每行以 `\n"` 结束，没有被拆成多行
3. 每个新增行的结尾都有 `\n"`
4. 旧的那行「自动浇水：每天中午12点检测湿度…」已被整行删除，没有残留
5. `loadWater();` 出现在 `poll();` 之前

- [ ] **Step 6: 提交**

```bash
git add relay_v2/main/main.c
git commit -m "Add watering strategy card to the web dashboard"
```

---

## Task 6: 构建配置与用户验证

**Files:**
- Modify: `relay_v2/main/CMakeLists.txt`

- [ ] **Step 1: 把新源文件加进 SRCS**

把：

```
    SRCS
        "main.c"
        "ds18b20.c"
        "../../nanopb_gen/proto/flower.pb.c"
```

改成：

```
    SRCS
        "main.c"
        "ds18b20.c"
        "water_logic.c"
        "water_store.c"
        "../../nanopb_gen/proto/flower.pb.c"
```

- [ ] **Step 2: 提交**

```bash
git add relay_v2/main/CMakeLists.txt
git commit -m "Build water_logic.c and water_store.c into the firmware"
```

- [ ] **Step 3: 交给用户编译（执行者不能自己跑）**

请用户在自己的 ESP-IDF 终端里执行：

```
idf.py -C relay_v2 build
```

预期：`Project build complete.`

如果报错，把完整报错贴回来。

- [ ] **Step 4: 用户运行主机端测试（可选，需要 C 编译器）**

```bash
sh relay_v2/test/run_tests.sh
```

预期：`all tests passed`

- [ ] **Step 5: 设备上手工验证**

烧录后依次确认：

1. `http://<设备IP>/` 页面出现「自动浇水策略」卡片，「当前」显示 `中午12点（每天 12:00 阶梯浇水）`，三个策略按钮里「中午12点」高亮
2. 选「浇表面」，把阈值改成 70、时长改成 30，点保存 → 显示「已保存」
3. 串口日志出现 `Water config saved`
4. 断电重启 → 页面仍是「浇表面 / 70% / 30 秒」
5. 把阈值临时改成 99（高于当前湿度）→ 等 30 秒内串口出现 `Auto watering [浇表面]: humidity=..., opening relay for 30000 ms`，继电器吸合，30 秒后断开
6. 30 分钟内不再重复触发
7. 把系统时间调到 20:00 后再试 → 不再触发（窗口外）
8. `curl "http://<设备IP>/api/water/config?surface_thresh_pct=200"` → HTTP 400，且页面刷新后阈值仍是旧值
9. `curl "http://<设备IP>/api/water/config"` → 返回的 JSON 里 `strategy` 与页面显示的当前策略一致

---

## 完成标准

- 三个策略可在 web 上切换，页面显示当前策略（含参数摘要）
- 策略 2、3 的阈值与时长可在 web 上修改并立即生效
- 配置断电重启后保留
- 策略 1 的行为与参数与改动前一致
- `idf.py build` 通过（由用户确认）
