// appfw/src/appfw_mcp.c —— 见 appfw_mcp.h(协议核:工具表 + JSON-RPC 分发;
// 传输在 appfw_mcp_srv.c)。
#include "appfw_mcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "appfw_client.h"
#include "appfw_net.h"
#include "appfw_storage.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "appfw_mcp";

static const appfw_mcp_tool_t *s_tools;      // 应用工具表
static int s_tool_count;
static const appfw_mcp_tool_t *s_bi_tools;   // 框架内置功能工具表(按使能位)
static int s_bi_count;
static const char *s_srv_name = "ai-passport";
static void (*s_brightness_apply)(uint8_t);   // UI 注入的背光执行器
static const char *s_srv_ver = "2.0-marker";

void appfw_mcp_set_tools(const appfw_mcp_tool_t *tools, int count)
{
    s_tools = tools;
    s_tool_count = count;
}

int appfw_mcp_tool_count(void)
{
    return s_tool_count + s_bi_count;
}

void appfw_mcp_set_brightness_apply(void (*fn)(uint8_t pct))
{
    s_brightness_apply = fn;
}

void appfw_mcp_set_server_info(const char *name, const char *version)
{
    if (name) s_srv_name = name;
    if (version) s_srv_ver = version;
}

void appfw_mcp_resp_addf(appfw_mcp_resp_t *r, const char *fmt, ...)
{
    if (!r || r->len >= APPFW_MCP_TEXT_CAP - 1) return;
    va_list ap;
    va_start(ap, fmt);
    r->len += (size_t)vsnprintf(r->text + r->len,
                                APPFW_MCP_TEXT_CAP - r->len, fmt, ap);
    va_end(ap);
    if (r->len >= APPFW_MCP_TEXT_CAP) r->len = APPFW_MCP_TEXT_CAP - 1;
}

// JSON-RPC 错误回复(code:-32700 解析/-32601 方法不存在/-32602 参数错)。
static cJSON *build_error(const cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    return resp;
}

static const appfw_mcp_tool_t *tool_at(int i)
{
    return i < s_tool_count ? &s_tools[i] : &s_bi_tools[i - s_tool_count];
}

static const appfw_mcp_tool_t *find_tool(const char *name)
{
    const int total = s_tool_count + s_bi_count;
    for (int i = 0; i < total; i++) {
        const appfw_mcp_tool_t *t = tool_at(i);
        if (strcmp(t->name, name) == 0) return t;
    }
    return NULL;
}

static cJSON *wrap_result(const cJSON *id, cJSON *result)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(r, "result", result);
    return r;
}

// tools/call:取工具 → 跑处理函数 → 包成 MCP content。
static cJSON *do_tool_call(const cJSON *id, const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        return build_error(id, -32602, "missing tool name");
    }
    const appfw_mcp_tool_t *tool = find_tool(name->valuestring);
    if (!tool) {
        return build_error(id, -32602, "unknown tool");
    }
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

    appfw_mcp_resp_t resp = { 0 };
    const int rc = tool->handler((cJSON *)args, &resp);
    resp.text[resp.len < APPFW_MCP_TEXT_CAP ? resp.len
                                            : APPFW_MCP_TEXT_CAP - 1] = '\0';

    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", resp.text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", rc != 0 || resp.is_error);
    return wrap_result(id, result);
}

// tools/list:走注册表。schema 是 const JSON 字符串,逐个 parse 挂上。
static cJSON *do_tools_list(const cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");
    const int total = s_tool_count + s_bi_count;
    for (int i = 0; i < total; i++) {
        const appfw_mcp_tool_t *tool = tool_at(i);
        cJSON *t = cJSON_AddObjectToObject(tools, "");
        cJSON_AddStringToObject(t, "name", tool->name);
        cJSON_AddStringToObject(t, "description", tool->description);
        cJSON *schema = cJSON_Parse(tool->input_schema);
        if (schema) cJSON_AddItemToObject(t, "inputSchema", schema);
        else cJSON_AddObjectToObject(t, "inputSchema");
    }
    return wrap_result(id, result);
}

// initialize:回协议版本(兼容则回显客户端请求的)+ 能力 + 服务器信息。
static cJSON *do_initialize(const cJSON *id, const cJSON *params)
{
    const char *version = "2025-03-26";
    const cJSON *asked = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    if (cJSON_IsString(asked) && asked->valuestring) version = asked->valuestring;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", s_srv_name);
    cJSON_AddStringToObject(info, "version", s_srv_ver);
    return wrap_result(id, result);
}

cJSON *appfw_mcp_handle(const char *body, size_t len, int *status)
{
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!root) {
        if (status) *status = 200;
        return build_error(NULL, -32700, "parse error");
    }
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(method) || !method->valuestring) {
        cJSON *e = build_error(id, -32601, "missing method");
        cJSON_Delete(root);
        if (status) *status = 200;
        return e;
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const bool is_notification = (id == NULL);

    cJSON *resp = NULL;
    if (is_notification) {
        resp = NULL;                                 // 通知:202 空体
    } else if (strcmp(method->valuestring, "initialize") == 0) {
        resp = do_initialize(id, params);
    } else if (strcmp(method->valuestring, "ping") == 0) {
        resp = wrap_result(id, cJSON_CreateObject()); // 空对象即空 result
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        resp = do_tools_list(id);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        resp = do_tool_call(id, params);
    } else {
        resp = build_error(id, -32601, "method not found");
    }
    cJSON_Delete(root);
    if (status) *status = is_notification ? 202 : 200;
    return resp;
}

// ---------------------------------------------------------------- 内置功能工具
//
// 每个内置菜单项使能后自动挂载的等价 MCP 工具(位序同 appfw_menu_item_t):
//   bit0 刷新周期 → set_refresh_period   bit1 熄屏时间 → set_screen_off
//   bit2 WiFi     → wifi_status + wifi_connect_saved
//   bit3 设备信息 → get_device_info      bit4 配网   → get_provisioning_status
//   bit5 AI 管理  → 不挂(纯信息页,AI 本来就在用本协议)
// 全部是对框架既有函数的薄封装。

static int bi_screen_off(cJSON *args, appfw_mcp_resp_t *resp)
{
    static const uint16_t GEARS[] = { 0, 60, 300, 600, 900, 1800 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "seconds");
    if (!cJSON_IsNumber(v)) {
        uint16_t cur = 0;
        (void)appfw_store_get_screen_off(&cur);
        appfw_mcp_resp_addf(resp, "当前熄屏时间 %u 秒(0=永不);可设 0/60/300/600/900/1800",
                            (unsigned)cur);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(GEARS) / sizeof(GEARS[0]); i++) {
        if (GEARS[i] == (uint16_t)v->valueint) {
            (void)appfw_store_set_screen_off(GEARS[i]);
            appfw_mcp_resp_addf(resp, "熄屏时间已设为 %d 秒%s",
                                v->valueint, v->valueint == 0 ? "(永不)" : "");
            return 0;
        }
    }
    appfw_mcp_resp_addf(resp, "非法值 %d:可选 0/60/300/600/900/1800", v->valueint);
    return 1;
}

static int bi_brightness(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "percent");
    if (!cJSON_IsNumber(v)) {
        uint16_t cur = 100;
        (void)appfw_store_get_brightness(&cur);
        appfw_mcp_resp_addf(resp, "当前屏幕亮度 %u%%;可设 10/30/50/70/100(屏幕全灭用 set_screen_off)",
                            (unsigned)cur);
        return 0;
    }
    int pct = v->valueint;
    if (pct < 10) pct = 10;                 // 不给 AI 把屏幕调到全黑的能力
    if (pct > 100) pct = 100;
    // 落到最近档位(菜单/存储只认 10/30/50/70/100)
    static const uint16_t GEARS[] = { 10, 30, 50, 70, 100 };
    unsigned best = 0;
    for (unsigned i = 1; i < sizeof(GEARS) / sizeof(GEARS[0]); i++) {
        if (abs(pct - (int)GEARS[i]) < abs(pct - (int)GEARS[best])) best = i;
    }
    (void)appfw_store_set_brightness(GEARS[best]);
    if (s_brightness_apply) s_brightness_apply((uint8_t)GEARS[best]);
    else appfw_mcp_resp_addf(resp, "(设备未接屏幕,仅保存)");
    appfw_mcp_resp_addf(resp, "亮度已设为 %u%%(要求 %d,就近取档)",
                        (unsigned)GEARS[best], pct);
    return 0;
}

static int bi_refresh(cJSON *args, appfw_mcp_resp_t *resp)
{
    static const uint16_t GEARS[] = { 60, 300, 600, 900, 1800, 3600 };
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "seconds");
    if (!cJSON_IsNumber(v)) {
        uint16_t cur = 60;
        (void)appfw_store_get_period(&cur);
        appfw_mcp_resp_addf(resp, "当前刷新周期 %u 秒;可设 60/300/600/900/1800/3600",
                            (unsigned)cur);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(GEARS) / sizeof(GEARS[0]); i++) {
        if (GEARS[i] == (uint16_t)v->valueint) {
            (void)appfw_store_set_period(GEARS[i]);
            appfw_mcp_resp_addf(resp, "刷新周期已设为 %d 秒", v->valueint);
            appfw_client_refresh_now();
            return 0;
        }
    }
    appfw_mcp_resp_addf(resp, "非法值 %d:可选 60/300/600/900/1800/3600", v->valueint);
    return 1;
}

static int bi_wifi_status(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    static const char *K[] = { "未连接", "扫描中", "连接中", "在线", "掉线重试中" };
    appfw_mcp_resp_addf(resp, "%s | IP %s | 热点 %s",
                        K[st.state & 3], st.ip[0] ? st.ip : "--", st.ap_ssid);
    return 0;
}

static int bi_wifi_connect(cJSON *args, appfw_mcp_resp_t *resp)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) {
        appfw_mcp_resp_addf(resp, "参数 ssid(string)缺失;仅能连接已保存的热点");
        return 1;
    }
    appfw_netlist_t list;
    if (!appfw_store_netlist_load(&list)) {
        appfw_mcp_resp_addf(resp, "设备没有已保存的热点");
        return 1;
    }
    for (int i = 0; i < list.count; i++) {
        if (strcmp(list.items[i].ssid, ssid->valuestring) == 0) {
            appfw_net_connect_ssid(ssid->valuestring);
            appfw_mcp_resp_addf(resp, "正在连接已保存的热点 %s,稍后用 wifi_status 查询结果",
                                ssid->valuestring);
            return 0;
        }
    }
    appfw_mcp_resp_addf(resp, "%s 不在已保存列表里(新热点可用应用的 wifi_add_hotspot)",
                        ssid->valuestring);
    return 1;
}

static int bi_device_info(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    const esp_app_desc_t *app = esp_app_get_description();
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    appfw_mcp_resp_addf(resp,
        "固件 %s %s | IP %s | 空闲堆 %uKB | 运行 %u 分钟",
        app->project_name, app->version,
        st.ip[0] ? st.ip : "--",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(esp_timer_get_time() / 60000000LL));
    return 0;
}

static int bi_prov_status(cJSON *args, appfw_mcp_resp_t *resp)
{
    (void)args;
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    appfw_mcp_resp_addf(resp, "配网门户:%s | AP %s | 本机 IP %s",
                        st.portal_active ? "开启" : "关闭",
                        st.ap_ssid, st.ip[0] ? st.ip : "--");
    return 0;
}

// 描述符按位序放([3] 空缺 = WiFi 项挂两个工具,连接工具单独收尾)。
static const appfw_mcp_tool_t BI_TOOLS[] = {
    [0] = { "set_refresh_period", "设置看板刷新周期(秒;无参数=查询当前值)",
            "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\"}}}", bi_refresh },
    [1] = { "set_screen_off", "设置屏幕熄屏时间(秒;0=永不;无参数=查询当前值)",
            "{\"type\":\"object\",\"properties\":{\"seconds\":{\"type\":\"integer\"}}}", bi_screen_off },
    [2] = { "wifi_status", "查询 WiFi 连接状态与本机 IP",
            "{}", bi_wifi_status },
    [3] = { "wifi_connect_saved", "连接一个已保存的热点",
            "{\"type\":\"object\",\"properties\":{\"ssid\":{\"type\":\"string\"}}}", bi_wifi_connect },
    [4] = { "get_device_info", "查询设备信息(固件/版本/IP/内存/运行时长)",
            "{}", bi_device_info },
    [5] = { "get_provisioning_status", "查询配网门户状态(AP 名/IP/客户端)",
            "{}", bi_prov_status },
    [7] = { "set_brightness", "设置屏幕亮度(百分比;无参数=查询当前值;10-100 就近取档)",
            "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\"}}}", bi_brightness },
};

void appfw_mcp_set_builtin_tools(unsigned menu_show_mask)
{
    static appfw_mcp_tool_t picked[9];           // 6 项 + WiFi 连接 + 余量
    int n = 0;
    // bit5(AI 管理)是纯信息页,不挂工具——AI 本来就在用本协议。
    for (int bit = 0; bit < 7; bit++) {
        if (bit == 5) continue;
        if (!(menu_show_mask & (1u << bit))) continue;
        // 位序→表下标映射:表里 [3] 是 WiFi 连接工具(随 bit2 附带),
        // bit≥3 的功能取表时要跳过它。
        const int ti = (bit < 3) ? bit : (bit + 1);
        picked[n++] = BI_TOOLS[ti];
        if (bit == 2) picked[n++] = BI_TOOLS[3];   // WiFi 连接工具随 bit2 附带
    }
    s_bi_tools = picked;
    s_bi_count = n;
    ESP_LOGI(TAG, "内置工具挂载 %d 个(mask=0x%x):", n, menu_show_mask);
    for (int i = 0; i < n; i++) ESP_LOGI(TAG, "  - %s", picked[i].name);
}
