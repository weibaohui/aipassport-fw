// appfw/src/appfw_loudness.c —— 见 appfw_loudness.h。
#include "appfw_loudness.h"

#include <math.h>
#include <string.h>

// 输出接近满幅的门槛(≈-0.8dBFS):碰到就立即回撤增益。
#define LIMITER_KNEE 29000
#define LIMITER_PULLDOWN_MD100 300   // 每次回撤 3dB
#define GAIN_Q16_ONE (1 << 16)

static int32_t clamp_gain(const appfw_loudness_cfg_t *c, int32_t mdB)
{
    if (mdB > c->boost_mdB) mdB = c->boost_mdB;
    if (mdB < c->cut_mdB) mdB = c->cut_mdB;
    return mdB;
}

void appfw_loudness_init(appfw_loudness_t *l, const appfw_loudness_cfg_t *cfg)
{
    memset(l, 0, sizeof(*l));
    l->cfg = cfg ? *cfg : (appfw_loudness_cfg_t){
        .target_mabs = 3600, .boost_mdB = 1200, .cut_mdB = -1200,
        .slew_mdBps = 150, .block_ms = 100,
    };
    l->gain_q16 = GAIN_Q16_ONE;
    l->block_samples = l->cfg.block_ms;   // 无采样率时的占位,reset 会重算
}

void appfw_loudness_reset(appfw_loudness_t *l, uint32_t sample_rate)
{
    if (sample_rate < 8000) sample_rate = 44100;
    l->gain_mdB = 0;
    l->gain_q16 = GAIN_Q16_ONE;
    l->block_samples = (uint32_t)((uint64_t)sample_rate * l->cfg.block_ms / 1000);
    if (l->block_samples < 16) l->block_samples = 16;
    l->fill = 0;
    l->abssum = 0;
    l->block_peak = 0;
}

void appfw_loudness_process(appfw_loudness_t *l, int16_t *pcm, size_t bytes)
{
    if (!l || !pcm || bytes < 2) return;
    size_t i = 0;
    const size_t n = bytes / 2;

    while (i < n) {
        // 分段处理:凑满一个测量块就结算一次——任意调用粒度(整帧/整秒)
        // 与逐块喂行为严格一致。整秒喂曾经把 10 块的 |采样| 累加除以
        // 1 块的样本数,mean 虚大 10 倍,增益决策反向(真机踩过)。
        const size_t take = n - i < l->block_samples - l->fill
                                ? n - i : l->block_samples - l->fill;
        const int32_t g = l->gain_q16;
        int32_t peak = 0;
        for (size_t k = i; k < i + take; k++) {
            const int32_t s = pcm[k];
            const int32_t a = (s >= 0) ? s : -s;
            if (a > peak) peak = a;
            l->abssum += a;
            if (g != GAIN_Q16_ONE) {
                int32_t o = (s * g) >> 16;
                if (o > 32767) o = 32767;
                else if (o < -32768) o = -32768;
                const int32_t oa = (o >= 0) ? o : -o;
                if (oa > peak) peak = oa;
                pcm[k] = (int16_t)o;
            }
        }
        if (peak > l->block_peak) l->block_peak = peak;
        l->fill += (uint32_t)take;
        i += take;

        if (l->fill >= l->block_samples) {
            // ---- 块结束:测响度 → 调增益 ----
            l->fill = 0;
            const int32_t mean = (int32_t)(l->abssum / (int64_t)l->block_samples);
            l->abssum = 0;
            const int32_t blk_peak = l->block_peak;   // 先取后清:清零放前面
            l->block_peak = 0;                        // 限幅器会跨块永久误触发

            if (blk_peak > LIMITER_KNEE && l->gain_mdB > 0) {
                // 限幅器快放:输出顶到 knee,立刻回撤(逐块 3dB,不用等慢速适应)。
                l->gain_mdB = clamp_gain(&l->cfg, l->gain_mdB - LIMITER_PULLDOWN_MD100);
            } else if (mean > 50) {
                // 慢速均衡:朝 target/mean 的对数目标挪,限速。静音(mean≈0)不追。
                const float desired = 2000.0f * log10f((float)l->cfg.target_mabs / (float)mean);
                const float cur = (float)l->gain_mdB;
                float step = desired - cur;
                const float max_step = (float)l->cfg.slew_mdBps * (float)l->cfg.block_ms / 1000.0f;
                if (step > max_step) step = max_step;
                else if (step < -max_step) step = -max_step;
                l->gain_mdB = clamp_gain(&l->cfg, l->gain_mdB + (int32_t)step);
            }
            l->gain_q16 = (int32_t)(powf(10.0f, (float)l->gain_mdB / 2000.0f) * GAIN_Q16_ONE);
        }
    }
}

int32_t appfw_loudness_gain_mdB(const appfw_loudness_t *l)
{
    return l ? l->gain_mdB : 0;
}
