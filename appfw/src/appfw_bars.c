// appfw/src/appfw_bars.c —— 见 appfw_bars.h。
#include "appfw_bars.h"

#include <string.h>

// ---- 配色:柱色随高度(0..255)从青绿走到亮黄 ----
//
// 色相不是线性走的:用 gamma=1.6 把大部分柱子压在青绿区,只有最高的
// 20% 才推到黄。线性映射的话中等响的柱子就已经是纯绿了,看着像
// 绿→黄两段,少了"一片薄荷色里挑出几根黄的"层次。
//
// 注意 s/v 是**百分比 0-100**,lv_color_hsv_to_rgb 内部自己乘 255/100。
// 当成 0-255 传会被 uint8_t 截断(231→77、218→37),整屏变成暗褐色,
// 而且症状很隐蔽:条子照样画得出来,只是颜色全不对(踩过一次)。
static lv_color_t bar_color(uint8_t lv255)
{
    const int t = lv255;                       // 0..255
    const int hue = 168 - (t * (168 - 48)) / 255;   // 青 168° → 黄 48°
    const int sat = 88 - (t * 30) / 255;            // 越高越饱和
    const int val = 55 + (t * 43) / 255;            // 越高越亮
    return lv_color_hsv_to_rgb((uint16_t)hue, (uint8_t)sat, (uint8_t)val);
}

static lv_obj_t *plain_obj(lv_obj_t *parent, int32_t w, int32_t h,
                           int32_t x, int32_t y, uint32_t color, int32_t radius)
{
    lv_obj_t *l = lv_obj_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, w, h);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_bg_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, radius, 0);
    return l;
}

void appfw_bars_create(appfw_bars_t *b, lv_obj_t *parent,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       int32_t pad, int32_t min_h, int n_bars)
{
    memset(b, 0, sizeof(*b));
    if (n_bars < 1) n_bars = 1;
    if (n_bars > APPFW_BARS_MAX) n_bars = APPFW_BARS_MAX;
    b->n_bars = n_bars;

    b->root = plain_obj(parent, w, h, x, y, 0x16202E, 8);
    lv_obj_set_scrollbar_mode(b->root, LV_SCROLLBAR_MODE_OFF);

    const int32_t top = 6, bot = 8;             // 柱子在底板内的上下留白
    b->baseline = h - bot;
    b->max_h = h - top - bot;
    const int32_t usable = w - pad * 2;
    b->slot = usable / n_bars;
    b->bar_w = b->slot - 1;                     // 槽间 1px 缝
    b->x0 = pad + (usable - b->slot * n_bars) / 2;

    for (int k = 0; k < n_bars; k++) {
        b->bar[k] = plain_obj(b->root, b->bar_w, min_h,
                              b->x0 + k * b->slot, b->baseline - min_h,
                              0x1F8C86, b->bar_w / 2);   // 胶囊形柱顶
    }
}

void appfw_bars_update(appfw_bars_t *b, const uint8_t *src, int n_src, uint8_t level)
{
    if (!b || !b->root) return;
    const int32_t min_h = 4;                    // 再矮也留 4px,一排都是小圆头
    if (b->rainbow) b->rainbow_hue = (uint16_t)((b->rainbow_hue + 4) % 360);

    for (int j = 0; j < b->n_bars; j++) {
        uint8_t lv = 0;
        if (src && n_src > 1) {
            // 柱 j 对应源数据的线性插值位置(柱数==源数时 u 恒等于 j)。
            const int32_t u = (int32_t)j * (n_src - 1) / (b->n_bars - 1);
            const int i0 = u;
            const int i1 = (i0 + 1 < n_src) ? i0 + 1 : i0;
            const int frac = u - i0;
            lv = (uint8_t)(src[i0] + ((int)src[i1] - (int)src[i0]) * frac / 255);
        } else if (src) {
            lv = src[0];
        }

        int32_t hgt = (int32_t)((int)lv * b->max_h / 255);
        if (hgt < min_h) hgt = min_h;
        if (hgt > b->max_h) hgt = b->max_h;

        lv_obj_set_size(b->bar[j], b->bar_w, hgt);
        lv_obj_set_pos(b->bar[j], b->x0 + j * b->slot, b->baseline - hgt);
        if (b->rainbow) {
            const uint16_t h = (uint16_t)((b->rainbow_hue + (uint32_t)j * 36) % 360);
            lv_obj_set_style_bg_color(b->bar[j], lv_color_hsv_to_rgb(h, 82, 80), 0);
        } else {
            lv_obj_set_style_bg_color(b->bar[j], bar_color(lv), 0);
        }
    }

    // 底板随总电平微微发亮,信号越强越"热"
    lv_obj_set_style_bg_color(b->root,
        lv_color_make((uint8_t)(22 + level / 20), (uint8_t)(32 + level / 16),
                      (uint8_t)(46 + level / 10)), 0);
}
