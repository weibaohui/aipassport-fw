// appfw/src/appfw_mcp_srv.c —— MCP 常驻极简 TCP 服务,见 appfw_mcp.h。
//
// 为什么不用 esp_http_server:一个单端点 JSON 服务用不上它的解析器矩阵、
// keep-alive 状态机和 6KB 任务栈;常驻内存要省到个位数 KB,只能自己写。
// 协议面刻意收窄到 AI 宿主实际会发的样子:POST /mcp + Content-Length 的
// JSON,一律 Connection: close——没有会话、没有分块请求、没有 SSE。
// 空转成本 = 一个任务栈(4KB 静态)+ 一个监听 PCB;请求期间 cJSON 树是
// 瞬时堆,响应超 1KB(tools/list)也是打印后即刻释放。
#include "appfw_mcp.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define MCP_PORT        8080
#define HDR_CAP         768    // 请求头上限(AI 宿主通常 <300B)
#define BODY_CAP        2048   // 请求体上限(tools/call 的参数都很小)
#define STACK_SIZE      6144   // FAT 挂载/重建索引链 + newlib vfprintf 是栈大户,
                               // 4096 真机实测打穿(栈保护故障);水位日志盯峰值

static const char *TAG = "appfw_mcp_srv";
static TaskHandle_t s_task;
static volatile bool s_running;

static void send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    int retries = 0;
    while (off < len) {
        const ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            // 设备重连/借洞的瞬间堆最碎,lwip 可能暂时借不到发送缓冲:
            // 短重试而不是放弃——AI 的应答丢一半比晚半秒糟得多。
            const int err = errno;
            if ((err == ENOMEM || err == EAGAIN || err == EWOULDBLOCK) &&
                ++retries <= 20) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            return;
        }
        if (n == 0) return;
        off += (size_t)n;
        retries = 0;
    }
}

// 极简应答:状态行 + Content-Length + close。body 为 NULL 时只发头。
static void send_resp(int fd, int status, const char *body, size_t len)
{
    static const struct { int code; const char *text; } R[] = {
        { 200, "200 OK" },       { 202, "202 Accepted" },
        { 400, "400 Bad Request" }, { 404, "404 Not Found" },
        { 405, "405 Method Not Allowed" }, { 413, "413 Content Too Large" },
    };
    const char *text = "500 Internal Server Error";
    for (size_t i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        if (R[i].code == status) { text = R[i].text; break; }
    }
    char hdr[128];
    const int hn = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 %s\r\n"
                            "Content-Type: application/json\r\n"
                            "Content-Length: %u\r\n"
                            "Connection: close\r\n\r\n",
                            text, (unsigned)(body ? len : 0));
    send_all(fd, hdr, (size_t)hn);
    if (body) {
        // ≤512B 一片发:碎片堆上 lwip 也发得出去(门户时代的教训)。
        size_t off = 0;
        while (off < len) {
            const size_t n = len - off < 512 ? len - off : 512;
            send_all(fd, body + off, n);
            off += n;
        }
    }
}

// 头部里找 Content-Length(逐行扫,字段名大小写不敏感;没有/非法 = -1)。
static int content_length_of(const char *hdr, size_t len)
{
    static const char key[] = "content-length:";
    const char *p = hdr;
    const char *end = hdr + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *q = p;                       // 行首剥 \r\n
        while (q < eol && (*q == '\r' || *q == '\n')) q++;
        size_t k = 0;
        while (k < sizeof(key) - 1 && q + k < eol) {
            char c = q[k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != key[k]) break;
            k++;
        }
        if (k == sizeof(key) - 1) return atoi(q + k);
        p = eol < end ? eol + 1 : end;
    }
    return -1;
}

// 处理一条已接受的连接;返回是否收到了完整请求(只用于日志)。
static void serve(int fd)
{
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static char hdr[HDR_CAP];      // 单连接串行服务,静态缓冲安全
    size_t hlen = 0;
    char *body_end = NULL;
    while (!body_end) {
        if (hlen >= HDR_CAP) { send_resp(fd, 400, "{\"error\":\"headers too large\"}", 0); return; }
        const ssize_t n = recv(fd, hdr + hlen, HDR_CAP - hlen, 0);
        if (n <= 0) return;                        // 超时/对端断开:静默关
        hlen += (size_t)n;
        for (size_t i = 0; i + 3 < hlen; i++) {
            if (memcmp(hdr + i, "\r\n\r\n", 4) == 0) { body_end = hdr + i + 4; break; }
        }
    }

    // 只认 POST /mcp(带或不带 query)。其余按错误关掉,不喂任何解析。
    const bool is_post = hlen > 10 && strncmp(hdr, "POST /mcp", 9) == 0 &&
                         (hdr[9] == ' ' || hdr[9] == '?');
    if (!is_post) {
        const bool is_get = hlen > 9 && strncmp(hdr, "GET /mcp", 8) == 0;
        send_resp(fd, is_get ? 405 : 404,
                  is_get ? "{\"error\":\"POST only\"}" : "{\"error\":\"not found\"}", 0);
        return;
    }

    const int cl = content_length_of(hdr, (size_t)(body_end - hdr));
    if (cl < 0) { send_resp(fd, 400, "{\"error\":\"missing Content-Length\"}", 0); return; }
    if (cl > BODY_CAP) { send_resp(fd, 413, "{\"error\":\"body too large\"}", 0); return; }

    static char body[BODY_CAP];    // 同上,串行服务静态安全
    size_t have = hlen - (size_t)(body_end - hdr);
    if (have > (size_t)cl) have = (size_t)cl;
    memmove(body, body_end, have);
    while (have < (size_t)cl) {
        const ssize_t n = recv(fd, body + have, (size_t)cl - have, 0);
        if (n <= 0) return;
        have += (size_t)n;
    }

    int status = 200;
    cJSON *resp = appfw_mcp_handle(body, have, &status);
    if (resp) {
        char *txt = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (txt) {
            send_resp(fd, status, txt, strlen(txt));
            cJSON_free(txt);
        } else {
            send_resp(fd, 500, "{\"error\":\"oom\"}", 0);
        }
    } else {
        send_resp(fd, status, NULL, 0);            // 通知:202 空体
    }
}

static void mcp_task(void *arg)
{
    (void)arg;
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket 失败,常驻服务未启动");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    const int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(MCP_PORT),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 2) < 0) {
        ESP_LOGE(TAG, "bind/listen :%d 失败,常驻服务未启动", MCP_PORT);
        close(fd);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    s_running = true;
    ESP_LOGI(TAG, "AI 常驻入口:POST http://<ip>:%d/mcp(%d 个工具)",
             MCP_PORT, appfw_mcp_tool_count());

    for (;;) {
        const int c = accept(fd, NULL, NULL);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        serve(c);
        shutdown(c, 0);
        close(c);
        // 栈水位平时只在 DEBUG 看;余量跌破半 KB 才升 WARNING(栈只加不减是
        // 教训:4096 曾真机打穿,现在 6144,峰值实测 ~4.5KB)。
        const unsigned wm = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        if (wm < 512) ESP_LOGW(TAG, "ai_mcp 栈余量告急:%u", wm);
        else ESP_LOGD(TAG, "ai_mcp 栈水位 %u", wm);
    }
}

void appfw_mcp_server_start(void)
{
    if (s_task || appfw_mcp_tool_count() == 0) return;
    if (xTaskCreate(mcp_task, "ai_mcp", STACK_SIZE, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        s_task = NULL;
    }
}

bool appfw_mcp_server_running(void) { return s_running; }
int appfw_mcp_server_port(void) { return MCP_PORT; }
