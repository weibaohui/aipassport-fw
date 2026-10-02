// components/appfw/include/appfw_portal.h —— 配网门户(通用)。
//
// 常驻 HTTP(TCP 80)+ captive DNS(UDP 53,仅 AP 阶段);两阶段网页:
// 阶段一(未联网)只显示连接 WiFi;阶段二(在线,经局域网 IP)显示框架设置
// (刷新/熄屏)+ WiFi 管理 + 应用注入片段(<!--APP_CONFIG_HTML-->)。
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

// 启动 HTTP 服务(TCP 80,常驻)与 DNS 劫持(UDP 53,配网期)。幂等。
bool appfw_portal_start(void);

// 停止 DNS 劫持(联网后不再需要);HTTP 服务保持。幂等。
void appfw_portal_stop_dns(void);

// 门户 HTTP 当前是否在服务(用于自检与异常重启)。
bool appfw_portal_running(void);

// ---- 应用处理器复用的 JSON 助手 ----
void *appfw_prov_read_json(struct httpd_req *req); // 失败已回 400,返回 NULL
void appfw_prov_send_ok(struct httpd_req *req, bool ok);


// 把当前配置 JSON 同时写入 files 分区(导出 = 下载 + 设备本地留存)。
// 由导出处理器内部调用,无需应用关心。

// 文件管理(/files 页面与 /api/files/* 端点;框架内置,需开锁)。幂等。
bool appfw_files_register(void *httpd);
