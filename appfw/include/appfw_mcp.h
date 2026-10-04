#pragma once
// appfw/include/appfw_mcp.h —— MCP(Model Context Protocol)服务器壳。
//
// 让局域网里的 AI 宿主(ZCode / Claude 等)通过标准协议操作设备:应用把
// 自己的能力注册成"工具"(名字 + 说明 + 参数说明 + 处理函数),本模块负责
// 协议本身——JSON-RPC 2.0、initialize 握手、tools/list 列举、tools/call
// 分发。框架对工具做什么一无所知(收音机注册点播,别的应用可以注册别的)。
//
// 传输:MCP 的 Streamable HTTP 单端点 POST /mcp(请求 JSON-RPC,响应普通
// JSON,不做 SSE);GET/DELETE 一律 405。无会话状态,天然配合门户"按需
// 开启、空闲自卸"的内存模型:门户不在,AI 就够不到设备。
//
// 默认关闭:应用调用 appfw_mcp_set_tools 注册了工具才视为开启(路由才会
// 注册)。这是 opt-in 哲学的延续。
#pragma once

#include <stdbool.h>
#include <stddef.h>

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
// 注册即视为开启 MCP 路由;传 NULL/0 = 关闭。
void appfw_mcp_set_tools(const appfw_mcp_tool_t *tools, int count);

// 服务器名/版本(initialize 握手回给 AI;有默认值,可不调)。
void appfw_mcp_set_server_info(const char *name, const char *version);

// 框架内部:门户 httpd 就绪后注册 /mcp 路由(未注册工具时是空操作)。
bool appfw_mcp_register(void *httpd);
