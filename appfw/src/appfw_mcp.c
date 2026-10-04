// appfw/src/appfw_mcp.c —— 见 appfw_mcp.h。
#include "appfw_mcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "appfw_portal.h"

static const char *TAG = "appfw_mcp";

static const appfw_mcp_tool_t *s_tools;
static int s_tool_count;
static const char *s_srv_name = "ai-passport";
static const char *s_srv_ver = "1.0";

void appfw_mcp_set_tools(const appfw_mcp_tool_t *tools, int count)
{
    s_tools = tools;
    s_tool_count = count;
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

// JSON 回复统一分块发送(≤256B 一片,碎片堆上也发得出去)。
static esp_err_t send_cjson(httpd_req_t *req, cJSON *j)
{
    char *txt = cJSON_PrintUnformatted(j);
    if (!txt) return ESP_FAIL;
    httpd_resp_set_type(req, "application/json");
    const char *p = txt;
    size_t len = strlen(txt);
    esp_err_t e = ESP_OK;
    while (len && e == ESP_OK) {
        const size_t n = len < 256 ? len : 256;
        e = httpd_resp_send_chunk(req, p, (int)n);
        p += n;
        len -= n;
    }
    cJSON_free(txt);
    if (e != ESP_OK) return e;
    return httpd_resp_send_chunk(req, NULL, 0);
}

// JSON-RPC 错误回复(code:-32700 解析/-32601 方法不存在/-32602 参数错)。
static esp_err_t send_error(httpd_req_t *req, const cJSON *id, int code,
                            const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
    else cJSON_AddNullToObject(resp, "id");
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    esp_err_t e = send_cjson(req, resp);
    cJSON_Delete(resp);
    return e;
}

static const appfw_mcp_tool_t *find_tool(const char *name)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) return &s_tools[i];
    }
    return NULL;
}

// tools/call:取工具 → 跑处理函数 → 包成 MCP content。
static esp_err_t do_tool_call(httpd_req_t *req, const cJSON *id,
                              const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        return send_error(req, id, -32602, "missing tool name");
    }
    const appfw_mcp_tool_t *tool = find_tool(name->valuestring);
    if (!tool) {
        return send_error(req, id, -32602, "unknown tool");
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

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    cJSON_AddItemToObject(r, "result", result);
    esp_err_t e = send_cjson(req, r);
    cJSON_Delete(r);
    return e;
}

// tools/list:走注册表。schema 是 const JSON 字符串,逐个 parse 挂上。
static esp_err_t do_tools_list(httpd_req_t *req, const cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    cJSON *result = cJSON_AddObjectToObject(r, "result");
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");
    for (int i = 0; i < s_tool_count; i++) {
        cJSON *t = cJSON_AddObjectToObject(tools, "");
        cJSON_AddStringToObject(t, "name", s_tools[i].name);
        cJSON_AddStringToObject(t, "description", s_tools[i].description);
        cJSON *schema = cJSON_Parse(s_tools[i].input_schema);
        if (schema) cJSON_AddItemToObject(t, "inputSchema", schema);
        else cJSON_AddObjectToObject(t, "inputSchema");
    }
    esp_err_t e = send_cjson(req, r);
    cJSON_Delete(r);
    return e;
}

// initialize:回协议版本(兼容则回显客户端请求的)+ 能力 + 服务器信息。
static esp_err_t do_initialize(httpd_req_t *req, const cJSON *id,
                               const cJSON *params)
{
    const char *version = "2025-03-26";
    const cJSON *asked = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");
    if (cJSON_IsString(asked) && asked->valuestring) version = asked->valuestring;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    cJSON *result = cJSON_AddObjectToObject(r, "result");
    cJSON_AddStringToObject(result, "protocolVersion", version);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", s_srv_name);
    cJSON_AddStringToObject(info, "version", s_srv_ver);
    esp_err_t e = send_cjson(req, r);
    cJSON_Delete(r);
    return e;
}

// MCP 端点:POST JSON-RPC。通知(无 id)按规范回 202 空体。
static esp_err_t mcp_handler(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"POST only\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    appfw_portal_touch();          // MCP 请求同样给门户续命
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;    // 读不到/非 JSON:read_json 已回 400

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(method) || !method->valuestring) {
        cJSON_Delete(root);
        return send_error(req, id, -32601, "missing method");
    }
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const bool is_notification = (id == NULL);

    esp_err_t e;
    if (strcmp(method->valuestring, "initialize") == 0) {
        e = is_notification ? ESP_OK : do_initialize(req, id, params);
    } else if (strcmp(method->valuestring, "notifications/initialized") == 0 ||
               strcmp(method->valuestring, "notifications/cancelled") == 0) {
        e = ESP_OK;                                  // 通知:202 空体
    } else if (strcmp(method->valuestring, "ping") == 0) {
        e = is_notification ? ESP_OK : send_cjson(req,
                cJSON_CreateObject());               // 空对象即空 result
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        e = is_notification ? ESP_OK : do_tools_list(req, id);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        e = is_notification ? ESP_OK : do_tool_call(req, id, params);
    } else {
        e = is_notification ? ESP_OK
                            : send_error(req, id, -32601, "method not found");
    }
    cJSON_Delete(root);
    if (is_notification) {
        // 通知不需要响应体;204 语义最贴切(httpd 无 204 助手,手工发)。
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_send(req, NULL, 0);
    }
    return e;
}

bool appfw_mcp_register(void *httpd)
{
    if (s_tool_count == 0) return true;   // 未开启:一个路由都不占
    httpd_handle_t h = (httpd_handle_t)httpd;
    static const httpd_uri_t post = {
        .uri = "/mcp", .method = HTTP_POST, .handler = mcp_handler,
    };
    static const httpd_uri_t get = {
        .uri = "/mcp", .method = HTTP_GET, .handler = mcp_handler,
    };
    if (httpd_register_uri_handler(h, &post) != ESP_OK ||
        httpd_register_uri_handler(h, &get) != ESP_OK) {
        ESP_LOGE(TAG, "注册 /mcp 路由失败");
        return false;
    }
    ESP_LOGI(TAG, "MCP 已开启:%d 个工具,POST /mcp", s_tool_count);
    return true;
}
