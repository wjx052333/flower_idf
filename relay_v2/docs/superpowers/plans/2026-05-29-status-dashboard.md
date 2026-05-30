# Status Dashboard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `/status` (HTML dashboard) and `/api/sensors` (JSON API) to the ESP32 HTTP server so a browser can display real-time time-series charts for humidity, temperature, and lux.

**Architecture:** The browser opens `/status`, receives a self-contained HTML page, then polls `/api/sensors` at a configurable interval. Each poll causes the HTTP handler to read all three sensors directly (blocking ~800ms). A FreeRTOS mutex serializes concurrent access between the HTTP handler and the existing 30s MQTT status timer.

**Tech Stack:** ESP-IDF esp_http_server, FreeRTOS SemaphoreHandle_t, Chart.js 4.4.1 via BootCDN, plain C string literal for HTML.

---

## File Map

| File | Change |
|---|---|
| `main/main.c` | Only file modified — add mutex, 2 handlers, 2 routes |

---

### Task 1: Add sensor mutex

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Add semphr include**

At line 27 (after the existing FreeRTOS includes), add one line:

```c
#include "freertos/semphr.h"
```

- [ ] **Step 2: Declare the global mutex**

At line 119 (after `static EventGroupHandle_t s_wifi_event_group;`), add:

```c
static SemaphoreHandle_t g_sensor_mutex = NULL;
```

- [ ] **Step 3: Initialize mutex in app_main**

In `app_main()`, after the `modbus_init();` call at line 727, add:

```c
    /* Sensor mutex — serializes HTTP handler vs status_timer reads */
    g_sensor_mutex = xSemaphoreCreateMutex();
    assert(g_sensor_mutex != NULL);
```

- [ ] **Step 4: Wrap sensor reads in publish_status_report**

In `publish_status_report()`, replace lines 426–436:

```c
    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    float humidity = raw_low * 100.0f / 4096;
    ESP_LOGI(TAG, "ADC humidity(GPIO0)=%d,%.1f%%", raw_low, humidity);

    float temp = ds18b20_read_temperature();
    if (temp > -273.0f)
        ESP_LOGI(TAG, "Temperature(GPIO1)=%.2f°C", temp);

    float lux = modbus_read_illuminance();
    if (lux >= 0)
        ESP_LOGI(TAG, "Illuminance=%.3f Lux", lux);
```

with:

```c
    xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(3000));
    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    float humidity = raw_low * 100.0f / 4096;
    ESP_LOGI(TAG, "ADC humidity(GPIO0)=%d,%.1f%%", raw_low, humidity);

    float temp = ds18b20_read_temperature();
    if (temp > -273.0f)
        ESP_LOGI(TAG, "Temperature(GPIO1)=%.2f°C", temp);

    float lux = modbus_read_illuminance();
    if (lux >= 0)
        ESP_LOGI(TAG, "Illuminance=%.3f Lux", lux);
    xSemaphoreGive(g_sensor_mutex);
```

- [ ] **Step 5: Verify build**

```bash
cd C:/temp/esp32/test/livekit_mqtt_cpp/client/esp32/flower_idf/relay_v2
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 6: Commit**

```bash
git add main/main.c
git commit -m "feat: add sensor mutex to serialize HTTP vs MQTT timer reads"
```

---

### Task 2: Add /api/sensors JSON handler

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Add handle_api_sensors function**

Insert before `http_server_start()` (before line 599), add this new function:

```c
static esp_err_t handle_api_sensors(httpd_req_t *req)
{
    xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(3000));

    int raw_low = adc_read_raw(ADC_PIN_LOW_HUMIDITY);
    float humidity = raw_low * 100.0f / 4096.0f;

    float temp = ds18b20_read_temperature();

    float lux = modbus_read_illuminance();

    xSemaphoreGive(g_sensor_mutex);

    char json[160];
    snprintf(json, sizeof(json),
        "{\"humidity\":%.1f,\"temperature\":%.2f,\"lux\":%.1f,"
        "\"relay_last_on_ts\":%lld,\"relay_last_on_dur_ms\":%u}",
        humidity,
        (double)(temp > -273.0f ? temp : -273.0f),
        (double)(lux >= 0.0f ? lux : -1.0f),
        (long long)s_last_on_utc_s,
        (unsigned)s_last_on_dur_ms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}
```

- [ ] **Step 2: Register route in http_server_start**

In `http_server_start()`, after the existing `uri_relay` registration (after line 618), add:

```c
    static const httpd_uri_t uri_api_sensors = {
        .uri     = "/api/sensors",
        .method  = HTTP_GET,
        .handler = handle_api_sensors,
    };
    httpd_register_uri_handler(server, &uri_api_sensors);
```

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 4: Quick smoke test**

Flash and open `http://<device-ip>/api/sensors` in a browser. Expected response (values will vary):

```json
{"humidity":45.2,"temperature":24.10,"lux":1234.5,"relay_last_on_ts":0,"relay_last_on_dur_ms":0}
```

- [ ] **Step 5: Commit**

```bash
git add main/main.c
git commit -m "feat: add GET /api/sensors JSON endpoint"
```

---

### Task 3: Add /status HTML dashboard

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Add HTML constant**

Insert before `handle_api_sensors()` (i.e., before the function added in Task 2), add:

```c
static const char STATUS_HTML[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"zh\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>传感器状态</title>\n"
    "<script src=\"https://cdn.bootcdn.net/ajax/libs/Chart.js/4.4.1/chart.umd.min.js\"></script>\n"
    "<style>\n"
    "body{font-family:sans-serif;margin:0;padding:16px;background:#111;color:#eee}\n"
    "h2{margin:0 0 8px;font-size:18px}\n"
    ".relay{margin-bottom:16px;font-size:13px;color:#aaa;padding:8px;background:#1e1e1e;border-radius:6px}\n"
    ".chart-wrap{margin-bottom:20px}\n"
    "canvas{background:#1a1a1a;border-radius:8px;width:100%!important}\n"
    "#status{font-size:11px;color:#555;margin-top:4px}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h2>传感器实时数据</h2>\n"
    "<div class=\"relay\" id=\"relay-info\">继电器：加载中...</div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-humi\" height=\"100\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-temp\" height=\"100\"></canvas></div>\n"
    "<div class=\"chart-wrap\"><canvas id=\"c-lux\"  height=\"100\"></canvas></div>\n"
    "<div id=\"status\">正在连接...</div>\n"
    "<script>\n"
    "const p=new URLSearchParams(location.search);\n"
    "const IV=parseInt(p.get('interval')||'2000',10);\n"
    "const MX=parseInt(p.get('points')||'120',10);\n"
    "const t0=Date.now();\n"
    "function fmt(ms){const s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;\n"
    "  return`${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(ss).padStart(2,'0')}`}\n"
    "function mk(id,lbl,unit,clr){\n"
    "  return new Chart(document.getElementById(id),{\n"
    "    type:'line',\n"
    "    data:{labels:[],datasets:[{label:`${lbl} (${unit})`,data:[],\n"
    "      borderColor:clr,backgroundColor:clr+'22',borderWidth:1.5,\n"
    "      pointRadius:0,tension:0.3,fill:true}]},\n"
    "    options:{animation:false,responsive:true,\n"
    "      scales:{\n"
    "        x:{ticks:{color:'#888',maxTicksLimit:8},grid:{color:'#333'}},\n"
    "        y:{ticks:{color:'#888'},grid:{color:'#333'}}},\n"
    "      plugins:{legend:{labels:{color:'#ccc'}}}}})}\n"
    "const C={h:mk('c-humi','湿度','%','#4fc3f7'),\n"
    "         t:mk('c-temp','温度','°C','#ef9a9a'),\n"
    "         l:mk('c-lux','照度','lx','#fff176')};\n"
    "function push(c,x,y){c.data.labels.push(x);c.data.datasets[0].data.push(y);\n"
    "  if(c.data.labels.length>MX){c.data.labels.shift();c.data.datasets[0].data.shift()}\n"
    "  c.update('none')}\n"
    "async function poll(){\n"
    "  const t=fmt(Date.now()-t0);\n"
    "  try{\n"
    "    const r=await fetch('/api/sensors');\n"
    "    if(!r.ok)throw new Error('HTTP '+r.status);\n"
    "    const d=await r.json();\n"
    "    push(C.h,t,parseFloat(d.humidity.toFixed(1)));\n"
    "    push(C.t,t,parseFloat(d.temperature.toFixed(2)));\n"
    "    push(C.l,t,parseFloat(d.lux.toFixed(1)));\n"
    "    const el=document.getElementById('relay-info');\n"
    "    if(d.relay_last_on_ts>0){\n"
    "      const dt=new Date(d.relay_last_on_ts*1000).toLocaleString();\n"
    "      const dur=(d.relay_last_on_dur_ms/1000).toFixed(1);\n"
    "      el.textContent=`继电器最后触发：${dt}（持续 ${dur} 秒）`;\n"
    "    }else{el.textContent='继电器：从未触发'}\n"
    "    document.getElementById('status').textContent=\n"
    "      `最后更新：${new Date().toLocaleTimeString()}  轮询间隔：${IV}ms  保留点数：${MX}`;\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent=`错误：${e.message}，重试中...`;\n"
    "  }\n"
    "}\n"
    "poll();\n"
    "setInterval(poll,IV);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";
```

- [ ] **Step 2: Add handle_status function**

Insert immediately after the `STATUS_HTML` constant:

```c
static esp_err_t handle_status(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, STATUS_HTML, sizeof(STATUS_HTML) - 1);
    return ESP_OK;
}
```

- [ ] **Step 3: Register route in http_server_start**

In `http_server_start()`, after the `uri_api_sensors` registration added in Task 2, add:

```c
    static const httpd_uri_t uri_status = {
        .uri     = "/status",
        .method  = HTTP_GET,
        .handler = handle_status,
    };
    httpd_register_uri_handler(server, &uri_status);
```

- [ ] **Step 4: Build**

```bash
idf.py build 2>&1 | tail -5
```

Expected: `Project build complete.`

- [ ] **Step 5: Flash and verify**

```bash
idf.py flash monitor
```

Open `http://<device-ip>/status` in a browser. Verify:
- Page loads with "传感器实时数据" heading
- 3 charts appear (湿度、温度、照度)
- Charts start updating every 2 seconds (default interval)
- 继电器信息显示在顶部
- `http://<device-ip>/status?interval=500&points=60` works (faster poll, shorter window)

- [ ] **Step 6: Commit**

```bash
git add main/main.c
git commit -m "feat: add GET /status sensor dashboard with Chart.js time-series charts"
```
