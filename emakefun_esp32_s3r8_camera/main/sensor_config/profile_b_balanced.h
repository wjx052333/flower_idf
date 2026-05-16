/*
 * Profile B — balanced
 *
 * Strategy: night mode + moderate AE target + moderate gain + moderate denoise
 *   在亮度和噪声之间折中，牺牲一些亮度换更干净的画面
 */
static const ov3660_reginfo_t ov3660_dvp_8bit_10Minput_1280x720_jpeg_12fps[] = {
    ov3660_settings_1280X720
    ov3660_settings_fmt_jpeg

    {0x3a00, 0x3b}, // night mode on
    {0x3a18, 0x00}, // gain ceiling high
    {0x3a19, 0x80}, // gain ceiling low  → max ~8x (折中)
    {0x3a0f, 0x40}, // AE target high  ↓ (vs bright: 50)
    {0x3a10, 0x30}, // AE target low   ↓
    {0x3a1b, 0x40}, // AE band2 high   ↓
    {0x3a1e, 0x30}, // AE band2 low    ↓
    {0x3a11, 0x68}, // fast AE high    ↓ (vs bright: 0x80)
    {0x3a1f, 0x1e}, // fast AE low     ↓
    {0x5306, 0x20}, // denoise offset1  (中等降噪)
    {0x5307, 0x28}, // denoise offset2  (中等降噪)

    {OV3660_REGLIST_TAIL, 0x00},
};