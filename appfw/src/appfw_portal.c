// components/appfw/src/appfw_portal.c —— 配网门户实现(通用)。
//
// 常驻 HTTP(TCP 80)+ captive DNS(UDP 53,仅 AP 阶段);两阶段网页:
// 阶段一(未联网)只显示连接 WiFi;阶段二(在线,经局域网 IP)显示
// 框架设置(刷新/熄屏)+ WiFi 管理 + 应用注入片段(<!--APP_CONFIG_HTML-->)。
// REST:status/scan/saved/networks(+add/delete)/connect/settings/config-export/
// config-import/clear。应用专属端点经 appfw_prov_on_httpd_ready 注册。
#include "appfw_portal.h"

#include <string.h>

#include "appfw_client.h"
#include "appfw_files.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_storage.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "appfw_prov";

static appfw_prov_cfg_t s_cfg;
static TaskHandle_t s_dns_task;
static volatile bool s_dns_quit;
static int s_dns_sock = -1;
static httpd_handle_t s_http;
static volatile bool s_running;
static char s_page_buf[16 * 1024];

void appfw_prov_configure(const appfw_prov_cfg_t *cfg)
{
    if (cfg) s_cfg = *cfg;
}

// --------------------------------------------------------------- DNS 劫持

#define DNS_PORT 53
#define DNS_BUF_LEN 512

// 极简 DNS 应答:任何 A 查询都回 192.168.4.1,把 captive portal 探测引到设备。
static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_BUF_LEN];
    struct sockaddr_storage from;
    socklen_t from_len;

    while (!s_dns_quit) {
        from_len = sizeof(from);
        int n = recvfrom(s_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n < 12) continue;
        uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
        if (flags & 0x8000) continue; // 是应答不是查询
        uint16_t qd = (uint16_t)((buf[4] << 8) | buf[5]);
        if (qd != 1) continue;

        uint8_t resp[DNS_BUF_LEN];
        size_t qlen = (size_t)n;
        if (qlen + 16 > sizeof(resp)) continue;
        memcpy(resp, buf, qlen);
        resp[2] = 0x81; // QR=1 RD=1
        resp[3] = 0x80; // RA=1
        resp[6] = 0; resp[7] = 1; // ANCOUNT=1
        resp[8] = 0; resp[9] = 0;
        resp[10] = 0; resp[11] = 0;
        static const uint8_t tail[12] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
                                          0x00, 0x00, 0x00, 0x3C, 0x00, 0x04 };
        memcpy(resp + qlen, tail, sizeof(tail));
        static const uint8_t ip4[4] = { 192, 168, 4, 1 };
        memcpy(resp + qlen + sizeof(tail), ip4, 4);
        (void)sendto(s_dns_sock, resp, qlen + sizeof(tail) + 4, 0,
                     (struct sockaddr *)&from, from_len);
    }
    vTaskDelete(NULL);
}

// --------------------------------------------------------------- JSON 助手

void *appfw_prov_read_json(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return NULL;
    }
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, buf + received, (size_t)(total - received));
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return NULL;
        }
        received += n;
    }
    buf[total] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return root;
}

void appfw_prov_send_ok(httpd_req_t *req, bool ok)
{
    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                          HTTPD_RESP_USE_STRLEN);
}

// --------------------------------------------------------------- REST 处理器

static esp_err_t handler_status(httpd_req_t *req)
{
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "state", st.state);
    cJSON_AddBoolToObject(root, "portal", st.portal_active);
    cJSON_AddBoolToObject(root, "has_config", st.has_config);
    cJSON_AddStringToObject(root, "cur_ssid", st.cur_ssid);
    cJSON_AddStringToObject(root, "ip", st.ip);
    cJSON_AddStringToObject(root, "ap_ssid", st.ap_ssid);
    cJSON_AddNumberToObject(root, "close_s", st.portal_close_s);
    uint16_t period_s = 60, soff = 300;
    appfw_store_get_period(&period_s);
    appfw_store_get_screen_off(&soff);
    cJSON_AddNumberToObject(root, "period_s", period_s);
    cJSON_AddNumberToObject(root, "screen_off_s", soff);
    if (s_cfg.app_config_fill) s_cfg.app_config_fill((void *)root);
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_scan_trigger(httpd_req_t *req)
{
    (void)req;
    appfw_net_scan();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_scan_result(httpd_req_t *req)
{
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "seq", st.scan_seq);
    cJSON_AddNumberToObject(root, "count", st.scan_count);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    for (uint8_t i = 0; i < st.scan_count && items; i++) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "ssid", st.scan[i].ssid);
        cJSON_AddNumberToObject(it, "rssi", st.scan[i].rssi);
        cJSON_AddBoolToObject(it, "auth", st.scan[i].auth);
        cJSON_AddItemToArray(items, it);
    }
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_saved(httpd_req_t *req)
{
    appfw_netlist_t list;
    bool have = appfw_store_netlist_load(&list);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "count", have ? list.count : 0);
    cJSON *items = cJSON_AddArrayToObject(root, "items");
    if (have && items) {
        for (uint8_t i = 0; i < list.count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", list.items[i].ssid);
            cJSON_AddBoolToObject(it, "selected", (list.selected == (int8_t)i));
            cJSON_AddItemToArray(items, it);
        }
    }
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_networks(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *nets = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (!cJSON_IsArray(nets)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "networks required");
        return ESP_FAIL;
    }
    // 整表替换;替换前读旧表,旧点选的 SSID 若仍在新表中则保持点选。
    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    appfw_netlist_t old;
    char old_sel[APPFW_NETLIST_SSID_MAX] = { 0 };
    if (appfw_store_netlist_load(&old) && old.selected >= 0 &&
        old.selected < (int8_t)old.count) {
        snprintf(old_sel, sizeof(old_sel), "%s", old.items[old.selected].ssid);
    }
    cJSON *it;
    cJSON_ArrayForEach(it, nets) {
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(it, "ssid");
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(it, "pwd");
        if (!cJSON_IsString(ssid) || !ssid->valuestring) continue;
        if (!appfw_netlist_add(&list, ssid->valuestring,
                               cJSON_IsString(pwd) && pwd->valuestring ? pwd->valuestring : "")) break;
    }
    bool ok = list.count > 0 && appfw_store_netlist_save(&list);
    if (ok && old_sel[0] != '\0' && appfw_netlist_select(&list, old_sel)) {
        (void)appfw_store_netlist_save(&list);
    }
    if (ok) {
        appfw_net_reload_config();
        appfw_net_connect_saved(); // 保存即生效:重载后自动连接
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// 新增热点(免扫描):合并必须在设备端做(密码不回显浏览器)。
static esp_err_t handler_networks_add(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    cJSON *pwd = cJSON_GetObjectItemCaseSensitive(root, "pwd");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    appfw_store_netlist_load(&list);
    bool ok = appfw_netlist_add(&list, ssid->valuestring,
                                cJSON_IsString(pwd) && pwd->valuestring ? pwd->valuestring : "") &&
              appfw_store_netlist_save(&list);
    if (ok) appfw_net_reload_config();
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_delete(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    appfw_netlist_t list;
    bool ok = false;
    if (appfw_store_netlist_load(&list)) {
        for (uint8_t i = 0; i < list.count; i++) {
            if (strcmp(list.items[i].ssid, ssid->valuestring) == 0) {
                ok = appfw_netlist_remove(&list, i) && appfw_store_netlist_save(&list);
                if (ok) appfw_net_reload_config();
                break;
            }
        }
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_connect(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid) || !ssid->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    // 目标必须在已存列表(net 任务只认已存热点);不存在直接报错不静默。
    appfw_netlist_t list;
    bool known = appfw_store_netlist_load(&list) &&
                 appfw_netlist_select(&list, ssid->valuestring);
    if (!known) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid not saved");
        return ESP_FAIL;
    }
    appfw_net_connect_ssid(ssid->valuestring);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// 框架设置:刷新周期/熄屏档位(合法值校验;缺省不改动)。
static esp_err_t handler_settings(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    bool ok = true;
    cJSON *period = cJSON_GetObjectItemCaseSensitive(root, "period");
    if (cJSON_IsNumber(period)) {
        if (!appfw_store_set_period((uint16_t)period->valueint)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "period invalid");
            return ESP_FAIL;
        }
    }
    cJSON *soff = cJSON_GetObjectItemCaseSensitive(root, "screen_off");
    if (cJSON_IsNumber(soff)) {
        if (!appfw_store_set_screen_off((uint16_t)soff->valueint)) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "screen_off invalid");
            return ESP_FAIL;
        }
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// 导出全部配置(框架设置 + 应用钩子补字段;热点表含密码,文件由用户保管)。
static esp_err_t handler_export(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    cJSON_AddNumberToObject(root, "v", 1);
    uint16_t period_s = 60, soff = 300;
    appfw_store_get_period(&period_s);
    appfw_store_get_screen_off(&soff);
    cJSON_AddNumberToObject(root, "period_s", period_s);
    cJSON_AddNumberToObject(root, "screen_off_s", soff);
    if (s_cfg.app_config_fill) s_cfg.app_config_fill((void *)root);

    appfw_netlist_t list;
    bool have = appfw_store_netlist_load(&list);
    cJSON_AddNumberToObject(root, "count", have ? list.count : 0);
    cJSON *nets = cJSON_AddArrayToObject(root, "networks");
    const char *selected = "";
    if (have && nets) {
        for (uint8_t i = 0; i < list.count; i++) {
            cJSON *it = cJSON_CreateObject();
            cJSON_AddStringToObject(it, "ssid", list.items[i].ssid);
            cJSON_AddStringToObject(it, "pwd", list.items[i].pwd);
            cJSON_AddItemToArray(nets, it);
            if (list.selected == (int8_t)i) selected = list.items[i].ssid;
        }
    }
    cJSON_AddStringToObject(root, "selected", have ? selected : "");

    const char *txt = cJSON_PrintUnformatted(root);
    // 设备本地同步留存:全量刷机后开机自动恢复(见 restore_config_from_files)。
    if (txt) appfw_files_write("config.json", txt, strlen(txt));
    cJSON_Delete(root);
    if (!txt) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=appfw-config.json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

// 导入:v=1 校验;框架设置写回,应用钩子处理自有字段;热点表逐条校验合并。
// 应用导入根对象(框架设置 + 应用钩子 + 热点表 + 点选)。供 /api/config/import
// 与开机自动恢复共用。返回是否全部成功。
static bool apply_import_root(cJSON *root)
{
    bool ok = true;
    cJSON *period = cJSON_GetObjectItemCaseSensitive(root, "period_s");
    if (cJSON_IsNumber(period)) ok = ok && appfw_store_set_period((uint16_t)period->valueint);
    cJSON *soff = cJSON_GetObjectItemCaseSensitive(root, "screen_off_s");
    if (cJSON_IsNumber(soff)) ok = ok && appfw_store_set_screen_off((uint16_t)soff->valueint);

    appfw_netlist_t list;
    appfw_netlist_reset(&list);
    cJSON *nets = cJSON_GetObjectItemCaseSensitive(root, "networks");
    if (cJSON_IsArray(nets)) {
        cJSON *it;
        cJSON_ArrayForEach(it, nets) {
            cJSON *s = cJSON_GetObjectItemCaseSensitive(it, "ssid");
            cJSON *w = cJSON_GetObjectItemCaseSensitive(it, "pwd");
            if (!cJSON_IsString(s) || !s->valuestring) continue;
            if (!appfw_netlist_add(&list, s->valuestring,
                                   cJSON_IsString(w) && w->valuestring ? w->valuestring : "")) break;
        }
    }
    cJSON *sel = cJSON_GetObjectItemCaseSensitive(root, "selected");
    if (cJSON_IsString(sel) && sel->valuestring && sel->valuestring[0]) {
        (void)appfw_netlist_select(&list, sel->valuestring);
    }
    ok = ok && appfw_store_netlist_save(&list);
    if (ok && s_cfg.app_config_apply) ok = s_cfg.app_config_apply((void *)root);
    if (ok) {
        appfw_net_reload_config();
        appfw_client_refresh_now();
    }
    return ok;
}

static esp_err_t handler_import(httpd_req_t *req)
{
    cJSON *root = appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(v) || v->valueint != 1) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported version");
        return ESP_FAIL;
    }
    bool ok = apply_import_root(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_clear(httpd_req_t *req)
{
    (void)req;
    bool ok = appfw_store_clear_all();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

// --------------------------------------------------------------- 页面

static const char PAGE_HTML_TEMPLATE[] =
#include "appfw_portal_html.inc"
;

// 开机自动恢复:若 files 分区存在 config.json(此前"导出"留存的配置),
// 读入并应用(成功后删除,一次性语义,避免覆盖之后的手动修改)。
bool appfw_portal_restore_config(const char *path)
{
    static char buf[4096];
    size_t len = 0;
    if (!appfw_files_read("config.json", buf, sizeof(buf), &len)) return false;
    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    bool ok = cJSON_IsNumber(v) && v->valueint == 1 && apply_import_root(root);
    cJSON_Delete(root);
    if (ok) {
        char path[32];
        snprintf(path, sizeof(path), "/files/%s", "config.json");
        unlink(path);
        ESP_LOGI(TAG, "已从 files 分区恢复配置(config.json)");
    }
    return ok;
}

// 应用 HTML 片段注入:模板中 <!--APP_CONFIG_HTML--> 替换为应用片段(首次构建)。
static esp_err_t handler_index(httpd_req_t *req)
{
    if (!s_page_buf[0]) {
        const char *frag = s_cfg.app_config_html ? s_cfg.app_config_html() : "";
        const char *marker = "<!--APP_CONFIG_HTML-->";
        const char *pos = strstr(PAGE_HTML_TEMPLATE, marker);
        if (!pos) {
            snprintf(s_page_buf, sizeof(s_page_buf), "%s", PAGE_HTML_TEMPLATE);
        } else {
            size_t head = (size_t)(pos - PAGE_HTML_TEMPLATE);
            snprintf(s_page_buf, sizeof(s_page_buf), "%.*s%s%s",
                     (int)head, PAGE_HTML_TEMPLATE, frag ? frag : "",
                     pos + strlen(marker));
        }
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_page_buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_captive(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t err_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

// --------------------------------------------------------------- 启停

bool appfw_portal_start(void)
{
    if (s_running) return true;
    if (s_dns_sock < 0) {
        s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_dns_sock < 0) {
            ESP_LOGE(TAG, "DNS socket 创建失败");
            return false;
        }
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            ESP_LOGE(TAG, "DNS bind 失败");
            close(s_dns_sock);
            s_dns_sock = -1;
            return false;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 300 * 1000 };
        setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s_dns_quit = false;
    if (!s_dns_task &&
        xTaskCreate(dns_task, "appfw_dns", 3072, NULL, 4, &s_dns_task) != pdPASS) {
        ESP_LOGE(TAG, "DNS 任务创建失败");
        return false;
    }

    if (!s_http) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.max_uri_handlers = 24; // 框架 14 + 文件管理 8 + 应用注入约 2 // 框架 14 + 应用注入约 3
        cfg.stack_size = 6144;
        if (httpd_start(&s_http, &cfg) != ESP_OK) {
            ESP_LOGE(TAG, "HTTP 服务启动失败");
            return false;
        }
        static const httpd_uri_t routes[] = {
            { .uri = "/",                 .method = HTTP_GET,  .handler = handler_index },
            { .uri = "/api/status",       .method = HTTP_GET,  .handler = handler_status },
            { .uri = "/api/scan",         .method = HTTP_GET,  .handler = handler_scan_result },
            { .uri = "/api/scan",         .method = HTTP_POST, .handler = handler_scan_trigger },
            { .uri = "/api/saved",        .method = HTTP_GET,  .handler = handler_saved },
            { .uri = "/api/networks",     .method = HTTP_POST, .handler = handler_networks },
            { .uri = "/api/networks/add", .method = HTTP_POST, .handler = handler_networks_add },
            { .uri = "/api/delete",       .method = HTTP_POST, .handler = handler_delete },
            { .uri = "/api/connect",      .method = HTTP_POST, .handler = handler_connect },
            { .uri = "/api/settings",     .method = HTTP_POST, .handler = handler_settings },
            { .uri = "/api/config/export", .method = HTTP_GET,  .handler = handler_export },
            { .uri = "/api/config/import", .method = HTTP_POST, .handler = handler_import },
            { .uri = "/api/clear",        .method = HTTP_POST, .handler = handler_clear },
            { .uri = "/generate_204",     .method = HTTP_GET,  .handler = handler_captive },
        };
        for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
            if (httpd_register_uri_handler(s_http, &routes[i]) != ESP_OK) {
                ESP_LOGE(TAG, "注册路由失败:%s", routes[i].uri);
                httpd_stop(s_http);
                s_http = NULL;
                return false;
            }
        }
        httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, err_404);
        // 框架内置:文件管理页面与端点。
        if (!appfw_files_register((void *)s_http)) {
            ESP_LOGW(TAG, "文件管理端点注册失败");
        }
        if (s_cfg.on_httpd_ready && !s_cfg.on_httpd_ready((void *)s_http)) {
            ESP_LOGW(TAG, "应用门户端点注册失败(不影响框架端点)");
        }
    }
    s_running = true;
    ESP_LOGI(TAG, "管理门户已就绪:配网期 http://192.168.4.1,联网后 http://<设备IP>");
    return true;
}

void appfw_portal_stop_dns(void)
{
    if (s_dns_sock >= 0) {
        s_dns_quit = true;
        vTaskDelay(pdMS_TO_TICKS(400));
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        ESP_LOGI(TAG, "DNS 劫持已停止(联网模式)");
    }
}

bool appfw_portal_running(void)
{
    return s_running;
}
