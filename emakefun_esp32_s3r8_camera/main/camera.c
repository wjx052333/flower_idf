/**
 * camera.c — OV3660 DVP camera via esp_video / V4L2
 *
 * Isolated in its own compilation unit so that linux/videodev2.h and
 * lwip/sockets.h (pulled in by esp_http_client) never share a translation
 * unit — both define _IO/_IOR/_IOW/_IOWR with incompatible signatures.
 */

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor_types.h"

#include "esp_jpeg_enc.h"

#include "camera.h"

#ifndef ESP_VIDEO_DVP_DEVICE_NAME
#define ESP_VIDEO_DVP_DEVICE_NAME "/dev/video2"
#endif

#define CAM_BUF_COUNT 2
#define TAG "Camera"

static int               s_cam_fd = -1;
static uint8_t          *s_cam_buf[CAM_BUF_COUNT];
static uint32_t          s_cam_buf_size[CAM_BUF_COUNT];
static uint32_t          s_cam_pixel_fmt;
static uint32_t          s_cam_width;
static uint32_t          s_cam_height;
static SemaphoreHandle_t s_cam_sem;

/* Software JPEG encoder for sensors that don't output JPEG natively */
static jpeg_enc_handle_t s_jpeg_enc     = NULL;
static uint8_t          *s_jpeg_out_buf = NULL;
static uint32_t          s_jpeg_out_size = 0;

/* Holds the currently dequeued buffer between capture and release. */
static struct v4l2_buffer s_vbuf;

/* Flash LED GPIO (-1 = not configured) */
static int s_led_gpio = -1;

/* LED off-delay timer — each capture restarts it, so continuous capture keeps LED on */
static esp_timer_handle_t s_led_timer = NULL;
#define LED_OFF_DELAY_US  (1000 * 1000)

static void led_off_timer_cb(void *arg)
{
    gpio_set_level(s_led_gpio, 0);
}

esp_err_t camera_init_with_i2c(i2c_master_bus_handle_t i2c_handle)
{
    esp_err_t ret;
    bool own_i2c = (i2c_handle == NULL);
    if (own_i2c) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port              = CONFIG_CAM_SCCB_I2C_PORT,
            .scl_io_num            = CONFIG_CAM_SCCB_SCL_PIN,
            .sda_io_num            = CONFIG_CAM_SCCB_SDA_PIN,
            .clk_source            = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt     = 7,
            .flags.enable_internal_pullup = true,
        };
        ret = i2c_new_master_bus(&bus_cfg, &i2c_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    esp_video_init_dvp_config_t dvp_cfg = {
        .sccb_config = {
            .init_sccb  = false,
            .i2c_handle = i2c_handle,
            .freq       = CONFIG_CAM_SCCB_I2C_FREQ,
        },
        .reset_pin = CONFIG_CAM_RESET_PIN,
        .pwdn_pin  = CONFIG_CAM_PWDN_PIN,
        .dvp_pin = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                CONFIG_CAM_D0_PIN,
                CONFIG_CAM_D1_PIN,
                CONFIG_CAM_D2_PIN,
                CONFIG_CAM_D3_PIN,
                CONFIG_CAM_D4_PIN,
                CONFIG_CAM_D5_PIN,
                CONFIG_CAM_D6_PIN,
                CONFIG_CAM_D7_PIN,
            },
            .vsync_io = CONFIG_CAM_VSYNC_PIN,
            .de_io    = CONFIG_CAM_DE_PIN,
            .pclk_io  = CONFIG_CAM_PCLK_PIN,
            .xclk_io  = CONFIG_CAM_XCLK_PIN,
        },
        .xclk_freq = CONFIG_CAM_XCLK_FREQ,
    };

    esp_video_init_config_t cam_cfg = { .dvp = &dvp_cfg };
    ret = esp_video_init(&cam_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        if (own_i2c) i2c_del_master_bus(i2c_handle);
        return ret;
    }
    ESP_LOGI(TAG, "esp_video_init OK");

    s_cam_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (s_cam_fd < 0) {
        ESP_LOGE(TAG, "open %s failed (errno=%d)", ESP_VIDEO_DVP_DEVICE_NAME, errno);
        return ESP_ERR_NOT_FOUND;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return ESP_FAIL;
    }
    s_cam_pixel_fmt = fmt.fmt.pix.pixelformat;
    s_cam_width     = fmt.fmt.pix.width;
    s_cam_height    = fmt.fmt.pix.height;
    ESP_LOGI(TAG, "Camera format: %ux%u fmt=%.4s",
             s_cam_width, s_cam_height,
             (char *)&s_cam_pixel_fmt);

    if (s_cam_pixel_fmt != V4L2_PIX_FMT_JPEG) {
        jpeg_enc_config_t enc_cfg = {
            .width       = s_cam_width,
            .height      = s_cam_height,
            .src_type    = JPEG_PIXEL_FORMAT_YCbYCr,
            .subsampling = JPEG_SUBSAMPLE_422,
            .quality     = 80,
            .rotate      = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        jpeg_error_t jerr = jpeg_enc_open(&enc_cfg, &s_jpeg_enc);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "jpeg_enc_open failed: %d", jerr);
        } else {
            s_jpeg_out_size = s_cam_width * s_cam_height;
            s_jpeg_out_buf  = jpeg_calloc_align(s_jpeg_out_size, 16);
            if (!s_jpeg_out_buf) {
                ESP_LOGE(TAG, "jpeg_calloc_align failed");
                jpeg_enc_close(s_jpeg_enc);
                s_jpeg_enc = NULL;
            } else {
                ESP_LOGI(TAG, "Software JPEG encoder opened");
            }
        }
    }

    struct v4l2_requestbuffers reqbuf = {0};
    reqbuf.count  = CAM_BUF_COUNT;
    reqbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return ESP_FAIL;
    }

    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(s_cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed", i);
            close(s_cam_fd);
            s_cam_fd = -1;
            return ESP_FAIL;
        }
        s_cam_buf[i] = (uint8_t *)mmap(NULL, buf.length,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED, s_cam_fd, buf.m.offset);
        if (s_cam_buf[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%d] failed", i);
            close(s_cam_fd);
            s_cam_fd = -1;
            return ESP_ERR_NO_MEM;
        }
        s_cam_buf_size[i] = buf.length;

        if (ioctl(s_cam_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed", i);
            close(s_cam_fd);
            s_cam_fd = -1;
            return ESP_FAIL;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return ESP_FAIL;
    }

    s_cam_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(s_cam_sem);

    /* Flash LED init (emakefun ESP32-S3-R8: GPIO 3) */
#if CONFIG_CAM_LED_GPIO >= 0
    s_led_gpio = CONFIG_CAM_LED_GPIO;
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << s_led_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(s_led_gpio, 0);
    ESP_LOGI(TAG, "Flash LED on GPIO %d, off-delay %d ms", s_led_gpio,
             (int)(LED_OFF_DELAY_US / 1000));
#endif

    /* Create LED off-delay timer */
    if (s_led_gpio >= 0) {
        esp_timer_create_args_t targs = {
            .callback              = led_off_timer_cb,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "led_off",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&targs, &s_led_timer);
    }

    /* JPEG quality: 1 = max quality for OV3660 (range 1–63, lower = better) */
    {
        struct v4l2_ext_control ctrl  = { .id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = 1 };
        struct v4l2_ext_controls ctrls = { .count = 1, .controls = &ctrl };
        if (ioctl(s_cam_fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0)
            ESP_LOGW(TAG, "JPEG quality init failed (errno=%d)", errno);
    }

    ESP_LOGI(TAG, "Camera stream started (fd=%d)", s_cam_fd);
    return ESP_OK;
}

esp_err_t camera_init(void)
{
    return camera_init_with_i2c(NULL);
}

bool camera_is_ready(void)
{
    return s_cam_fd >= 0;
}

esp_err_t camera_capture_jpeg(const uint8_t **data, uint32_t *size)
{
    if (s_cam_fd < 0) {
        ESP_LOGE(TAG, "camera_capture_jpeg: camera not ready");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cam_sem, portMAX_DELAY);

    /* Drain stale buffers (filled at STREAMON), requeue so DMA refills them. */
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        memset(&s_vbuf, 0, sizeof(s_vbuf));
        s_vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        s_vbuf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_cam_fd, VIDIOC_DQBUF, &s_vbuf) < 0) {
            xSemaphoreGive(s_cam_sem);
            ESP_LOGE(TAG, "VIDIOC_DQBUF drain[%d] failed (errno=%d)", i, errno);
            return ESP_FAIL;
        }
        ioctl(s_cam_fd, VIDIOC_QBUF, &s_vbuf);
    }

    /* Flash LED on before fresh exposure */
    if (s_led_gpio >= 0) gpio_set_level(s_led_gpio, 1);

    /* Wait for DMA to refill with a fresh frame. OV3660 @20MHz XCLK needs
     * more settling time than OV5640; 500ms covers worst-case AE convergence. */
    vTaskDelay(pdMS_TO_TICKS(500));

    memset(&s_vbuf, 0, sizeof(s_vbuf));
    s_vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s_vbuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam_fd, VIDIOC_DQBUF, &s_vbuf) < 0) {
        if (s_led_gpio >= 0) {
            gpio_set_level(s_led_gpio, 0);
            if (s_led_timer) esp_timer_stop(s_led_timer);
        }
        xSemaphoreGive(s_cam_sem);
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed (errno=%d)", errno);
        return ESP_FAIL;
    }

    /* Restart off-delay timer: LED stays on during continuous capture */
    if (s_led_gpio >= 0) {
        esp_timer_stop(s_led_timer);
        esp_timer_start_once(s_led_timer, LED_OFF_DELAY_US);
    }

    *data = s_cam_buf[s_vbuf.index];
    *size = s_vbuf.bytesused;

    if (s_jpeg_enc) {
        int out_size = 0;
        jpeg_error_t jerr = jpeg_enc_process(s_jpeg_enc, *data, *size,
                                              s_jpeg_out_buf, s_jpeg_out_size, &out_size);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "jpeg_enc_process failed: %d", jerr);
            xSemaphoreGive(s_cam_sem);
            return ESP_FAIL;
        }
        *data = s_jpeg_out_buf;
        *size = out_size;
    }
    return ESP_OK;
}

void camera_release_jpeg(void)
{
    ioctl(s_cam_fd, VIDIOC_QBUF, &s_vbuf);
    xSemaphoreGive(s_cam_sem);
}

/* ── Runtime sensor controls ─────────────────────────────────────────────── */

static esp_err_t set_v4l2_ctrl(uint32_t cid, int32_t value)
{
    struct v4l2_ext_control  ctrl  = { .id = cid, .value = value };
    struct v4l2_ext_controls ctrls = { .count = 1, .controls = &ctrl };
    if (ioctl(s_cam_fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
        ESP_LOGE(TAG, "set_v4l2_ctrl cid=0x%x val=%d failed (errno=%d)", cid, value, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sensor_write_reg(uint16_t reg, uint8_t val)
{
    esp_cam_sensor_reg_val_t rv = { .regaddr = reg, .value = val };
    struct v4l2_ext_control  ctrl  = {
        .id   = ESP_CAM_SENSOR_IOC_S_REG,
        .p_u8 = (uint8_t *)&rv,
        .size = sizeof(rv),
    };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL,
        .count      = 1,
        .controls   = &ctrl,
    };
    if (ioctl(s_cam_fd, VIDIOC_S_EXT_CTRLS, &ctrls) < 0) {
        ESP_LOGE(TAG, "sensor_write_reg 0x%04x=0x%02x failed (errno=%d)", reg, val, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t camera_set_hmirror(int enable)
{
    return set_v4l2_ctrl(V4L2_CID_HFLIP, enable ? 1 : 0);
}

esp_err_t camera_set_vflip(int enable)
{
    return set_v4l2_ctrl(V4L2_CID_VFLIP, enable ? 1 : 0);
}

esp_err_t camera_set_contrast(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    /* OV3660: reg 0x5586 = (level+4) << 3 */
    return sensor_write_reg(0x5586, (uint8_t)((level + 4) << 3));
}

esp_err_t camera_set_saturation(int level)
{
    if (level < -2 || level > 2) return ESP_ERR_INVALID_ARG;
    /* OV3660 color matrix registers 0x5381–0x538B (11 regs), levels −2..+2 */
    static const uint8_t sat[5][11] = {
        {0x1d,0x60,0x03,0x0a,0x60,0x6a,0x64,0x56,0x0e,0x01,0x98}, /* -2 */
        {0x1d,0x60,0x03,0x0b,0x6c,0x77,0x70,0x60,0x10,0x01,0x98}, /* -1 */
        {0x1d,0x60,0x03,0x0c,0x78,0x84,0x7d,0x6b,0x12,0x01,0x98}, /*  0 */
        {0x1d,0x60,0x03,0x0d,0x84,0x91,0x8a,0x76,0x14,0x01,0x98}, /* +1 */
        {0x1d,0x60,0x03,0x0e,0x90,0x9e,0x96,0x80,0x16,0x01,0x98}, /* +2 */
    };
    const uint8_t *row = sat[level + 2];
    for (int i = 0; i < 11; i++) {
        esp_err_t ret = sensor_write_reg(0x5381 + i, row[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

esp_err_t camera_set_wb_mode(int mode)
{
    if (mode < 0 || mode > 4) return ESP_ERR_INVALID_ARG;
    /* reg 0x3406: 0 = AWB auto, 1 = manual gains */
    sensor_write_reg(0x3406, mode != 0 ? 1 : 0);
    /* 12-bit gains: reg N = high nibble, reg N+1 = low byte */
    switch (mode) {
    case 1: /* Sunny */
        sensor_write_reg(0x3400,0x05); sensor_write_reg(0x3401,0xe0); /* R 0x5e0 */
        sensor_write_reg(0x3402,0x04); sensor_write_reg(0x3403,0x10); /* G 0x410 */
        sensor_write_reg(0x3404,0x05); sensor_write_reg(0x3405,0x40); /* B 0x540 */
        break;
    case 2: /* Cloudy */
        sensor_write_reg(0x3400,0x06); sensor_write_reg(0x3401,0x50); /* R 0x650 */
        sensor_write_reg(0x3402,0x04); sensor_write_reg(0x3403,0x10); /* G 0x410 */
        sensor_write_reg(0x3404,0x04); sensor_write_reg(0x3405,0xf0); /* B 0x4f0 */
        break;
    case 3: /* Office */
        sensor_write_reg(0x3400,0x05); sensor_write_reg(0x3401,0x20); /* R 0x520 */
        sensor_write_reg(0x3402,0x04); sensor_write_reg(0x3403,0x10); /* G 0x410 */
        sensor_write_reg(0x3404,0x06); sensor_write_reg(0x3405,0x60); /* B 0x660 */
        break;
    case 4: /* Home */
        sensor_write_reg(0x3400,0x04); sensor_write_reg(0x3401,0x20); /* R 0x420 */
        sensor_write_reg(0x3402,0x03); sensor_write_reg(0x3403,0xf0); /* G 0x3f0 */
        sensor_write_reg(0x3404,0x07); sensor_write_reg(0x3405,0x10); /* B 0x710 */
        break;
    default: /* Auto (0) — AWB already enabled via 0x3406=0 */
        break;
    }
    return ESP_OK;
}
