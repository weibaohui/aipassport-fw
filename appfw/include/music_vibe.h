// appfw/include/music_vibe.h —— 音乐律动可视化:LED 电平表 + 对称频谱(零 FFT)。
//
// 数据源是音符事件而非音频分析:音序器本来就知道在放哪个音,pitch 就是
// 频率真值。频率→10 频段的映射、衰减、峰帽全部定点整数,零浮点热路径。
//
// 两个 page 共用同一份 band 数据,渲染对象各自在 init 一次建好、每帧只改
// 几何(属性差分:值不变不调 setter——这比一切算法优化都省)。渲染铁律:
// 不删建对象、不 alpha 混合、接受 20fps;由宿主唯一的 50ms lv_timer 调
// tick,禁止另起 timer/anim。
//
// 页面挂活动屏幕,init 后默认全隐藏——宿主经 music_vibe_enable 按位打开
// (bit0=A LED 表,bit1=B 对称频谱)。A 与 B 的几何是为"独立使用"设计的,
// 同时打开视觉上会叠,建议互斥使用。
//
// 纯 C,不依赖 ESP-IDF 内部头;主机单测用 -DMUSIC_VIBE_NO_LVGL 编译
// (LVGL setter 被替换为调用记录,可断言差分行为)。
#pragma once

#include <stdint.h>

// 创建对象(挂活动屏幕,全部隐藏)并生成颜色 LUT。幂等:重复调用是空操作。
void music_vibe_init(void);

// 一个音符事件(和弦 = 对每个 pitch 各调一次)。
// pitch:MIDI 音高;b = clamp((pitch-36)/4, 0, 9)。velocity:MIDI 0..127。
void music_vibe_note_on(uint8_t pitch, uint8_t velocity);

// 每帧驱动(宿主唯一的 50ms lv_timer 调用):band ×0.88/帧,峰帽 -0.55/s,
// 然后把变化落到启用的 page(属性差分,无变化的 setter 一个都不调)。
void music_vibe_tick(uint32_t dt_ms);

// 页面开关:bit0 = A LED 电平表,bit1 = B 对称频谱;0 = 全隐藏。
void music_vibe_enable(uint8_t page_mask);

// ---- 主机单测钩子(仅 -DMUSIC_VIBE_TEST 时存在) ----
#ifdef MUSIC_VIBE_TEST
uint16_t music_vibe_test_band(uint8_t i);        // q8 (0..256)
uint16_t music_vibe_test_cap(uint8_t i);         // q8
uint16_t music_vibe_test_lut(uint8_t i);         // RGB565
// 最近一次 tick 的渲染 setter 记录数(差分测试:值不变应计 0)。
uint32_t music_vibe_test_setters(void);
#endif
