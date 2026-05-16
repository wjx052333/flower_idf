/*
 * Profile D — detail-first
 *
 * Strategy: no night mode + near-default AE + minimal gain + almost no denoise
 *   白天/光线充足时用，细节最锐利
 *   阴天会偏暗（增益被限在 2x），不适合暗光
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    {0x3a00, 0x3a}, // night mode off (关闭帧积分，AE 收敛更快)
    {0x3a18, 0x00}, // gain ceiling high
    {0x3a19, 0x20}, // gain ceiling low  → max ~2x (白天足够)
    {0x3a0f, 0x28}, // AE target high  ↓ (sensor 默认)
    {0x3a10, 0x20}, // AE target low
    {0x3a1b, 0x28}, // AE band2 high
    {0x3a1e, 0x20}, // AE band2 low
    {0x3a11, 0x40}, // fast AE high
    {0x3a1f, 0x10}, // fast AE low
    {0x5306, 0x0c}, // denoise offset1  (几乎不降噪，锐度最高)
    {0x5307, 0x10}, // denoise offset2

    {OV3660_REGLIST_TAIL, 0x00},
};