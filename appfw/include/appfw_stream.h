#pragma once
// appfw/include/appfw_stream.h —— 网络音频流传输源(自 aipassport-radio 上移)。
//
// 只管"把一条流地址变成连续的音频字节":http/https 连接、30x 重定向跟随、
// HLS(m3u8)段轮换与追新、ICY 元数据剥离、CDN 掐断后的 http 分身回退、
// WiFi 退出省电。解码、I2S 输出、内存编排是调用方的事。
//
// 行为约定(全部真机踩坑换来,勿删):
//   - https 流被 CDN 起流 1-2 秒后掐断是常态(同 URL 的 http 分身健康):
//     open 失败与 read 中断都会自动换 http 分身重试一次,调用方无感;
//   - HLS:首切最新段(直播)/顺播(点播),段尽换段,段与列表共用一条
//     keep-alive 连接;CDN 常在段后掐连接,必须回收再换段;连续 3 轮取
//     不到段判定整流死亡(返回 <0,调用方按故障重连);
//   - read() 返回的是剥完 ICY 的纯音频字节(就地压实,允许 buf==scratch);
//   - 暂停语义在调用方:继续 read 并丢弃即可,连接保持直播语义。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct appfw_stream appfw_stream_t;

typedef enum {
    APPFW_STREAM_ERR_NONE = 0,
    APPFW_STREAM_ERR_URL,      // 地址非法(非 http/https)
    APPFW_STREAM_ERR_CONNECT,  // 连接失败/超时(含分身回退后仍失败)
    APPFW_STREAM_ERR_HTTP,     // 服务端未返回 2xx
} appfw_stream_err_t;

typedef struct {
    const char *url;                    // 初始地址;https 会自动尝试 http 分身
    char *playlist;                     // HLS 播放列表文本缓冲(≥1024;直链可空)
    size_t playlist_cap;
    void (*on_title)(void *user, const char *title);  // ICY 曲名(仅非空时回调)
    void *user;
} appfw_stream_cfg_t;

// 连接并校验流。返回 ERR_NONE 后 read 可用。WiFi 省电在此关闭(播音期间
// modem sleep 会把突发收包压到几 KB/s,HLS 直播段必断)。
appfw_stream_err_t appfw_stream_open(appfw_stream_t **out, const appfw_stream_cfg_t *cfg);

// 读纯音频字节(ICY 已剥离)。>0=字节数;0=服务端暂时停推(HLS 新段已就绪
// 或 http 分身刚重连,再读一次即可);<0=流已死(调用方按故障收尾)。
int appfw_stream_read(appfw_stream_t *s, uint8_t *buf, int cap);

// 本台是否 HLS(调用方据此选解码器与抖动桶深)。
bool appfw_stream_is_hls(const appfw_stream_t *s);

// 实际生效的地址(可能已是 http 分身;只读,生命周期同句柄)。
const char *appfw_stream_url(const appfw_stream_t *s);

void appfw_stream_close(appfw_stream_t *s);
