// components/appfw/include/appfw_client.h —— 轮询 API 客户端框架(通用)。
//
// 应用提供两个回调:make_request(每周期组装 URL/认证/附加头)与
// on_result(HTTP 结果交付,应用自行解析并保存快照)。框架负责:
// 等联网 → SNTP 对时(TLS 证书校验需要正确时间)→ HTTPS GET(证书包校验)→
// 交付 → 按配置周期(存储键 period_s,1~60 分钟)休眠,期间可被
// appfw_client_refresh_now() 提前唤醒。错误分类与最近传输错误由框架记录,
// 应用界面可读取展示。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 查询失败原因(应用界面据其显示文案)。
typedef enum {
    APPFW_CLIENT_IDLE = 0,   // 尚未开始(未联网/对时中)
    APPFW_CLIENT_OK,         // 最近一次成功
    APPFW_CLIENT_WAIT_NET,   // 尚未联网
    APPFW_CLIENT_WAIT_TIME,  // 时间未同步(SNTP 未成功)
    APPFW_CLIENT_AUTH,       // 服务端拒绝(401/403,或应用判定)
    APPFW_CLIENT_HTTP,       // HTTP/网络错误
    APPFW_CLIENT_PARSE,      // 响应解析失败(应用判定)
} appfw_client_err_t;

typedef struct {
    // 每周期调用一次:组装本轮请求。
    //   url            输出完整请求地址(含查询参数)
    //   auth           输出 Authorization 头的值(原样,框架不加 Bearer;
    //                  应用可自行带前缀)
    //   extra_names/extra_vals/extra_max  附加头(成对;最多 extra_max 组)
    //   n_extra        输出附加头组数(0 = 无)
    // 返回 false = 本轮跳过(框架按 WAIT_NET 处理并睡一个周期)。
    bool (*make_request)(char *url, size_t url_len,
                         char *auth, size_t auth_len,
                         char (*extra_names)[64], char (*extra_vals)[64],
                         int *n_extra, int extra_max,
                         void *user);
    // 请求完成交付:status 为 HTTP 状态码(err 为框架级错误时 status=0);
    // transport_err 为最近一次 esp_err_t(0=无);body/len 仅在本调用内有效。
    // 应用自行解析、保存快照并决定界面文案。
    void (*on_result)(appfw_client_err_t err, int status, int transport_err,
                      const char *body, size_t len, void *user);
    void *user;
} appfw_client_cfg_t;

// 启动后台查询任务(常驻)。返回 0 成功,否则 ESP_ERR 码。
int appfw_client_start(const appfw_client_cfg_t *cfg);

// 请求立即刷新一次(按键触发);下次循环提前执行,不阻塞调用者。
void appfw_client_refresh_now(void);

// 单次 HTTPS GET(辅助端点用,如主查询之外的列表接口):带证书包校验与
// 认证/附加头;成功返回 ESP_OK 并带出状态码与响应体(截断至 body_max)。
// 在客户端任务的回调上下文调用(会阻塞数秒)。
int appfw_client_fetch_once(const char *url, const char *auth,
                            const char (*names)[64], const char (*vals)[64], int n_extra,
                            int *http_status, char *body, size_t body_max, size_t *body_len);

// 最近一次框架级状态(应用界面显示用)。
void appfw_client_get_state(appfw_client_err_t *err, int64_t *fetch_epoch_s, bool *last_ok);
appfw_client_err_t appfw_client_last_err(void);
int appfw_client_last_http_status(void);
int appfw_client_transport_err(void);
