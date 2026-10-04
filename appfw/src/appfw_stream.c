// appfw/src/appfw_stream.c —— 见 appfw_stream.h(自 aipassport-radio 上移)。
#include "appfw_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "appfw_hls.h"
#include "appfw_icy.h"

static const char *TAG = "appfw_stream";

#define APPFW_STREAM_URL_CAP 256

struct appfw_stream {
    esp_http_client_handle_t client;
    char url[APPFW_STREAM_URL_CAP];     // 实际生效地址(可能已是 http 分身)
    appfw_stream_err_t err;
    bool twin_tried;                    // http 分身已试过(每条流一次)

    bool hls;
    bool seg_open;                      // 当前段请求在读
    bool started;                       // 已切入首段
    bool vod_done;                      // 点播放完
    uint64_t last_seq;                  // 已切出的最后一段序号
    char *playlist; size_t playlist_cap;

    uint32_t metaint_seen;              // 响应头探针结果(事件回调写入)
    appfw_icy_t icy;                    // ICY 解复用(metaint=0 时不启用)
    void (*on_title)(void *user, const char *title);
    void *user;
};

// ---- 响应头探针:icy-metaint 只能经 HTTP_EVENT_ON_HEADER 拿到 ----
// (esp_http_client_get_header 读的是请求头,拿不到响应头——参考固件踩过。)
static esp_err_t metaint_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == NULL) return ESP_OK;
    if (evt->header_key == NULL || evt->header_value == NULL) return ESP_OK;
    if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        ((appfw_stream_t *)evt->user_data)->metaint_seen =
            (uint32_t)strtoul(evt->header_value, NULL, 10);
    }
    return ESP_OK;
}

// 取一个 URL 的响应体到 buf(截断到 cap)。返回长度;-1 失败。
// keep-alive 连接在请求间复用。HLS 取列表/变体流专用。
static int hls_fetch(esp_http_client_handle_t client, const char *url,
                     uint8_t *buf, size_t cap)
{
    esp_http_client_set_url(client, url);
    // 手工跟随 30x(≤5 跳):infomaniak 这类 CDN 会把流 302 到别的域名/端口,
    // 且 Location 的 scheme 是大写 "HTTP://",esp_http_client 不自动跟。
    int status = 0;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) return -1;
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status < 300 || status >= 400) break;
        char *loc = NULL;
        esp_http_client_get_header(client, "Location", &loc);
        if (!loc || !loc[0]) break;
        ESP_LOGI(TAG, "HLS 跟随重定向(%d): %s", status, loc);
        esp_http_client_set_url(client, loc);
        esp_http_client_close(client);
    }
    if (status < 200 || status >= 300) return -1;
    size_t used = 0;
    while (used < cap) {
        const int n = esp_http_client_read(client, (char *)buf + used, (int)(cap - used));
        if (n > 0) { used += (size_t)n; continue; }
        break;                     // 读完(0)或出错(-1,按截断处理)
    }
    buf[used ? used - 1 : 0] = '\0';
    return (int)used;
}

// 换到下一段:刷播放列表 → 挑段 → open。返回 1=已开新段,0=点播放完,-1=故障。
static int hls_advance(appfw_stream_t *s)
{
    int depth = 0;                 // 变体流(m3u8 套 m3u8)最多展开一层
    for (int attempt = 0; attempt < 20; attempt++) {
        if (hls_fetch(s->client, s->url, (uint8_t *)s->playlist, s->playlist_cap) <= 0) {
            ESP_LOGW(TAG, "[hls] 取列表失败 attempt=%d", attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        appfw_hls_pick_t pick;
        const bool ok = appfw_hls_pick_segment(s->playlist, s->url,
                                               !s->started,
                                               s->started ? s->last_seq : UINT64_MAX,
                                               &pick);
        if (!ok) {
            if (pick.endlist && s->started) { s->vod_done = true; return 0; }
            ESP_LOGW(TAG, "[hls] 无新段 last_seq=%llu attempt=%d",
                     (unsigned long long)s->last_seq, attempt);
            vTaskDelay(pdMS_TO_TICKS(1000));   // 直播追新:每秒重查一次列表
            continue;
        }
        if (pick.is_playlist) {
            if (++depth > 2) return -1;
            snprintf(s->url, sizeof(s->url), "%s", pick.seg_url);
            continue;
        }
        // 点播收尾:ENDLIST 列表的"最新段"已经放过了,就是放完了。
        esp_http_client_set_url(s->client, pick.seg_url);
        if (esp_http_client_open(s->client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "[hls] 段 open 失败:%.60s", pick.seg_url);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        (void)esp_http_client_fetch_headers(s->client);
        const int status = esp_http_client_get_status_code(s->client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "[hls] 段 HTTP %d:%.60s", status, pick.seg_url);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        ESP_LOGI(TAG, "[hls] 段就绪 seq=%llu:%.60s",
                 (unsigned long long)pick.media_seq, pick.seg_url);
        s->seg_open = true;
        s->started = true;
        s->last_seq = pick.media_seq;
        return 1;
    }
    return -1;
}

// 建客户端并(直链时)打开流、校验状态;成功即初始化 ICY。HLS 只建客户端
// ——首个响应是播放列表文本,留给 hls_advance 统一管理。
static appfw_stream_err_t try_connect(appfw_stream_t *s, const char *url)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .user_agent = "AI-Passport-Radio/1.0",   // 别伪装浏览器:有的 CDN 对
                                                 // 浏览器 UA 会在 32KB 处掐断
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
        .event_handler = metaint_cb,
        .user_data = s,
        .crt_bundle_attach = (strncmp(url, "https://", 8) == 0)
                                 ? esp_crt_bundle_attach : NULL,
    };
    if (s->client) {
        esp_http_client_cleanup(s->client);
        s->client = NULL;
    }
    s->client = esp_http_client_init(&cfg);
    if (!s->client) return APPFW_STREAM_ERR_CONNECT;
    s->metaint_seen = 0;
    if (s->hls) return APPFW_STREAM_ERR_NONE;

    int status = 0;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(s->client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "打开流失败: %s", url);
            return APPFW_STREAM_ERR_CONNECT;
        }
        (void)esp_http_client_fetch_headers(s->client);
        status = esp_http_client_get_status_code(s->client);
        if (status < 300 || status >= 400) break;
        char *loc = NULL;
        esp_http_client_get_header(s->client, "Location", &loc);
        if (!loc || !loc[0]) break;
        ESP_LOGI(TAG, "跟随重定向(%d): %s", status, loc);
        esp_http_client_set_url(s->client, loc);
        esp_http_client_close(s->client);
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP 状态码 %d", status);
        return APPFW_STREAM_ERR_HTTP;
    }
    appfw_icy_init(&s->icy, s->metaint_seen);
    ESP_LOGI(TAG, "已连接, icy-metaint=%u", (unsigned)s->metaint_seen);
    return APPFW_STREAM_ERR_NONE;
}

appfw_stream_err_t appfw_stream_open(appfw_stream_t **out, const appfw_stream_cfg_t *cfg)
{
    if (!out || !cfg || !cfg->url) return APPFW_STREAM_ERR_URL;
    if (strncmp(cfg->url, "http://", 7) != 0 &&
        strncmp(cfg->url, "https://", 8) != 0) {
        return APPFW_STREAM_ERR_URL;
    }
    if (cfg->playlist_cap > 0 && cfg->playlist_cap < 1024) return APPFW_STREAM_ERR_URL;

    appfw_stream_t *s = calloc(1, sizeof(*s));
    if (!s) return APPFW_STREAM_ERR_CONNECT;
    snprintf(s->url, sizeof(s->url), "%s", cfg->url);
    s->playlist = cfg->playlist;
    s->playlist_cap = cfg->playlist_cap;
    s->on_title = cfg->on_title;
    s->user = cfg->user;
    s->hls = appfw_hls_is_playlist_url(s->url);
    if (s->hls && !s->playlist) {
        free(s);
        return APPFW_STREAM_ERR_URL;
    }

    // WiFi 退出省电(modem sleep):内存紧张时 PS 模式的突发收包会被压到
    // 几 KB/s,HLS 直播流(段 200KB/10s)必断。播音期间不需要省电。
    esp_wifi_set_ps(WIFI_PS_NONE);

    // https 流被 CDN 掐是常态:open 失败先换 http 分身再定输赢。
    appfw_stream_err_t err = try_connect(s, s->url);
    if (err != APPFW_STREAM_ERR_NONE && !s->twin_tried &&
        strncmp(s->url, "https://", 8) == 0) {
        s->twin_tried = true;
        memmove(s->url + 7, s->url + 8, strlen(s->url) - 8 + 1);
        memcpy(s->url, "http://", 7);
        ESP_LOGI(TAG, "https 打不开,换 http 分身:%s", s->url);
        err = try_connect(s, s->url);
    }
    if (err != APPFW_STREAM_ERR_NONE) {
        if (s->client) esp_http_client_cleanup(s->client);
        free(s);
        return err;
    }
    *out = s;
    return APPFW_STREAM_ERR_NONE;
}

// 原始字节:直链一次读;HLS 完成段轮换(段尽回收连接换段、追新、3 轮取不
// 到段判定整流死亡)。返回 >0 字节;0=暂时无数据;<0=流已死。
static int raw_read(appfw_stream_t *s, uint8_t *buf, int cap)
{
    if (!s->hls) {
        return esp_http_client_read(s->client, (char *)buf, cap);
    }
    int stalls = 0;                // 连续取不到新段(含取列表失败)的轮数
    for (;;) {
        if (s->seg_open) {
            const int n = esp_http_client_read(s->client, (char *)buf, cap);
            if (n > 0) return n;
            ESP_LOGI(TAG, "[hls] 段结束(%d),回收连接换段", n);
            s->seg_open = false;
            esp_http_client_close(s->client);   // CDN 常在段后掐断 keep-alive:
        }                                      // 不回收,后续 open 全在死连接上失败
        if (s->vod_done) return 0;
        const int adv = hls_advance(s);
        if (adv > 0) { stalls = 0; continue; }
        if (s->vod_done) return 0;
        if (++stalls >= 3) {
            ESP_LOGW(TAG, "[hls] 连续 %d 轮取段失败,整流重启", stalls);
            return -1;
        }
    }
}

int appfw_stream_read(appfw_stream_t *s, uint8_t *buf, int cap)
{
    if (!s || !buf || cap <= 0) return -1;
    const int raw = raw_read(s, buf, cap);
    if (raw <= 0) {
        // 读死亡(含 TLS 被 CDN 掐):https 换 http 分身重连一次。errno=
        // Success 的 TLS 干净关闭在 qtfm 系 CDN 上是常态,分身普遍健康。
        if (raw < 0 && !s->twin_tried && strncmp(s->url, "https://", 8) == 0) {
            s->twin_tried = true;
            memmove(s->url + 7, s->url + 8, strlen(s->url) - 8 + 1);
            memcpy(s->url, "http://", 7);
            ESP_LOGW(TAG, "流中断,换 http 分身:%s", s->url);
            if (try_connect(s, s->url) == APPFW_STREAM_ERR_NONE) return 0;
        }
        return raw;
    }
    if (s->icy.metaint == 0) return raw;

    // ICY 剥离:元数据字节就地压实,写位不超前于读位,buf 原地压实安全。
    size_t alen = 0;
    for (int i = 0; i < raw; i++) {
        if (appfw_icy_consume(&s->icy, buf[i]) == APPFW_ICY_AUDIO) {
            buf[alen++] = buf[i];
        }
    }
    const char *t = appfw_icy_title(&s->icy);
    if (t[0] && s->on_title) s->on_title(s->user, t);
    return (int)alen;
}

bool appfw_stream_is_hls(const appfw_stream_t *s) { return s && s->hls; }

const char *appfw_stream_url(const appfw_stream_t *s) { return s ? s->url : ""; }

void appfw_stream_close(appfw_stream_t *s)
{
    if (!s) return;
    if (s->client) esp_http_client_cleanup(s->client);
    free(s);
}
