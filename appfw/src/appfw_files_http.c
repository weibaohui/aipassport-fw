// components/appfw/src/appfw_files_http.c —— 文件管理的 HTTP 端点与页面。
//
// 安全:除"设置密码/开锁"外,所有 /api/files/* 端点都要求已开锁;
// 未开锁一律 403。上传走 POST body(原始字节),下载走 GET(附件)。
#include "appfw_files.h"
#include "appfw_portal.h"

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "appfw_files_http";

static bool req_unlocked(struct httpd_req *req)
{
    if (appfw_files_is_unlocked()) return true;
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"error\":\"locked\"}", HTTPD_RESP_USE_STRLEN);
    return false;
}

// 从 query 取 name 并校验;非法已回 400。返回 NULL 表示失败。
static char *req_name(struct httpd_req *req)
{
    static char name[80];
    char query[160], qname[80];
    size_t qlen = sizeof(query);
    if (httpd_req_get_url_query_str(req, query, qlen) != ESP_OK) return NULL;
    if (httpd_query_key_value(query, "name", qname, sizeof(qname)) != ESP_OK) return NULL;
    if (!appfw_files_valid_name(qname)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return NULL;
    }
    snprintf(name, sizeof(name), "%s", qname);
    return name;
}

static esp_err_t handler_list(httpd_req_t *req)
{
    if (!req_unlocked(req)) return ESP_FAIL;
    static char names[32][64];
    int n = appfw_files_list(names, 32);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", n);
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    const char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    cJSON_free((void *)txt);
    return ret;
}

static esp_err_t handler_download(httpd_req_t *req)
{
    if (!req_unlocked(req)) return ESP_FAIL;
    char *name = req_name(req);
    if (!name) return ESP_FAIL;
    // 流式分块下载:不占大缓冲(C3 无 PSRAM,64KB 静态缓冲曾挤爆堆)。
    // chunk 必须静态:httpd 任务栈只有 6KB,4KB 放栈上+FAT 写路径实测
    // 栈保护故障(真机 panic)。httpd 单工作线程串行处理请求,静态安全。
    if (!appfw_files_ensure_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mount failed");
        return ESP_FAIL;
    }
    char path[96];
    snprintf(path, sizeof(path), "/files/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char disp[112];
    snprintf(disp, sizeof(disp), "attachment; filename=%s", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    static char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// 上传:POST body 为原始文件内容,name 经 query 传入。
static esp_err_t handler_upload(httpd_req_t *req)
{
    if (!req_unlocked(req)) return ESP_FAIL;
    char *name = req_name(req);
    if (!name) return ESP_FAIL;
    int total = req->content_len;
    if (total <= 0 || total > 512 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    // 流式写入:4KB 分片收一发一,不占大缓冲(C3 无 PSRAM,512KB 静态缓冲
    // 会把 DRAM 撑爆,实测链接期溢出)。chunk 静态:httpd 任务栈 6KB,4KB
    // 上栈 + FAT 写路径实测栈保护故障;httpd 单线程串行,静态安全。
    // 直接 fopen 绕过了 write_impl 的懒挂载,必须自己先确保挂载。
    if (!appfw_files_ensure_mounted()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mount failed");
        return ESP_FAIL;
    }
    char path[96];
    snprintf(path, sizeof(path), "/files/%s", name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }
    static char chunk[4096];
    int received = 0;
    bool ok = true;
    while (received < total) {
        int n = httpd_req_recv(req, chunk, sizeof(chunk) < (size_t)(total - received)
                                                   ? sizeof(chunk)
                                                   : (size_t)(total - received));
        if (n <= 0) { ok = false; break; }
        if (fwrite(chunk, 1, (size_t)n, f) != (size_t)n) { ok = false; break; }
        received += n;
    }
    fclose(f);
    if (!ok) {
        unlink(path); // 半截文件不留
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_delete(httpd_req_t *req)
{
    if (!req_unlocked(req)) return ESP_FAIL;
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    bool ok = cJSON_IsString(name) && name->valuestring &&
              appfw_files_delete(name->valuestring);
    cJSON_Delete(root);
    appfw_prov_send_ok(req, ok);
    return ESP_OK;
}

static esp_err_t handler_unlock(httpd_req_t *req)
{
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "password");
    bool ok = cJSON_IsString(pw) && pw->valuestring &&
              appfw_files_unlock(pw->valuestring);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"error\":\"wrong password\"}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_lock(httpd_req_t *req)
{
    (void)req;
    appfw_files_lock();
    appfw_prov_send_ok(req, true);
    return ESP_OK;
}

// 设置密码:未设密码时 old 可空;已设则 old 必须正确。
static esp_err_t handler_password(httpd_req_t *req)
{
    cJSON *root = (cJSON *)appfw_prov_read_json(req);
    if (!root) return ESP_FAIL;
    cJSON *oldp = cJSON_GetObjectItemCaseSensitive(root, "old");
    cJSON *newp = cJSON_GetObjectItemCaseSensitive(root, "new");
    bool ok = cJSON_IsString(newp) && newp->valuestring &&
              appfw_files_set_password(
                  cJSON_IsString(oldp) && oldp->valuestring ? oldp->valuestring : NULL,
                  newp->valuestring);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"error\":\"failed\"}",
                           HTTPD_RESP_USE_STRLEN);
}

// /files 页面(自包含)。
static esp_err_t handler_files_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
"<DOCTYPE html><html><head><meta charset=utf-8><title>文件管理</title></head>"
"<body style='font-family:sans-serif;max-width:520px;margin:30px auto'>"
"<h2>文件管理</h2><div id=msg></div>"
"<h3>开锁</h3><input id=pw type=password placeholder='密码'>"
"<button onclick=\"act('/api/files/unlock',{password:pw.value},'已开锁')\">开锁</button> "
"<button onclick=\"act('/api/files/lock',{},'已上锁')\">上锁</button> "
"<button onclick=\"setpw()\">设置密码</button>"
"<h3>上传</h3><input id=fname type=text placeholder='文件名(如 config.json)'> "
"<input id=file type=file><button onclick=up()>上传</button>"
"<h3>文件列表</h3><button onclick=load()>刷新</button><ul id=list></ul>"
"<script>\n"
"function $(i){return document.getElementById(i)}\n"
"async function act(u,b,okmsg){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});const j=await r.json();$('msg').textContent=j.ok?(okmsg||'成功'):(j.error||'失败');load();return j}\n"
"async function setpw(){const old=prompt('旧密码(首次设置留空并直接确定):');const nw=prompt('新密码(至少 4 位):');if(nw){const j=await act('/api/files/password',{old:old,new:nw},'密码已设置');}}\n"
"async function load(){try{const r=await fetch('/api/files/list');const j=await r.json();\n"
"$('list').innerHTML=(j.files||[]).map(f=>'<li><a href=\"/api/files/download?name='+encodeURIComponent(f)+'\">'+f+'</a> <button onclick=\\'del(\"'+f+'\")\\'>删除</button></li>').join('')||'<li>无文件</li>';\n"
"}catch(e){$('list').innerHTML='<li>未开锁或加载失败</li>'}}\n"
"function del(n){if(confirm('删除 '+n+'?'))act('/api/files/delete',{name:n},'已删除')}\n"
"async function up(){const f=$('file').files[0];if(!f)return alert('选择文件');\n"
"const name=$('fname').value.trim()||f.name;\n"
"const r=await fetch('/api/files/upload?name='+encodeURIComponent(name),{method:'POST',body:f});\n"
"const j=await r.json();alert(j.ok?'上传成功':'上传失败');load()}\n"
"load();\n"
"</script></body></html>",
        HTTPD_RESP_USE_STRLEN);
}

// 注册全部文件管理路由(httpd 就绪回调中调用;幂等由调用方保证)。
bool appfw_files_register(void *httpd)
{
    httpd_handle_t h = (httpd_handle_t)httpd;
    static const httpd_uri_t routes[] = {
        { .uri = "/files",                  .method = HTTP_GET,  .handler = handler_files_page },
        { .uri = "/api/files/list",         .method = HTTP_GET,  .handler = handler_list },
        { .uri = "/api/files/download",     .method = HTTP_GET,  .handler = handler_download },
        { .uri = "/api/files/upload",       .method = HTTP_POST, .handler = handler_upload },
        { .uri = "/api/files/delete",       .method = HTTP_POST, .handler = handler_delete },
        { .uri = "/api/files/unlock",       .method = HTTP_POST, .handler = handler_unlock },
        { .uri = "/api/files/lock",         .method = HTTP_POST, .handler = handler_lock },
        { .uri = "/api/files/password",     .method = HTTP_POST, .handler = handler_password },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(h, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "注册路由失败:%s", routes[i].uri);
            return false;
        }
    }
    return true;
}
