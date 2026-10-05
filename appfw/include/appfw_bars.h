// appfw/include/appfw_bars.h —— 柱阵显示控件(纯 LVGL,无 BSP/FreeRTOS 依赖)。
//
// 一排随输入值起伏的圆头柱 + HSV 色阶(越突出越暖) + 底板随总电平微亮。
// 上移自收音机播放页的频谱条(2026-10-05):柱阵怎么画是机制,数据从哪来
// (频谱/电量/任意指标)是调用方的事——本控件不知道"频谱"这个词。
#pragma once

#include <stdint.h>

#include "lvgl.h"

// 柱数上限(结构体内定长数组;C3 上每根柱一个 lv_obj + 本地样式,32 根曾把
// MP3 解码器挤出内存,应用侧自减柱数,框架只设上限)。
#define APPFW_BARS_MAX 32

typedef struct {
    lv_obj_t *root;            // 底板(随 level 微亮);挂在 parent 上
    // 几何(create 时算好;勿用 lv_obj_get_width() 读回——LVGL9 布局前
    // 读回是 0,会把宽度清成 0 直接消失,踩过)。
    int32_t baseline;          // 柱底边(柱子从这里往上长)
    int32_t max_h;             // 满格柱高
    int32_t slot;              // 每根柱横向步进
    int32_t bar_w;             // 柱宽
    int32_t x0;                // 第一根柱左边
    int n_bars;                // 实际柱数 ≤ APPFW_BARS_MAX
    lv_obj_t *bar[APPFW_BARS_MAX];
    bool rainbow;              // 彩虹模式:柱色相 = rainbow_hue + i*36,每帧推进
    uint16_t rainbow_hue;      //   (开=流动彩虹,关=默认青→黄高度色阶)
} appfw_bars_t;

// 在 parent 内创建柱阵:x/y/w/h 是底板几何,pad 为底板内边距(两侧对称,
// 柱子在剩余宽度里均分槽位),min_h 是最矮柱高,n_bars 为柱数。
void appfw_bars_create(appfw_bars_t *b, lv_obj_t *parent,
                       int32_t x, int32_t y, int32_t w, int32_t h,
                       int32_t pad, int32_t min_h, int n_bars);

// 更新柱阵。src 长度 n_src(可与柱数不同:线性插值映射,插值值不越过
// 两侧均值,不会造出假峰);src 为 NULL 时全部落底。level 0..255 驱动
// 底板辉光。
void appfw_bars_update(appfw_bars_t *b, const uint8_t *src, int n_src, uint8_t level);
