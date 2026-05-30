/*
 * Profile A — brightness-first (当前参数的等价版本)
 *
 * Strategy: night mode + high AE target + full gain range + strong denoise
 *   阴天偏暗场景优先保亮度，接受噪声代价
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    {OV3660_REGLIST_TAIL, 0x00},
};