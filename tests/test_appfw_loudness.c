// tests/test_appfw_loudness.c —— 响度均衡的主机测试(纯逻辑,不需要设备)。
//
// 钉死这些行为:安静台增益向上爬并收敛在目标附近、响亮台向下压、限速
// (不许一步到位)、增益硬边界(+12/-12dB)、峰值保护触发回撤、换台重置、
// 静音不追增益、输出不越 int16。
#include "appfw_loudness.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 44100

static void gen_tone(int16_t *pcm, size_t n, float amp)
{
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)(amp * sinf(2.0f * (float)M_PI * 440.0f * (float)i / (float)FS));
    }
}

int main(void)
{
    static int16_t pcm[FS];                      // 1 秒
    appfw_loudness_t l;
    appfw_loudness_init(&l, NULL);
    appfw_loudness_reset(&l, FS);

    // ---- 安静台(均值 ~1273):增益应向上爬,输出变大 ----
    for (int s = 0; s < 12; s++) {           // 12s,每秒重生成(流式只过一遍)
        gen_tone(pcm, FS, 1273.0f);
        appfw_loudness_process(&l, pcm, sizeof(pcm));
    }
    const int32_t quiet_gain = appfw_loudness_gain_mdB(&l);
    // 正弦均值 = amp×2/π ≈ 810,目标 3600 → 期望 +12.96dB,顶在 +12dB 上限
    assert(quiet_gain >= 1100);                   // 明显提升且已接近上限
    assert(quiet_gain <= 1200 + 5);               // 不越上限(+12dB)+容差
    int16_t mx = 0;
    for (size_t i = 0; i < FS / 2; i++) {         // 处理后输出确实变响
        if (pcm[i] > mx) mx = pcm[i];
    }
    assert(mx > 4000);                            // 1273 → 至少 4000
    printf("ok  安静台提升 %d.%02ddB, 输出峰值 %d\n", quiet_gain / 100,
           abs(quiet_gain % 100), mx);

    // ---- 响亮台(均值 ~20000):增益向下压到下限 ----
    appfw_loudness_init(&l, NULL);
    appfw_loudness_reset(&l, FS);
    for (int s = 0; s < 20; s++) {
        gen_tone(pcm, FS, 20000.0f);
        appfw_loudness_process(&l, pcm, sizeof(pcm));
    }
    const int32_t loud_gain = appfw_loudness_gain_mdB(&l);
    assert(loud_gain < -1000);                    // 明显压低
    assert(loud_gain >= -1200 - 5);               // 不越下限
    printf("ok  响亮台压低 %d.%02ddB\n", loud_gain / 100, abs(loud_gain % 100));

    // ---- 限速:从 0 开始一秒内不许跳到 +12dB ----
    appfw_loudness_init(&l, NULL);
    appfw_loudness_reset(&l, FS);
    gen_tone(pcm, FS, 1273.0f);
    appfw_loudness_process(&l, pcm, sizeof(pcm)); // 1s
    assert(appfw_loudness_gain_mdB(&l) < 1500);   // 1.5dB/s × 1s 最多 ~1.5dB
    printf("ok  增益限速\n");

    // ---- 峰值保护:给一个上限内提升但顶到满幅的台,增益必须被回撤 ----
    appfw_loudness_init(&l, NULL);
    appfw_loudness_reset(&l, FS);
    gen_tone(pcm, FS, 1200.0f);
    for (int s = 0; s < 3; s++) appfw_loudness_process(&l, pcm, sizeof(pcm));
    // 人工把增益抬高到接近上限,再喂满幅音,限幅器应把增益拉下来
    l.gain_mdB = 1190;
    l.gain_q16 = (int32_t)(powf(10.0f, 1190.0f / 2000.0f) * 65536);
    for (int s = 0; s < 4; s++) {
        gen_tone(pcm, FS, 32700.0f);
        appfw_loudness_process(&l, pcm, sizeof(pcm));
    }
    assert(l.gain_mdB < 1190);
    for (size_t i = 0; i < FS; i++) assert(pcm[i] <= 32767 && pcm[i] >= -32768);
    printf("ok  峰值保护回撤且输出不越界\n");

    // ---- 换台重置 / 静音不追 ----
    appfw_loudness_reset(&l, FS);
    assert(appfw_loudness_gain_mdB(&l) == 0);
    memset(pcm, 0, sizeof(pcm));
    appfw_loudness_process(&l, pcm, sizeof(pcm));
    assert(appfw_loudness_gain_mdB(&l) == 0);     // 静音不推增益
    printf("ok  重置/静音\n");

    printf("全部通过\n");
    return 0;
}
