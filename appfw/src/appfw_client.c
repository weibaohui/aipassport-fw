// components/appfw/src/appfw_client.c —— 轮询 API 客户端框架实现,见 appfw_client.h。
#include "appfw_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "appfw_storage.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "appfw_net.h"

static const char *TAG = "appfw_client";

#define EV_REFRESH BIT0
#define RESPONSE_MAX (2 * 1024)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static appfw_client_err_t s_err = APPFW_CLIENT_IDLE;
static int s_http_status;
static int s_transport_err;
static int64_t s_fetch_epoch_s;
static bool s_last_ok;

static EventGroupHandle_t s_events;
static appfw_client_cfg_t s_cfg;

appfw_client_err_t appfw_client_last_err(void) { return s_err; }
int appfw_client_last_http_status(void) { return s_http_status; }
int appfw_client_transport_err(void) { return s_transport_err; }

static void publish_err(appfw_client_err_t err)
{
    time_t now = time(NULL);
    portENTER_CRITICAL(&s_lock);
    s_err = err;
    s_fetch_epoch_s = (int64_t)now;
    portEXIT_CRITICAL(&s_lock);
}

void appfw_client_get_state(appfw_client_err_t *err, int64_t *fetch_epoch_s, bool *last_ok)
{
    portENTER_CRITICAL(&s_lock);
    if (err) *err = s_err;
    if (fetch_epoch_s) *fetch_epoch_s = s_fetch_epoch_s;
    if (last_ok) *last_ok = (s_err == APPFW_CLIENT_OK);
    portEXIT_CRITICAL(&s_lock);
}

void appfw_client_refresh_now(void)
{
    if (s_events) xEventGroupSetBits(s_events, EV_REFRESH);
}

typedef struct { char *body; size_t max; size_t len; } body_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    body_ctx_t *ctx = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && evt->data_len > 0) {
        size_t copy = (size_t)evt->data_len;
        if (copy > ctx->max - ctx->len) copy = ctx->max - ctx->len;
        memcpy(ctx->body + ctx->len, evt->data, copy);
        ctx->len += copy;
    }
    return ESP_OK;
}

// 单次 HTTPS GET(框架内复用 + 导出给应用辅助端点)。
int appfw_client_fetch_once(const char *url, const char *auth,
                            const char (*names)[64], const char (*vals)[64], int n_extra,
                            int *http_status, char *body, size_t body_max, size_t *body_len)
{
    body_ctx_t ctx = { .body = body, .max = body_max, .len = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .event_handler = on_http_event,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "application/json");
    for (int i = 0; i < n_extra; i++) {
        esp_http_client_set_header(client, names[i], vals[i]);
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        *http_status = esp_http_client_get_status_code(client);
        body[ctx.len] = '\0';
        *body_len = ctx.len;
    }
    esp_http_client_cleanup(client);
    return err;
}

static bool s_time_synced;
static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
}

static bool ensure_time_synced(void)
{
    if (s_time_synced) return true;
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    for (int i = 0; i < 30 && !s_time_synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_time_synced) {
        esp_sntp_stop();
        return false;
    }
    return true;
}

static void client_task(void *arg)
{
    (void)arg;
    static char body[RESPONSE_MAX];
    static char url[256];
    static char auth[128];
    static char names[4][64];
    static char vals[4][64];

    for (;;) {
        bool online = false;
        for (int i = 0; i < 30; i++) {
            if (appfw_net_state() == APPFW_NET_ONLINE) { online = true; break; }
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (xEventGroupGetBits(s_events) & EV_REFRESH) break;
        }
        if (!online) {
            publish_err(APPFW_CLIENT_WAIT_NET);
            goto sleep_cycle;
        }

        if (!ensure_time_synced()) {
            ESP_LOGW(TAG, "SNTP 对时失败");
            publish_err(APPFW_CLIENT_WAIT_TIME);
            goto sleep_cycle;
        }

        {
            int n_extra = 0;
            bool go = s_cfg.make_request(url, sizeof(url), auth, sizeof(auth),
                                         names, vals, &n_extra, 4, s_cfg.user);
            if (!go) { publish_err(APPFW_CLIENT_WAIT_NET); goto sleep_cycle; }

            int status = 0;
            size_t blen = 0;
            esp_err_t err = appfw_client_fetch_once(url, auth, names, vals, n_extra,
                                                    &status, body, sizeof(body), &blen);
            time_t now = time(NULL);
            appfw_client_err_t ferr;
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "请求失败:%s", esp_err_to_name(err));
                portENTER_CRITICAL(&s_lock);
                s_transport_err = (int)err; s_http_status = 0;
                s_err = APPFW_CLIENT_HTTP; s_fetch_epoch_s = (int64_t)now;
                portEXIT_CRITICAL(&s_lock);
                ferr = APPFW_CLIENT_HTTP;
            } else {
                portENTER_CRITICAL(&s_lock);
                s_http_status = status; s_transport_err = 0;
                s_fetch_epoch_s = (int64_t)now;
                portEXIT_CRITICAL(&s_lock);
                ferr = (status == 401 || status == 403) ? APPFW_CLIENT_AUTH
                                                        : APPFW_CLIENT_OK;
                if (status != 200) ferr = APPFW_CLIENT_HTTP;
            }
            s_cfg.on_result(ferr, status, (int)err, body, blen, s_cfg.user);
        }

sleep_cycle:
        {
            uint16_t period_s = 60;
            appfw_store_get_period(&period_s);
            xEventGroupWaitBits(s_events, EV_REFRESH, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS((uint32_t)period_s * 1000));
        }
    }
    vTaskDelete(NULL);
}

int appfw_client_start(const appfw_client_cfg_t *cfg)
{
    if (!cfg || !cfg->make_request || !cfg->on_result) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(client_task, "appfw_client", 8192, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
