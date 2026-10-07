// components/appfw/include/appfw_portal.h —— 配网门户(通用)。
//
// 常驻 HTTP(TCP 80)+ captive DNS(UDP 53,仅 AP 阶段);两阶段网页:
// 阶段一(未联网)只显示连接 WiFi;阶段二(在线,经局域网 IP)显示框架设置
// (刷新/息屏)+ WiFi 管理 + 应用注入片段(<!--APP_CONFIG_HTML-->)。
// REST(全部 JSON):/api/status /api/scan(GET 触发后 GET 结果/POST 触发)
// /api/saved /api/networks(+add/delete)/api/connect /api/settings
// /api/config/export|import /api/clear;应用端点经 on_httpd_ready 注册。
#pragma once

#include <stdbool.h>
#include <stdint.h>

struct httpd_req;

// 门户构建/运行配置:应用注入点(HTML 片段、导入导出钩子、httpd 就绪回调)。
typedef struct {
    const char *(*app_config_html)(void);      // 阶段二页面注入片段(可 NULL)
    bool (*app_config_apply)(void *cjson_root); // 导入/保存:处理应用自有字段
    void (*app_config_fill)(void *cjson_obj);   // 状态/导出回显应用字段
    bool (*on_httpd_ready)(void *httpd);        // httpd 启动后注册应用端点
} appfw_prov_cfg_t;

// 配置注入(必须在首次 appfw_portal_start() 前调用;可空)。
void appfw_prov_configure(const appfw_prov_cfg_t *cfg);

// 启动 HTTP 服务(TCP 80)与 DNS 劫持(UDP 53,配网期)。幂等。
// 门户不再强制常驻:见 appfw_portal_stop / appfw_portal_touch。
bool appfw_portal_start(void);

// 启动前的内存腾挪钩子(弱符号,默认空):httpd 任务/控制块/路由表要 ~10KB,
// 应用可在此释放"播放预留"等大块,确保启动稳稳落地。
void appfw_portal_pre_start_hook(void);

// 整个门户下线(httpd 任务 + 控制块 + 套接字 + DNS),内存还给系统。
// 下次 start 原样重建(应用端点经 on_httpd_ready 重新注册)。幂等。
void appfw_portal_stop(void);

// 记录一次门户活动。框架处理器已在高频入口(index/status/export/POST JSON)
// 调用;应用自注册的处理器若也想给"空闲自动关闭"续命,照调即可。
void appfw_portal_touch(void);

// 门户在跑、且距最近一次活动超过 seconds 秒。空闲自动关闭的判据。
bool appfw_portal_idle_past(int seconds);

// 停止 DNS 劫持(联网后不再需要);HTTP 服务保持。幂等。
void appfw_portal_stop_dns(void);

// 门户 HTTP 当前是否在服务(用于自检与异常重启)。
bool appfw_portal_running(void);

// ---- 应用处理器复用的 JSON 助手 ----
void *appfw_prov_read_json(struct httpd_req *req); // 失败已回 400,返回 NULL
void appfw_prov_send_ok(struct httpd_req *req, bool ok);
