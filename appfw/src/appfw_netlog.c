// appfw/src/appfw_netlog.c —— 见 appfw_netlog.h。
#include "appfw_netlog.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "appfw_mcp.h"
#include "appfw_storage.h"

static const char *TAG = "appfw_netlog";

#define RING_CAP     (4 * 1024)   // 环形缓冲(init 时一次 malloc;约 25 行)
#define LINE_MAX     384          // 单行上限(ESP_LOG 行很少超过 200)
#define SYSLOG_PORT  5514

// ---- 环形缓冲(字节环 + 2 字节长度前缀的行;满则丢最旧) ----
static uint8_t *s_ring;                  // init 时分配,服务生命周期不释放
static uint32_t s_ring_wr;               // 绝对写位置(模 RING_CAP)
static uint32_t s_ring_rd;               // 绝对读点 = 最旧存活行的起点
static uint32_t s_ring_total;            // 已存入行数
static uint32_t s_ring_dropped;          // 因满被丢弃的行数
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

// 日志钩子被任意任务并发调用;自旋锁包住指针/统计。锁内零 IO。
static void ring_push(const char *line, size_t len)
{
    if (!s_ring || len == 0) return;
    if (len > LINE_MAX) len = LINE_MAX;

    portENTER_CRITICAL(&s_lock);
    // 腾空间:丢弃最旧行直到放得下(2B 长度 + 载荷)。丢多少行不看内容,
    // 从头"消费"长度即可——因为最旧的行一定从最旧的写点开始。
    const uint32_t need = (uint32_t)(2 + len);
    uint32_t used = s_ring_total == 0 ? 0 : (s_ring_wr - s_ring_rd) % RING_CAP;
    while (used + need > RING_CAP) {
        uint8_t hi = s_ring[s_ring_rd % RING_CAP];
        uint8_t lo = s_ring[(s_ring_rd + 1) % RING_CAP];
        const uint32_t elen = ((uint32_t)hi << 8) | lo;
        s_ring_rd += 2 + elen;
        s_ring_dropped++;
        used = (s_ring_wr - s_ring_rd) % RING_CAP;
    }
    const uint32_t p = s_ring_wr;
    s_ring[p % RING_CAP] = (uint8_t)(len >> 8);
    s_ring[(p + 1) % RING_CAP] = (uint8_t)(len & 0xFF);
    for (size_t k = 0; k < len; k++) {
        s_ring[(p + 2 + k) % RING_CAP] = (uint8_t)line[k];
    }
    s_ring_wr += need;
    s_ring_total++;
    portEXIT_CRITICAL(&s_lock);
}

// 环上任意区段拷到线性缓冲(行可能跨环回绕,必须分两段)。
static size_t ring_copy(char *dst, size_t cap, uint32_t abs_pos, size_t len)
{
    size_t off = 0;
    for (size_t k = 0; k < len && off + 1 < cap; k++) {
        dst[off++] = (char)s_ring[(abs_pos + k) % RING_CAP];
    }
    return off;
}

int appfw_netlog_recent(char *out, size_t cap, int max_lines, uint32_t *dropped)
{
    if (dropped) *dropped = s_ring_dropped;
    out[0] = '\0';
    if (!s_ring) return 0;
    if (max_lines < 1) max_lines = 8;
    if (max_lines > 32) max_lines = 32;

    // 正序扫一遍,滑动窗口保留最后 max_lines 行的 (绝对位置, 长度)。
    // 行数上限 ~RING_CAP/40(<200),全扫只是几十次 2 字节读,不值缓存。
    uint32_t pos[32];
    uint16_t len[32];
    int found = 0;
    portENTER_CRITICAL(&s_lock);
    for (uint32_t p = s_ring_rd; p < s_ring_wr; ) {
        const uint16_t elen = (uint16_t)(((uint16_t)s_ring[p % RING_CAP] << 8) |
                                          s_ring[(p + 1) % RING_CAP]);
        if (found < max_lines) {
            pos[found] = p;
            len[found] = elen;
            found++;
        } else {
            memmove(pos, pos + 1, sizeof(pos[0]) * (size_t)(found - 1));
            memmove(len, len + 1, sizeof(len[0]) * (size_t)(found - 1));
            pos[found - 1] = p;
            len[found - 1] = elen;
        }
        p += 2 + elen;
    }

    // 时间正序拼接到调用方缓冲(锁内短拷,行总量 <1KB)。
    size_t off = 0;
    for (int k = 0; k < found; k++) {
        if (off + 2 >= cap) break;
        off += ring_copy(out + off, cap - off - 1, pos[k] + 2, len[k]);
        out[off++] = '\n';
    }
    portEXIT_CRITICAL(&s_lock);
    out[off] = '\0';
    return found;
}

// ---- UDP syslog 推送 ----
static bool s_push_on;
static int s_fd = -1;
static struct sockaddr_in s_dest;
static char s_dest_str[24];
static bool s_in_send;                   // 递归闸(lwip 内部若打日志,不再外发)
static char s_restore_dest[24];          // 待恢复的推送目的地(开机 15s 后启用)
static int (*s_uart_vprintf)(const char *, va_list);

static void tx_sendto(const char *line, size_t len)
{
    if (s_fd < 0 || s_in_send) return;
    s_in_send = true;
    char pkt[LINE_MAX + 8];
    const int hn = snprintf(pkt, sizeof(pkt), "<14>esp32c3: ");
    size_t hl = (size_t)hn < sizeof(pkt) ? (size_t)hn : sizeof(pkt) - 1;
    if (hl + len > sizeof(pkt)) len = sizeof(pkt) - hl;
    memcpy(pkt + hl, line, len);
    (void)sendto(s_fd, pkt, hl + len, 0, (struct sockaddr *)&s_dest, sizeof(s_dest));
    s_in_send = false;
}

// ---- 投递队列 + 专职任务:socket 收发彻底离开日志调用上下文 ----
// WiFi 掉线时 tcpip/wifi 任务会发告警日志;若在那种上下文里同步 sendto,
// netconn 要等 tcpip 线程处理,而告警本身可能正由 tcpip 线程打出——
// 自己等自己,全系统冻结(真机抓到:日志戛然而止,无 panic)。
#define TXQ_LINES 10
static char s_txq[TXQ_LINES][LINE_MAX];
static uint16_t s_txlen[TXQ_LINES];
static uint8_t s_txq_wr, s_txq_rd, s_txq_n;      // 满则丢最旧(推送允许丢)
static TaskHandle_t s_tx_task;

static void tx_queue(const char *line, size_t len)
{
    if (!s_tx_task) return;                       // 未开启推送:无任务无队列

    if (len > LINE_MAX) len = LINE_MAX;
    portENTER_CRITICAL(&s_lock);
    if (s_txq_n == TXQ_LINES) {                   // 满:丢最旧
        s_txq_rd = (uint8_t)((s_txq_rd + 1) % TXQ_LINES);
        s_txq_n--;
    }
    memcpy(s_txq[s_txq_wr], line, len);
    s_txlen[s_txq_wr] = (uint16_t)len;
    s_txq_wr = (uint8_t)((s_txq_wr + 1) % TXQ_LINES);
    s_txq_n++;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_tx_task);
}

static void tx_worker(void *arg)
{
    (void)arg;
    char line[LINE_MAX];
    size_t len;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (s_txq_n) {
            portENTER_CRITICAL(&s_lock);
            if (s_txq_n == 0) { portEXIT_CRITICAL(&s_lock); break; }
            memcpy(line, s_txq[s_txq_rd], (size_t)s_txlen[s_txq_rd]);
            len = (size_t)s_txlen[s_txq_rd];
            s_txq_rd = (uint8_t)((s_txq_rd + 1) % TXQ_LINES);
            s_txq_n--;
            portEXIT_CRITICAL(&s_lock);
            tx_sendto(line, len);
        }
    }
}

// esp_log vprintf 钩子:格式化 → 进环形缓冲 → (可选)UDP → 透传控制台。
static int netlog_vprintf_hook(const char *fmt, va_list args)
{
    char line[LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n < 0) n = 0;
    if (n > LINE_MAX - 1) n = LINE_MAX - 1;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;   // 去行尾
    if (n > 0) {
        ring_push(line, (size_t)n);
        tx_queue(line, (size_t)n);      // 只拷贝+通知,绝不在日志路径碰 socket
    }
    return s_uart_vprintf ? s_uart_vprintf(fmt, args) : n;
}

// ---- 配置与持久化 ----
static void push_close(void)
{
    if (s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
    s_push_on = false;
    s_dest_str[0] = '\0';
}

// 投递任务按需建/删(推送关闭时不占任务栈)
static void tx_worker_task(void *arg)
{
    (void)arg;
    tx_worker(NULL);
}

static void tx_task_ensure(void)
{
    if (s_tx_task) return;
    if (xTaskCreate(tx_worker_task, "netlog_tx", 2560, NULL, 2, &s_tx_task) != pdPASS) {
        ESP_LOGW(TAG, "投递任务创建失败");
        s_tx_task = NULL;
    }
}

static void tx_task_kill(void)
{
    if (s_tx_task) {
        TaskHandle_t h = s_tx_task;
        s_tx_task = NULL;
        vTaskDelete(h);
    }
}

static void push_open(const char *ip, uint16_t port)
{
    push_close();
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_aton(ip, &d.sin_addr) == 0) {
        ESP_LOGW(TAG, "推送目的地不是合法 IPv4:%s", ip);
        return;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    const int bc = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bc, sizeof(bc));
    s_dest = d;
    s_fd = fd;
    s_push_on = true;
    snprintf(s_dest_str, sizeof(s_dest_str), "%s:%u", ip, (unsigned)port);
    ESP_LOGI(TAG, "UDP 日志推送 → %s", s_dest_str);
}

bool appfw_netlog_push_configure(bool on, const char *ip, uint16_t port)
{
    if (on) {
        if (!ip || !ip[0]) return false;
        if (port == 0) port = SYSLOG_PORT;
        char dest[24];
        snprintf(dest, sizeof(dest), "%s:%u", ip, (unsigned)port);
        (void)appfw_store_set_str("netlog_dest", dest);
        (void)appfw_store_set_u16("netlog_on", 1);
        tx_task_ensure();
        push_open(ip, port);
        return s_push_on;
    }
    (void)appfw_store_set_str("netlog_dest", "");
    (void)appfw_store_set_u16("netlog_on", 0);
    push_close();
    tx_task_kill();
    return true;
}

bool appfw_netlog_push_active(void) { return s_push_on; }

void appfw_netlog_stats(uint32_t *alive, uint32_t *dropped)
{
    portENTER_CRITICAL(&s_lock);
    if (alive) *alive = s_ring_total - s_ring_dropped;
    if (dropped) *dropped = s_ring_dropped;
    portEXIT_CRITICAL(&s_lock);
}

void appfw_netlog_push_dest(char *buf, size_t cap)
{
    snprintf(buf, cap, "%s", s_dest_str);
}

void appfw_netlog_init(void)
{
    static bool done;
    if (done) return;
    done = true;

    s_ring = malloc(RING_CAP);
    if (s_ring) memset(s_ring, 0, RING_CAP);
    s_ring_rd = 0;
    s_uart_vprintf = vprintf;            // 透传给控制台
    esp_log_set_vprintf(netlog_vprintf_hook);
    appfw_mcp_diag_tools_enable(true);   // 取日志/调级别/配推送,AI 全套可用
    ESP_LOGI(TAG, "网络日志就绪:环形缓冲 %d 行级缓存,UDP 推送待配置",
             RING_CAP / 160);

    // 恢复持久化的推送配置:不在开机最紧时刻展开(任务+socket 要 ~3KB),
    // 挂起等 appfw_netlog_poll 在联网稳定后启用。
    uint16_t on = 0;
    char dest[24];
    if (appfw_store_get_u16("netlog_on", &on, 0) && on == 1 &&
        appfw_store_get_str("netlog_dest", dest, sizeof(dest)) && dest[0]) {
        strlcpy(s_restore_dest, dest, sizeof(s_restore_dest));
        ESP_LOGI(TAG, "UDP 推送待恢复:%s(联网稳定后启用)", s_restore_dest);
    }
}

void appfw_netlog_poll(void)
{
    if (s_restore_dest[0] == '\0') return;
    int64_t now = esp_timer_get_time();
    if (now < 15LL * 1000000LL) return;              // 开机 15s 内堆最碎,不碰
    char dest[24];
    strlcpy(dest, s_restore_dest, sizeof(dest));
    s_restore_dest[0] = '\0';
    char *colon = strchr(dest, ':');
    if (colon) {
        *colon = '\0';
        tx_task_ensure();
        push_open(dest, (uint16_t)atoi(colon + 1));
    }
}
