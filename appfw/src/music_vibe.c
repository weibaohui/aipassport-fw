// appfw/src/music_vibe.c —— 见 music_vibe.h。
//
// 布局:分层数据 + 薄渲染。band/cap/几何全部定点(uint16 q8 / uint8),
// setter 前逐属性差分;LVGL 调用集中在文件尾的三个静态函数里,主机单测
// 用 -DMUSIC_VIBE_NO_LVGL 把它们替换为调用记录。
#include "music_vibe.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#ifdef MUSIC_VIBE_NO_LVGL
typedef uint8_t mv_obj_t;                 // 测试态:对象用 id 表示
#else
#include "lvgl.h"
typedef lv_obj_t mv_obj_t;
#endif

// ---- 几何常量(240x320;A 在上半,B 中心线 y=160) ----
#define A_Y0        8                     // LED 表顶
#define A_SEG_H     12                    // 段高(12 段 = 144px)
#define A_SEGS      12
#define A_BOT       (A_Y0 + A_SEGS * A_SEG_H)   // 152,填充底边
#define A_SLOT_W    24                    // 10 列等宽
#define A_COL_W     20                    // 列内填充宽
#define A_LINES     11                    // 内部分隔线(切出 12 段)
#define B_CY        160                   // 对称中心线
#define B_MAX_UP    130                   // 上半最大高
#define B_MAX_DN    (B_MAX_UP * 55 / 100) // 下半 = 上半的 0.55 倍

// ---- 颜色(纯色,零 alpha 混合) ----
#define C_SLOT      0x101820              // A 底槽暗色
#define C_SEPLINE   0x1B2534              // A 分隔线
#define C_GREEN     0x35C26B
#define C_YELLOW    0xFFC531
#define C_RED       0xE5484D
#define C_BASELINE  0x3A4A5C              // B 中心基准线

// ---- 对象编号(线性数组下标) ----
enum {
    OB_SLOT0 = 0, OB_SLOT_LAST = OB_SLOT0 + 9,
    OB_LINE0, OB_LINE_LAST = OB_LINE0 + A_LINES - 1,
    OB_FILL0, OB_FILL_LAST = OB_FILL0 + 9,
    OB_CAP0, OB_CAP_LAST = OB_CAP0 + 9,
    OB_BUP0, OB_BUP_LAST = OB_BUP0 + 9,
    OB_BDN0, OB_BDN_LAST = OB_BDN0 + 9,
    OB_BLINE0, OB_BLINE_LAST = OB_BLINE0 + 9,
    OB_TOTAL
};

// ---- 状态(全部 static,算上 LUT < 200B) ----
static uint16_t s_band[10];              // q8,0..256
static uint16_t s_cap[10];               // q8 峰值保持
static uint16_t s_lut[32];               // RGB565 色带
static uint8_t s_x[11];                  // B 段边界 x(≤230,uint8 装得下)
static uint8_t s_last_fill_h[10];        // 差分缓存(A 填充高度)
static uint8_t s_last_cap_y[10];         //            (A 峰帽 y)
static uint8_t s_last_color[10];         //            (A 颜色档 0绿1黄2红,255=未定)
static uint8_t s_last_up[10];            //            (B 上半高)
static uint8_t s_last_dn[10];            //            (B 下半高)
static uint8_t s_mask;                   // 页面开关(bit0=A,bit1=B)
static mv_obj_t *s_ob[OB_TOTAL];

// ---- 测试记录(NO_LVGL 下统计真实 setter 调用次数) ----
#ifdef MUSIC_VIBE_TEST
static uint32_t s_setters;
#endif
static inline void setter(void) {
#ifdef MUSIC_VIBE_TEST
    s_setters++;
#endif
}

// ---- 差分 setter(值相同绝不调) ----
static void ob_h(uint8_t id, uint8_t h)
{
    (void)id; (void)h;                       // NO_LVGL 下只剩记录
#ifndef MUSIC_VIBE_NO_LVGL
    lv_obj_set_height(s_ob[id], h);
#endif
    setter();
}

static void ob_y(uint8_t id, uint8_t y)
{
    (void)id; (void)y;
#ifndef MUSIC_VIBE_NO_LVGL
    lv_obj_set_y(s_ob[id], y);
#endif
    setter();
}

static void ob_hidden(uint8_t id, bool hidden)
{
    (void)id; (void)hidden;
#ifndef MUSIC_VIBE_NO_LVGL
    if (hidden) lv_obj_add_flag(s_ob[id], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(s_ob[id], LV_OBJ_FLAG_HIDDEN);
#endif
    setter();
}

// ---- LUT:深蓝→青→绿→黄→橙→红→品红→深蓝,S/V 压在 0.2~0.85。 ----
// 直接对 hue 环插值会经过荧光黄绿(刺眼),所以用锚点表手工绕开。
typedef struct { uint16_t h; uint8_t s, v; } hsv_stop_t;
static const hsv_stop_t STOPS[] = {
    { 240, 216, 76 },   // 深蓝  (s/v 均 0.85 内)
    { 180, 192, 153 },  // 青
    { 120, 179, 191 },  // 绿
    {  50, 217, 217 },  // 黄
    {  25, 217, 204 },  // 橙
    {   0, 217, 191 },  // 红
    { 300, 204, 179 },  // 品红
    { 240, 216, 76 },   // 深蓝(回到起点,首尾同色闭环)
};

static uint16_t hsv565(int h, int s, int v)
{
    // s/v 0..255;返回 RGB565
    const int region = (h / 60) % 6;
    const int f = (h % 60) * 255 / 60;
    const int p = (v * (255 - s)) / 255;
    const int q = (v * (255 - ((s * f) / 255))) / 255;
    const int t = (v * (255 - ((s * (255 - f)) / 255))) / 255;
    int r, g, b;
    switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void lut_gen(void)
{
    const int n = (int)(sizeof(STOPS) / sizeof(STOPS[0])) - 1;   // 段数
    for (int i = 0; i < 32; i++) {
        const int x = i * n * 256 / 31;            // 定点区间位置
        const int seg = (x >> 8) >= n ? n - 1 : (x >> 8);
        const int frac = x - (seg << 8);
        const hsv_stop_t *a = &STOPS[seg], *b = &STOPS[seg + 1];
        int dh = (int)b->h - (int)a->h;
        // 色相差走短弧(如 300→240 不该绕 0°)
        if (dh > 180) dh -= 360;
        else if (dh < -180) dh += 360;
        const int h = (int)a->h + (dh * frac) / 256;
        const int s = (int)a->s + (((int)b->s - (int)a->s) * frac) / 256;
        const int v = (int)a->v + (((int)b->v - (int)a->v) * frac) / 256;
        s_lut[i] = hsv565(h, s, v);
    }
}

// ---- 创建(一次性) ----
static mv_obj_t *new_rect(mv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                          uint32_t color, int32_t radius)
{
#ifndef MUSIC_VIBE_NO_LVGL
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
#else
    (void)parent; (void)x; (void)y; (void)w; (void)h; (void)color; (void)radius;
    return 0;
#endif
}

void music_vibe_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    lut_gen();

    mv_obj_t *parent = 0;                    // NO_LVGL:无宿主,对象数组全空
#ifndef MUSIC_VIBE_NO_LVGL
    parent = lv_scr_act();
#endif
    uint8_t id = OB_SLOT0;
    for (int i = 0; i < 10; i++)                       // A 底槽
        s_ob[id++] = new_rect(parent, i * A_SLOT_W + 2, A_Y0, A_COL_W,
                              A_SEGS * A_SEG_H, C_SLOT, 0);
    for (int k = 1; k <= A_LINES; k++)                 // A 分隔线(横贯全宽)
        s_ob[id++] = new_rect(parent, 0, A_Y0 + k * A_SEG_H - 1, 240, 1,
                              C_SEPLINE, 0);
    for (int i = 0; i < 10; i++)                       // A 填充(顶在底边)
        s_ob[id++] = new_rect(parent, i * A_SLOT_W + 2, A_BOT, A_COL_W, 1,
                              C_GREEN, 0);
    for (int i = 0; i < 10; i++)                       // A 峰帽
        s_ob[id++] = new_rect(parent, i * A_SLOT_W + 2, A_BOT - 2, A_COL_W, 2,
                              C_YELLOW, 0);
    // B:x_i = 10 + (i/10)^1.7 * 220,对数观感的对数分频
    int px[11];
    for (int i = 0; i <= 10; i++) {
        px[i] = 10 + (int)((float)powf((float)i / 10.0f, 1.7f) * 220.0f + 0.5f);
        s_x[i] = (uint8_t)px[i];
    }
    for (int i = 0; i < 10; i++)                          // B 中心基准线
        s_ob[id++] = new_rect(parent, px[i], B_CY - 1, px[i + 1] - px[i], 1,
                              C_BASELINE, 0);
    for (int i = 0; i < 10; i++) {                        // B 上半柱(颜色按段固定)
        const uint16_t idx = (uint16_t)((uint32_t)i * 26 / 10);
        s_ob[id++] = new_rect(parent, px[i], B_CY - 1, px[i + 1] - px[i], 1,
                              s_lut[idx], 0);
    }
    for (int i = 0; i < 10; i++) {                        // B 下半柱
        const uint16_t idx = (uint16_t)((uint32_t)i * 26 / 10);
        s_ob[id++] = new_rect(parent, px[i], B_CY, px[i + 1] - px[i], 1,
                              s_lut[idx], 0);
    }

    music_vibe_enable(0);                                // 默认全隐藏
}

// 页面显隐(差分:开关位变化才动 flag)
void music_vibe_enable(uint8_t page_mask)
{
    s_mask = page_mask & 3u;
    const bool a = (s_mask & 1u) != 0, b = (s_mask & 2u) != 0;
    for (uint8_t i = OB_SLOT0; i <= OB_CAP_LAST; i++) ob_hidden(i, !a);
    for (uint8_t i = OB_BUP0; i <= OB_BLINE_LAST; i++) ob_hidden(i, !b);
    for (uint8_t i = 0; i < 10; i++) {                   // 开关变化后强制重画一帧
        s_last_fill_h[i] = 0xFF;
        s_last_cap_y[i] = 0xFF;
        s_last_up[i] = 0xFF;
        s_last_dn[i] = 0xFF;
        s_last_color[i] = 255;
    }
}

void music_vibe_note_on(uint8_t pitch, uint8_t velocity)
{
    int b = ((int)pitch - 36) / 4;
    if (b < 0) b = 0;
    if (b > 9) b = 9;
    const uint32_t v = (uint32_t)velocity * 256u / 127u;   // 归一 q8
    const uint32_t main = v * 205u >> 8;                   // ×0.80
    const uint32_t left = v * 90u >> 8;                    // ×0.35 → b-1
    const uint32_t right = v * 115u >> 8;                  // ×0.45 → b+1

    s_band[b] = (uint16_t)(s_band[b] + main > 256 ? 256 : s_band[b] + main);
    if (b > 0)
        s_band[b - 1] = (uint16_t)(s_band[b - 1] + left > 256 ? 256 : s_band[b - 1] + left);
    if (b < 9)
        s_band[b + 1] = (uint16_t)(s_band[b + 1] + right > 256 ? 256 : s_band[b + 1] + right);
}

void music_vibe_tick(uint32_t dt_ms)
{
    // 数据层:衰减 + 峰帽
    for (int i = 0; i < 10; i++) {
        s_band[i] = (uint16_t)((uint32_t)s_band[i] * 225u >> 8);   // ×0.88
        uint32_t cap = s_cap[i];
        if (s_band[i] >= cap) cap = s_band[i];
        else {
            const uint32_t drop = (140u * dt_ms) / 1000u;          // 0.55/s → q8
            cap = cap > drop ? cap - drop : 0;
        }
        s_cap[i] = (uint16_t)cap;
    }

    if (s_mask & 1u) {                                             // A 渲染
        for (int i = 0; i < 10; i++) {
            const uint8_t seg = (uint8_t)((uint32_t)s_band[i] * A_SEGS / 256);
            const uint8_t h = (uint8_t)(seg * A_SEG_H);
            if (h != s_last_fill_h[i]) {
                s_last_fill_h[i] = h;
                ob_h(OB_FILL0 + i, h);
                ob_y(OB_FILL0 + i, (uint8_t)(A_BOT - h));
#ifndef MUSIC_VIBE_NO_LVGL
                // h==0 → 隐藏(0 高度对象照样触发失效重绘,白烧 1ms)
                if (h) lv_obj_clear_flag(s_ob[OB_FILL0 + i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(s_ob[OB_FILL0 + i], LV_OBJ_FLAG_HIDDEN);
#endif
                // 颜色三档是 h 的纯函数,h 变才查一次档
                const uint8_t tier = seg >= 10 ? 2u : seg >= 8 ? 1u : 0u;
                if (tier != s_last_color[i]) {
                    s_last_color[i] = tier;
#ifndef MUSIC_VIBE_NO_LVGL
                    static const uint32_t TIERC[3] = { C_GREEN, C_YELLOW, C_RED };
                    lv_obj_set_style_bg_color(s_ob[OB_FILL0 + i],
                                              lv_color_hex(TIERC[tier]), 0);
#endif
                    setter();
                }
            }
            const uint8_t cap_y =
                (uint8_t)(A_BOT - 2 - (uint32_t)s_cap[i] * (A_SEGS * A_SEG_H) / 256);
            if (cap_y != s_last_cap_y[i]) {
                s_last_cap_y[i] = cap_y;
                ob_y(OB_CAP0 + i, cap_y);
            }
        }
    }
    if (s_mask & 2u) {                                             // B 渲染
        for (int i = 0; i < 10; i++) {
            const uint8_t up = (uint8_t)((uint32_t)s_band[i] * B_MAX_UP / 256);
            const uint8_t dn = (uint8_t)((uint32_t)up * 55u / 100u);
            if (up != s_last_up[i]) {
                s_last_up[i] = up;
                ob_h(OB_BUP0 + i, up);
                ob_y(OB_BUP0 + i, (uint8_t)(B_CY - up));
#ifndef MUSIC_VIBE_NO_LVGL
                if (up) lv_obj_clear_flag(s_ob[OB_BUP0 + i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(s_ob[OB_BUP0 + i], LV_OBJ_FLAG_HIDDEN);
#endif
            }
            if (dn != s_last_dn[i]) {
                s_last_dn[i] = dn;
                ob_h(OB_BDN0 + i, dn);
#ifndef MUSIC_VIBE_NO_LVGL
                if (dn) lv_obj_clear_flag(s_ob[OB_BDN0 + i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_add_flag(s_ob[OB_BDN0 + i], LV_OBJ_FLAG_HIDDEN);
#endif
            }
        }
    }
}

#ifdef MUSIC_VIBE_TEST
uint16_t music_vibe_test_band(uint8_t i) { return s_band[i & 15]; }
uint16_t music_vibe_test_cap(uint8_t i) { return s_cap[i & 15]; }
uint16_t music_vibe_test_lut(uint8_t i) { return s_lut[i & 31]; }
uint32_t music_vibe_test_setters(void) { return s_setters; }
#endif
