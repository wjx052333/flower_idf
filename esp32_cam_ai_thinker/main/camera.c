/**
 * camera.c — OV2640 camera via esp_camera (AI Thinker ESP32-CAM)
 */

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera.h"

/* AI Thinker ESP32-CAM pin definitions */
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22
#define CAM_FLASH_LED    4

#define LED_OFF_DELAY_US  (1000 * 1000)
#define TAG "Camera"

static camera_fb_t       *s_fb        = NULL;
static SemaphoreHandle_t  s_cam_sem;
static esp_timer_handle_t s_led_timer = NULL;
static bool               s_cam_ok    = false;

static void led_off_timer_cb(void *arg)
{
    gpio_set_level(CAM_FLASH_LED, 0);
}

esp_err_t camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn     = CAM_PIN_PWDN,
        .pin_reset    = CAM_PIN_RESET,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_1,
        .ledc_channel = LEDC_CHANNEL_1,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location  = CAMERA_FB_IN_PSRAM,
    };

    esp_err_t ret = esp_camera_init(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_cam_ok = true;
    ESP_LOGI(TAG, "Camera init OK (OV2640, VGA JPEG)");

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << CAM_FLASH_LED),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(CAM_FLASH_LED, 0);

    esp_timer_create_args_t targs = {
        .callback              = led_off_timer_cb,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "led_off",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&targs, &s_led_timer);

    s_cam_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(s_cam_sem);
    return ESP_OK;
}

esp_err_t camera_init_with_i2c(i2c_master_bus_handle_t i2c_handle)
{
    /* esp_camera manages I2C internally; external handle not used */
    (void)i2c_handle;
    return camera_init();
}

bool camera_is_ready(void)
{
    return s_cam_ok;
}

esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size)
{
    if (!s_cam_ok) {
        ESP_LOGE(TAG, "camera_capture_jpeg: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cam_sem, portMAX_DELAY);

    gpio_set_level(CAM_FLASH_LED, 1);

    s_fb = esp_camera_fb_get();
    if (!s_fb) {
        gpio_set_level(CAM_FLASH_LED, 0);
        xSemaphoreGive(s_cam_sem);
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        return ESP_FAIL;
    }

    esp_timer_stop(s_led_timer);
    esp_timer_start_once(s_led_timer, LED_OFF_DELAY_US);

    *data = s_fb->buf;
    *size = s_fb->len;
    return ESP_OK;
}

void camera_release_jpeg(void)
{
    if (s_fb) {
        esp_camera_fb_return(s_fb);
        s_fb = NULL;
    }
    xSemaphoreGive(s_cam_sem);
}

static sensor_t *get_sensor(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
    return s;
}

esp_err_t camera_set_hmirror(int enable)
{
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_hmirror(s, enable ? 1 : 0);
    return ESP_OK;
}

esp_err_t camera_set_vflip(int enable)
{
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_vflip(s, enable ? 1 : 0);
    return ESP_OK;
}

esp_err_t camera_set_contrast(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_contrast(s, level);
    return ESP_OK;
}

esp_err_t camera_set_saturation(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    s->set_saturation(s, level);
    return ESP_OK;
}

esp_err_t camera_set_wb_mode(int mode)
{
    if (mode < 0 || mode > 4) return ESP_ERR_INVALID_ARG;
    sensor_t *s = get_sensor();
    if (!s) return ESP_ERR_INVALID_STATE;
    if (mode == 0) {
        s->set_whitebal(s, 1);      /* AWB on */
    } else {
        s->set_whitebal(s, 0);      /* AWB off */
        s->set_wb_mode(s, mode);    /* 1=Sunny 2=Cloudy 3=Office 4=Home */
    }
    return ESP_OK;
}
