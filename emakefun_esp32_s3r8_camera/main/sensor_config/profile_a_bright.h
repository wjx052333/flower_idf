/*
 * Profile A — brightness-first (当前参数的等价版本)
 *
 * Strategy: night mode + high AE target + full gain range + strong denoise
 *   阴天偏暗场景优先保亮度，接受噪声代价
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    {0x3a00, 0x3b}, // night mode on  (frame integration, up to 4-frame)
    {0x3a18, 0x00}, // gain ceiling high
    {0x3a19, 0xf8}, // gain ceiling low  → max ~15.5x (full range, 最亮)
    {0x3a0f, 0x50}, // AE target high
    {0x3a10, 0x40}, // AE target low
    {0x3a1b, 0x50}, // AE band2 high
    {0x3a1e, 0x40}, // AE band2 low
    {0x3a11, 0x80}, // fast AE high
    {0x3a1f, 0x28}, // fast AE low
    {0x5306, 0x30}, // denoise offset1  (强降噪)
    {0x5307, 0x40}, // denoise offset2  (强降噪)

    {OV3660_REGLIST_TAIL, 0x00},
};