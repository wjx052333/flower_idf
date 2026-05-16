/*
 * Profile X — original (之前调试好的版本)
 *
 * Low-light tuning: night mode + higher AE target + stronger denoise
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    /* Low-light tuning: night mode + higher AE target + stronger denoise */
    {0x3a00, 0x3b}, // night mode on (frame integration)
    {0x3a0f, 0x50}, // AE target high  ↑
    {0x3a10, 0x40}, // AE target low   ↑
    {0x3a1b, 0x50}, // AE band2 high   ↑
    {0x3a1e, 0x40}, // AE band2 low    ↑
    {0x3a11, 0x80}, // fast AE high    ↑
    {0x3a1f, 0x28}, // fast AE low     ↑
    {0x5306, 0x30}, // denoise offset1 ↑
    {0x5307, 0x40}, // denoise offset2 ↑

    {OV3660_REGLIST_TAIL, 0x00}, // tail
};