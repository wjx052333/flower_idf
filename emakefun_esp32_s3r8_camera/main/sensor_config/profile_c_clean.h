/*
 * Profile C — clean-first
 *
 * Strategy: night mode + conservative AE + low gain ceiling + light denoise
 *   优先压低噪声保留细节，阴天光线尚可时对比度最好
 *   代价是画面偏暗
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    {0x3a00, 0x3b}, // night mode on
    {0x3a18, 0x00}, // gain ceiling high
    {0x3a19, 0x40}, // gain ceiling low  → max ~4x (保守)
    {0x3a0f, 0x30}, // AE target high  ↓ (接近 sensor 默认)
    {0x3a10, 0x28}, // AE target low
    {0x3a1b, 0x30}, // AE band2 high
    {0x3a1e, 0x28}, // AE band2 low
    {0x3a11, 0x50}, // fast AE high
    {0x3a1f, 0x16}, // fast AE low
    {0x5306, 0x14}, // denoise offset1  (轻度降噪，保细节)
    {0x5307, 0x18}, // denoise offset2  (轻度降噪)

    {OV3660_REGLIST_TAIL, 0x00},
};