#pragma once
// appfw/include/appfw_mcp.h —— MCP(Model Context Protocol)服务器。
//
// 让局域网里的 AI 宿主(ZCode / Claude 等)通过标准协议操作设备:应用把
// 自己的能力注册成"工具"(名字 + 说明 + 参数说明 + 处理函数),本模块负责
// 协议本身——JSON-RPC 2.0、initialize 握手、tools/list 列举、tools/call
// 分发。框架对工具做什么一无所知(收音机注册点播,别的应用可以注册别的)。
//
// 传输:常驻极简 TCP 服务(appfw_mcp_srv.c,端口 8080),单端点 POST /mcp,
// 请求 JSON-RPC、响应普通 JSON(不做 SSE/会话),响应一律 Connection: close。
// 刻意不用 esp_http_server:那套解析器/任务栈/控制块对一个单端点 JSON 服务
// 是纯浪费,常驻内存要多付近一倍。配网门户(80 端口)只管配网,与 AI 无关。
//
// 默认关闭:应用调用 appfw_mcp_set_tools 注册了工具才视为开启(UI 初始化时
// 自动拉起常驻服务);没注册工具就一分钱内存不花。这是 opt-in 哲学的延续。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

// 工具处理结果文本缓冲上限(单个工具的回话文本;超出截断)。
#define APPFW_MCP_TEXT_CAP 1024

typedef struct {
    char text[APPFW_MCP_TEXT_CAP];  // 给 AI 看的结果文本
    size_t len;
    bool is_error;                  // true = 工具执行失败(文本里说明原因)
} appfw_mcp_resp_t;

// 结果追加(printf 风格)。超出缓冲自动截断。
void appfw_mcp_resp_addf(appfw_mcp_resp_t *resp, const char *fmt, ...)
#ifndef __cplusplus
    __attribute__((format(printf, 2, 3)))
#endif
    ;

typedef struct {
    const char *name;        // 工具名(AI 调用时引用;英文、下划线)
    const char *description; // 一句话说明 AI 什么时候该用它、怎么用
    const char *input_schema;// 参数说明(JSON Schema 字符串,原样给 AI)
    // 处理函数:args 是调用参数(cJSON 对象,可能为 NULL);往 resp 写结果
    // 文本;返回 0=成功,非 0=执行失败(is_error 置位)。
    int (*handler)(cJSON *args, appfw_mcp_resp_t *resp);
} appfw_mcp_tool_t;

// 应用注册工具表(数组生命周期须与运行期一致,建议 static const)。
void appfw_mcp_set_tools(const appfw_mcp_tool_t *tools, int count);

// 按使能位自动挂载框架内置功能的等价工具(appfw_menu_item_t 同一套位):
//   SCREEN_OFF → set_screen_off(熄屏时间档位,无参=查询)
//   REFRESH    → set_refresh_period(刷新周期档位,无参=查询)
//   WIFI       → wifi_status / wifi_connect_saved(连已存热点)
//   INFO       → get_device_info(固件/内存/运行时长)
//   PROV       → get_provisioning_status(配网状态/IP/客户端数)
//   AI_ADMIN   → 不挂工具(AI 管理页是纯信息页,AI 本来就在用 MCP)
// 使能位没开的项不挂载对应工具——配置与 AI 能力严格一致。应用工具
// (set_tools)在前,内置工具在后。
void appfw_mcp_set_builtin_tools(unsigned menu_show_mask);

// 服务器名/版本(initialize 握手回给 AI;有默认值,可不调)。
void appfw_mcp_set_server_info(const char *name, const char *version);

// 框架版本(构建期 git describe;拿不到 git 时为 "unknown")。
const char *appfw_framework_version(void);

// 已注册工具数(应用 + 内置;0 = 未开启)。
int appfw_mcp_tool_count(void);

// 处理一条 JSON-RPC 请求文本,返回应答 cJSON(调用方负责打印后 cJSON_Delete)。
// 通知(无 id)返回 NULL 且 *status=202;解析失败返回 -32700 错误对象。
cJSON *appfw_mcp_handle(const char *body, size_t len, int *status);

// 屏幕亮度执行器注入(set_brightness 工具用;appfw_ui_init 自动注入,
// 应用无需关心)。不注入时工具回"设备未接屏幕"。
void appfw_mcp_set_brightness_apply(void (*fn)(uint8_t pct));

// ---- 诊断工具段(网络日志配套;appfw_netlog_init 时自动挂载) ----
// get_recent_logs(取最近日志)/ set_log_level(远程调日志级别)/
// set_netlog(配置 UDP 推送)。不 init 网络日志则这组工具不存在。
void appfw_mcp_diag_tools_enable(bool on);

// ---- 常驻服务(极简 TCP,端口 8080) ----
// 启动监听任务(幂等)。空转只付一个任务栈;请求期间 cJSON 树为瞬时堆。
void appfw_mcp_server_start(void);
bool appfw_mcp_server_running(void);
int appfw_mcp_server_port(void);
