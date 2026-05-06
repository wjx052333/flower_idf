/**
 * camera.c — OV5640 DVP camera via esp_video / V4L2
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

#include "esp_log.h"
#include "esp_err.h"

#include "driver/i2c_master.h"

#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_cam_ctlr.h"

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

/* Software JPEG encoder for non-native-JPEG sensors (e.g. OV5640 outputs UYVY) */
static jpeg_enc_handle_t s_jpeg_enc     = NULL;
static uint8_t          *s_jpeg_out_buf = NULL;
static uint32_t          s_jpeg_out_size = 0;

/* Holds the currently dequeued buffer between capture and release. */
static struct v4l2_buffer s_vbuf;

esp_err_t camera_init(void)
{
    /* Pre-init I2C with internal pullups so OV5640 SCCB bus is not floating */
    i2c_master_bus_handle_t i2c_handle = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port              = CONFIG_CAM_SCCB_I2C_PORT,
        .scl_io_num            = CONFIG_CAM_SCCB_SCL_PIN,
        .sda_io_num            = CONFIG_CAM_SCCB_SDA_PIN,
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
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
        i2c_del_master_bus(i2c_handle);
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

    /* If the sensor doesn't output JPEG natively, open a software encoder */
    if (s_cam_pixel_fmt != V4L2_PIX_FMT_JPEG) {
        jpeg_enc_config_t enc_cfg = {
            .width       = s_cam_width,
            .height      = s_cam_height,
            .src_type    = JPEG_PIXEL_FORMAT_CbYCrY,
            .subsampling = JPEG_SUBSAMPLE_422,
            .quality     = 80,
            .rotate      = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        jpeg_error_t jerr = jpeg_enc_open(&enc_cfg, &s_jpeg_enc);
        if (jerr != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "jpeg_enc_open failed: %d", jerr);
        } else {
            s_jpeg_out_size = s_cam_width * s_cam_height;  /* more than enough for JPEG */
            s_jpeg_out_buf  = jpeg_calloc_align(s_jpeg_out_size, 16);
            if (!s_jpeg_out_buf) {
                ESP_LOGE(TAG, "jpeg_calloc_align failed");
                jpeg_enc_close(s_jpeg_enc);
                s_jpeg_enc = NULL;
            } else {
                ESP_LOGI(TAG, "Software JPEG encoder opened (UYVY→JPEG)");
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

    ESP_LOGI(TAG, "Camera stream started (fd=%d)", s_cam_fd);
    return ESP_OK;
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
    memset(&s_vbuf, 0, sizeof(s_vbuf));
    s_vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    s_vbuf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam_fd, VIDIOC_DQBUF, &s_vbuf) < 0) {
        xSemaphoreGive(s_cam_sem);
        ESP_LOGE(TAG, "VIDIOC_DQBUF failed (errno=%d)", errno);
        return ESP_FAIL;
    }
    *data = s_cam_buf[s_vbuf.index];
    *size = s_vbuf.bytesused;

    /* Convert to JPEG if the sensor outputs raw YUV */
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
