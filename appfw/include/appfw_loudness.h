// appfw/include/appfw_loudness.h —— 响度均衡(纯逻辑,无 LVGL/FreeRTOS/BSP 依赖)。
//
// 目的:不同电台的广播链路增益不同,同样的音量档位有的台震耳有的台要贴耳。
// 本模块在解码后的 PCM 上施加一个**慢速**数字增益:逐块测量平均幅度,
// 以每秒 ≤slew dB 的速度把响度拉向目标——慢到只抹平台与台之间的差,
// 不碰节目本身的强弱起伏(没有压缩器的"呼吸感")。
//
// 纯定点热路径:每采样一次 32×32 乘 + 一次移位;每块一次 powf/log10f。
// 峰值保护:输出接近满幅时立即回撤增益(快放慢收的经典限幅策略)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t target_mabs;   // 目标平均幅度 0..32767(默认 3600 ≈ -19dBFS,典型广播)
    int32_t  boost_mdB;     // 增益上限(默认 +1200 = +12dB;再大会连底噪一起放大)
    int32_t  cut_mdB;       // 增益下限(默认 -1200)
    int32_t  slew_mdBps;    // 增益变化限速(默认 150 = 1.5dB/s)
    uint16_t block_ms;      // 测量块长(默认 100ms)
} appfw_loudness_cfg_t;

typedef struct {
    appfw_loudness_cfg_t cfg;
    int32_t  gain_mdB;      // 当前增益(0 = 不动)
    int32_t  gain_q16;      // 当前增益的 Q16 定点(powf 每块只算一次)
    uint32_t block_samples; // 一个测量块有多少采样
    uint32_t fill;          // 当前块已累积的采样数
    int64_t  abssum;        // 当前块 |采样| 累加(int64:块内 4.8e8 封不住 int32 溢出保险)
    int32_t  block_peak;    // 当前块输出峰值(限幅器用)
} appfw_loudness_t;

// 初始化;cfg 传 NULL 用默认参数。
void appfw_loudness_init(appfw_loudness_t *l, const appfw_loudness_cfg_t *cfg);

// 换台/重开流时调用:增益归零、测量重新开始(每个台独立适应)。
// sample_rate 用于把 block_ms 换算成块采样数。
void appfw_loudness_reset(appfw_loudness_t *l, uint32_t sample_rate);

// 原地处理一段 16bit PCM(声道无关,逐采样同增益)。bytes 须为偶数。
void appfw_loudness_process(appfw_loudness_t *l, int16_t *pcm, size_t bytes);

// 当前增益(mdB,诊断/日志用)。
int32_t appfw_loudness_gain_mdB(const appfw_loudness_t *l);
