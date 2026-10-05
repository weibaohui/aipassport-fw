// components/appfw/src/appfw_ui.c —— UI 骨架实现,见 appfw_ui.h。
//
// 状态机:UI_MAIN(应用主页)→ UI_MENU(设置菜单)→ UI_SUB_*(框架固定子页)。
// 线程模型:按键在 input 任务持 bsp_lvgl_lock 改 UI,副作用在锁外;动态数据由
// LVGL 轮询定时器刷新;页面按状态整体重建(重建只发生在持锁路径)。
#include "appfw_ui.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "appfw_client.h"
#include "appfw_mcp.h"
#include "appfw_netlog.h"
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "bsp_battery.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static appfw_ui_cfg_t s_cfg;
static lv_font_t s_font16;
static lv_font_t s_font24;

// 中文默认字库由框架提供(appfw/fonts/:常见 3500 字全量,见该目录 README);
// 应用可用同名强符号覆盖为自有子集。链接锚点见 appfw/CMakeLists.txt 的 -u。
__attribute__((weak)) extern const lv_font_t app_font_16;
__attribute__((weak)) extern const lv_font_t app_font_24;

#define COL_BG 0x0E1116
#define COL_TEXT 0xE6E6E6
#define COL_DIM 0x8B98A5
#define COL_OK 0x35C26B
#define COL_WARN 0xE5A13D
#define COL_BAD 0xE5484D
#define COL_BAR 0x24303C
#define COL_CARD 0x171C24
#define COL_SEL_BG 0x1D4030
#define COL_TITLE 0xF2F5F7

#define IDLE_DEFAULT_S 300

typedef enum {
    UI_MAIN = 0, UI_MENU, UI_SUB_REFRESH, UI_SUB_SOFF, UI_SUB_BRIGHT, UI_SUB_WIFI,
    UI_SUB_PROV, UI_SUB_LOGS,
    UI_SUB_INFO, UI_SUB_APPOPT,
} ui_state_t;

typedef struct {
    lv_obj_t *page, *battery, *warn, *portal, *clock;
    lv_obj_t *rows[10];
    int row_count;
} ui_t;

static ui_t s_ui;
static const char *TAG = "appfw_ui";

static ui_state_t s_state = UI_MAIN;
static int s_menu_sel, s_opt_sel, s_wifi_sel, s_prov_sel, s_info_sel;
static int s_wifi_off;                 // WiFi 列表滚动窗口起点
static lv_obj_t *s_scr;
static int64_t s_last_input_us;
static atomic_bool s_screen_off;
static int s_prefs_age;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static const uint16_t REFRESH_OPTS[] = { 60, 300, 600, 900, 1800, 3600 };
static const char *REFRESH_LBL[] = { "1 分钟", "5 分钟", "10 分钟", "15 分钟", "30 分钟", "1 小时" };
#define REFRESH_N 6
static const uint16_t SOFF_OPTS[] = { 60, 300, 600, 900, 1800, 0 };
static const char *SOFF_LBL[] = { "1 分钟", "5 分钟", "10 分钟", "15 分钟", "30 分钟", "永不" };
#define SOFF_N 6
static const uint16_t BRIGHT_OPTS[] = { 10, 30, 50, 70, 100 };
static const char *BRIGHT_LBL[] = { "10%", "30%", "50%", "70%", "100%" };
#define BRIGHT_N 5
// AI 管理(设置菜单独立项):纯信息页——AI 入口(MCP)常驻在独立的极简
// TCP 服务里(见 appfw_mcp_srv),不随页面开关,本页只负责把地址告诉用户。
// 内置菜单行:页面目标 + 标签;builtin_hide 置位的项由 menu_rebuild_builtin
// 过滤掉(各项独立可配置,默认全显示)。
static const struct {
    ui_state_t page;
    const char *label;
} k_builtin[] = {
    { UI_SUB_REFRESH, LV_SYMBOL_REFRESH "  刷新周期" },
    { UI_SUB_SOFF,    LV_SYMBOL_BELL "  熄屏时间" },
    { UI_SUB_BRIGHT,  LV_SYMBOL_IMAGE "  亮度" },
    { UI_SUB_WIFI,    LV_SYMBOL_WIFI "  WiFi 管理" },
    { UI_SUB_INFO,    LV_SYMBOL_LIST "  设备信息" },
    { UI_SUB_PROV,    LV_SYMBOL_HOME "  配网" },
    { UI_SUB_LOGS,    LV_SYMBOL_EYE_OPEN "  日志" },
};
#define BUILTIN_TOTAL ((int)(sizeof(k_builtin) / sizeof(k_builtin[0])))
#define MENU_N (menu_rows())         // 兼容旧引用:可见内置项 + 应用项 + 返回行
static int s_builtin_n;                     // 本轮菜单可见的内置行数
static uint8_t s_builtin_idx[BUILTIN_TOTAL];// 可见行 → k_builtin 下标

// 依据 menu_show_mask 重建可见内置行(init 与每次进菜单时调用)。
static void menu_rebuild_builtin(void)
{
    s_builtin_n = 0;
    for (int b = 0; b < BUILTIN_TOTAL; b++) {
        if (s_cfg.menu_show_mask & (1u << b)) s_builtin_idx[s_builtin_n++] = (uint8_t)b;
    }
}
// 应用选项页的数值显示缓冲(菜单/子页渲染时从描述符格式化而来)。
static char s_appopt_lbls[8][12];
static uint8_t s_appopt_idx;         // 当前进入的应用选项页下标
static bool s_appopt_direct;         // 是否经 appfw_ui_open_app_option 直达(返回键回主页)
static uint8_t s_pending_opt;        // 待生效的应用选项(锁外执行 on_change)
static uint16_t s_pending_val;
static bool s_pending_fire;

static int menu_rows(void)
{
    return s_builtin_n + s_cfg.menu_opts_count + 1;
}
#define WIFI_PAGE_MAX 5  // WiFi 列表一屏最多行数(其余进入滚动窗口)

static char s_wifi_cache[10][33];
static int s_wifi_cache_n;

// ---------------------------------------------------------------- 工具

static void style_label(lv_obj_t *l, const lv_font_t *f, uint32_t color)
{
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

static lv_obj_t *make_bar(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BAR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_OK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    return bar;
}

static void set_bar_pct(lv_obj_t *bar, int pct)
{
    lv_bar_set_value(bar, pct > 0 ? pct : 0, LV_ANIM_OFF);
    uint32_t col = COL_OK;
    if (pct >= 90) col = COL_BAD;
    else if (pct >= 70) col = COL_WARN;
    lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
}

// ---------------------------------------------------------------- 吐司

static void toast_timer_cb(lv_timer_t *t)
{
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(t);
    s_toast_timer = NULL;
}

static void show_toast(const char *text) // 持锁调用
{
    if (!s_toast) {
        s_toast = lv_label_create(s_scr);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
        lv_obj_set_style_radius(s_toast, 12, 0);
        lv_obj_set_style_pad_hor(s_toast, 12, 0);
        lv_obj_set_style_pad_ver(s_toast, 5, 0);
        style_label(s_toast, &s_font16, 0xFFFFFF);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -34);
    }
    lv_label_set_text(s_toast, text);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast_timer = lv_timer_create(toast_timer_cb, 1200, NULL);
}

// ---------------------------------------------------------------- 页面构建

static void build_top_bar(lv_obj_t *page, const char *title)
{
    lv_obj_t *t = lv_label_create(page);
    style_label(t, &s_font24, COL_TITLE);
    lv_label_set_text(t, title);
    lv_obj_set_pos(t, 12, 8);

    lv_obj_t *ul = lv_obj_create(page);
    lv_obj_remove_style_all(ul);
    lv_obj_set_size(ul, 40, 3);
    lv_obj_set_pos(ul, 14, 40);
    lv_obj_set_style_bg_color(ul, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_radius(ul, 2, 0);

    // 状态栏时间:24 小时制 HH:mm(SNTP 对时前显示 "--:--")。
    s_ui.clock = lv_label_create(page);
    style_label(s_ui.clock, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.clock, 140, 14);
    lv_label_set_text(s_ui.clock, "--:--");

    s_ui.battery = lv_label_create(page);
    style_label(s_ui.battery, &s_font16, COL_DIM);
    lv_obj_set_pos(s_ui.battery, 190, 14);
    lv_label_set_text(s_ui.battery, "--");

    s_ui.warn = lv_label_create(page);
    lv_obj_set_style_text_font(s_ui.warn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ui.warn, lv_color_hex(COL_BAD), 0);
    lv_obj_set_pos(s_ui.warn, 158, 15);
    lv_label_set_text(s_ui.warn, LV_SYMBOL_WARNING);
    lv_obj_add_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_row_h(lv_obj_t *page, int y, int h, bool cursor,
                            const char *symbol, const char *text)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 216, h);
    lv_obj_set_pos(row, 12, y);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(cursor ? COL_SEL_BG : COL_CARD), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    if (cursor) {
        lv_obj_set_style_border_color(row, lv_color_hex(COL_OK), 0);
        lv_obj_set_style_border_width(row, 1, 0);
    } else {
        lv_obj_set_style_border_width(row, 0, 0);
    }
    lv_obj_t *sym = lv_label_create(row);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sym, lv_color_hex(cursor ? COL_OK : COL_DIM), 0);
    lv_label_set_text(sym, symbol);
    lv_obj_align(sym, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *lbl = lv_label_create(row);
    style_label(lbl, &s_font16, COL_TEXT);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 32, 0);
    return row;
}

static lv_obj_t *make_row(lv_obj_t *page, int y, bool cursor,
                          const char *symbol, const char *text)
{
    return make_row_h(page, y, 40, cursor, symbol, text);
}

// 光标移动后让视图跟随(容器原生滚动;行高 40 不压缩,整列表可滚)。
// 按内容坐标直接滚,布局计算前调用也可靠。
// 光标行滚入列表容器可视区(滚动只发生在容器内部,顶栏钉在页面不动)。
static void menu_follow_cursor(lv_obj_t *list)
{
    if (s_menu_sel < s_ui.row_count && s_ui.rows[s_menu_sel])
        lv_obj_scroll_to_view(s_ui.rows[s_menu_sel], LV_ANIM_OFF);
}

static void build_menu(lv_obj_t *page)
{
    menu_rebuild_builtin();
    const int rows = menu_rows();
    // 滚动限定在顶栏以下的列表容器(用户反馈:整页滚动会把状态栏滚走)。
    // 几何铁律(先算再写):列表视口 276px,行距 44、行高 36,超高内部滚动。
    lv_obj_t *list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 276);
    lv_obj_set_pos(list, 0, 44);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const int pitch = 44;                                // 行高不压缩且行间留 8px 空隙
    const int rh    = 36;
    for (int i = 0; i < rows; i++) {
        const char *lbl = LV_SYMBOL_LEFT "  返回";  // 返回行
        char opt_lbl[64];
        if (i < s_builtin_n) {
            lbl = k_builtin[s_builtin_idx[i]].label;
        } else if (i < rows - 1) {
            // 与内置行同构:图标嵌在文字开头(图标+两空格),整行从同一 x 起排,
            // 图标/文字才能与上下行严格对齐(独立图标槽的 x 会随内容漂移)。
            const struct appfw_menu_opt *o = &s_cfg.menu_opts[i - s_builtin_n];
            if (o->symbol) snprintf(opt_lbl, sizeof(opt_lbl), "%s  %s", o->symbol, o->label);
            else snprintf(opt_lbl, sizeof(opt_lbl), "  %s", o->label);
            lbl = opt_lbl;
        }
        lv_obj_t *row = make_row_h(list, 4 + i * pitch, rh, i == s_menu_sel, " ", lbl);
        lv_obj_t *arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
        s_ui.rows[i] = row;
    }
    s_ui.row_count = rows;
    menu_follow_cursor(list);                // 进菜单/移动光标后视图跟随
}

static void build_option_page(const uint16_t *opts, const char **lbls, int n, uint16_t current)
{
    for (int i = 0; i < n; i++) {
        bool cursor = (i == s_opt_sel);
        bool is_current = (opts[i] == current);
        char text[40];
        snprintf(text, sizeof(text), "%s %s", is_current ? LV_SYMBOL_OK : " ", lbls[i]);
        s_ui.rows[i] = make_row_h(s_ui.page, 46 + i * 36, 30, cursor,
                                  cursor ? LV_SYMBOL_RIGHT : " ", text);
    }
    s_ui.rows[n] = make_row(s_ui.page, 46 + n * 36, s_opt_sel == n,
                            LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = n + 1;
}

static void build_wifi_page(void)
{
    appfw_netlist_t list;
    bool have = appfw_store_netlist_load(&list);
    s_wifi_cache_n = 0;
    if (have) {
        for (uint8_t i = 0; i < list.count; i++) {
            strncpy(s_wifi_cache[i], list.items[i].ssid, sizeof(s_wifi_cache[0]) - 1);
            s_wifi_cache[i][sizeof(s_wifi_cache[0]) - 1] = '\0';
            s_wifi_cache_n++;
        }
    }
    int total = s_wifi_cache_n + 1; // 含返回行
    if (s_wifi_sel >= total) s_wifi_sel = total - 1;
    if (s_wifi_sel < 0) s_wifi_sel = 0;

    if (s_wifi_cache_n == 0) {
        lv_obj_t *empty = lv_label_create(s_ui.page);
        style_label(empty, &s_font16, COL_DIM);
        lv_label_set_text(empty, "暂无已存热点\n可在网页配网时添加");
        lv_obj_set_pos(empty, 14, 70);
        // 空列表同样保留返回行(框架每页统一),OK 即回菜单。
        s_ui.rows[0] = make_row_h(s_ui.page, 46 + WIFI_PAGE_MAX * 34, 28,
                                  s_wifi_sel == 0, LV_SYMBOL_LEFT, "返回");
        s_ui.row_count = 1;
        return;
    }

    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (s_wifi_off > s_wifi_cache_n - WIFI_PAGE_MAX) s_wifi_off = s_wifi_cache_n - WIFI_PAGE_MAX;
    if (s_wifi_off < 0) s_wifi_off = 0;
    if (s_wifi_sel < s_wifi_off) s_wifi_off = s_wifi_sel;
    if (s_wifi_sel >= s_wifi_off + WIFI_PAGE_MAX) s_wifi_off = s_wifi_sel - WIFI_PAGE_MAX + 1;

    for (int i = 0; i < WIFI_PAGE_MAX; i++) {
        int idx = s_wifi_off + i;
        if (idx >= s_wifi_cache_n) break;
        bool cursor = (idx == s_wifi_sel);
        bool current = (strcmp(st.cur_ssid, s_wifi_cache[idx]) == 0);
        char text[48];
        snprintf(text, sizeof(text), "%s %s",
                 current ? LV_SYMBOL_OK : " ", s_wifi_cache[idx]);
        make_row_h(s_ui.page, 46 + i * 34, 28, cursor,
                   cursor ? LV_SYMBOL_RIGHT : " ", text);
    }
    // 返回行:恒在底部(滚动窗口外,固定位置)。
    make_row(s_ui.page, 46 + WIFI_PAGE_MAX * 34, s_wifi_sel == s_wifi_cache_n,
             LV_SYMBOL_LEFT, "返回");
}

// 日志状态页(纯状态,无开关):UDP 推送是定向发给某个接收端的,配置走
// AI(MCP set_netlog),屏幕只负责让人看得见现状。
static void build_logs_page(void)
{
    uint32_t alive = 0, dropped = 0;
    appfw_netlog_stats(&alive, &dropped);
    char dest[24];
    appfw_netlog_push_dest(dest, sizeof(dest));

    lv_obj_t *l = lv_label_create(s_ui.page);
    style_label(l, &s_font16, COL_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 216);
    lv_obj_set_pos(l, 12, 60);
    if (dest[0]) {
        lv_label_set_text_fmt(l, "UDP 推送:开\n%s\n\n缓冲:已存 %u 行%s\n\n如需更改请使用 AI 设置",
                              dest, (unsigned)alive,
                              dropped ? "(有丢弃)" : "");
    } else {
        lv_label_set_text_fmt(l, "UDP 推送:关\n\n缓冲:已存 %u 行%s\n\n如需更改请使用 AI 设置",
                              (unsigned)alive,
                              dropped ? "(有丢弃)" : "");
    }
    s_ui.rows[0] = make_row(s_ui.page, 250, true, LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = 1;
}

static void build_prov_page(void)
{
    build_top_bar(s_ui.page, "配网");
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    const bool active = st.portal_active;

    // 两行光标:0 = 开启/关闭配网(随状态),1 = 返回。几何先算再写:
    // 关:行 40/84 + 提示 132;开:状态 46/66 + 码 90..194 + 注 198 + 行 222/264 ≤ 320。
    s_ui.row_count = 2;

    if (!active) {
        // 步骤 1:开启热点
        s_ui.rows[0] = make_row(s_ui.page, 40, s_prov_sel == 0, LV_SYMBOL_WIFI,
                                "1. 开启热点");
        s_ui.rows[1] = make_row(s_ui.page, 84, s_prov_sel == 1, LV_SYMBOL_LEFT, "返回");
        lv_obj_t *hint = lv_label_create(s_ui.page);
        style_label(hint, &s_font16, COL_DIM);
        lv_obj_set_pos(hint, 12, 132);
        lv_label_set_text(hint, "开启后本机断网,手机扫码连接");
        return;
    }

    // 顶部提示(用户定稿):怎么连(带热点名,太长就自然换成两行)+连上后去哪。
    // 几何:提示最多 3 行(42..99)+ 码 102..210 + 注 214 + 行 237/279 ≤ 320。
    const char *ap = st.ap_ssid[0] ? st.ap_ssid : "AI-WiFi";
    char apname[36];
    {
        size_t n = strlen(ap);
        const size_t cap = 30;                 // 超长热点名截断,防止提示挤出区域
        if (n > cap) {
            n = cap;
            while (n > 0 && ((unsigned char)ap[n] & 0xC0) == 0x80) n--; // 不劈开多字节字
        }
        snprintf(apname, sizeof(apname), "%.*s%s", (int)n, ap,
                 strlen(ap) > n ? "…" : "");
    }
    lv_obj_t *hint = lv_label_create(s_ui.page);
    style_label(hint, &s_font16, COL_TEXT);
    lv_obj_set_width(hint, 216);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(hint, 12, 42);
    lv_label_set_text_fmt(hint, "扫码或手动连接热点 %s\n访问 192.168.4.1 选热点",
                          apname);

    // 两个白底二维码面板。WIFI 串省略默认的 T:nopass,缩短负载
    // 降低 QR 版本(码点更大,手机好扫——真机反馈 84px 扫不上)。
    struct { int32_t x; const char *payload; const char *caption; } Q[2] = {
        { 12,  "http://192.168.4.1", "2. 扫码连热点" },
        { 126, "http://192.168.4.1", "3. 扫码开管理页" },
    };
    char wifiqr[48];
    snprintf(wifiqr, sizeof(wifiqr), "WIFI:S:%s;;",
             st.ap_ssid[0] ? st.ap_ssid : "AI-WiFi");
    Q[0].payload = wifiqr;

    for (int i = 0; i < 2; i++) {
        lv_obj_t *panel = lv_obj_create(s_ui.page);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, 108, 108);
        lv_obj_set_pos(panel, Q[i].x, 102);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0xF2F6FA), 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(panel, 8, 0);

        lv_obj_t *qr = lv_qrcode_create(panel);
        if (qr) {
            lv_qrcode_set_size(qr, 88);
            lv_qrcode_set_dark_color(qr, lv_color_hex(0x101418));
            lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
            lv_obj_set_pos(qr, 10, 10);
            if (lv_qrcode_update(qr, Q[i].payload, strlen(Q[i].payload)) != LV_RESULT_OK) {
                lv_obj_delete(qr);
                lv_obj_t *fb = lv_label_create(panel);
                style_label(fb, &s_font16, 0x101418);
                lv_obj_set_width(fb, 96);
                lv_label_set_long_mode(fb, LV_LABEL_LONG_WRAP);
                lv_label_set_text(fb, Q[i].payload);
            }
        }
        lv_obj_t *cap = lv_label_create(s_ui.page);
        style_label(cap, &s_font16, COL_TEXT);
        lv_obj_set_pos(cap, Q[i].x, 214);
        lv_label_set_text(cap, Q[i].caption);
    }

    // 步骤 4:关闭配网(开启后的唯一关闭入口,开关行已完成使命不再显示)
    s_ui.rows[0] = make_row(s_ui.page, 237, s_prov_sel == 0, LV_SYMBOL_CLOSE,
                            "4. 关闭配网");
    s_ui.rows[1] = make_row(s_ui.page, 279, s_prov_sel == 1, LV_SYMBOL_LEFT, "返回");
}

// 信息页行高与数据行上限:46 + 8 行×28 + 返回行 28 = 298 ≤ 320(几何铁律先算再写)。
#define INFO_ROW_H 28
#define INFO_DATA_MAX 8

// 键值行:容器卡片风格与其他子页一致;child 0=键名(光标行变绿,配合 refresh_rows_cursor)。
// ---- 设备信息:文字行全量渲染,容器可滚(与菜单同款滚动交互) ----

// 收集全部信息行(框架基础 + AI 地址 + 应用追加)。纯文字,值区整行宽。
static int info_collect(char (*keys)[16], char (*vals)[72], int max)
{
    const esp_app_desc_t *app = esp_app_get_description();
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    int n = 0;
    if (n < max) { snprintf(keys[n], 16, "应用"); snprintf(vals[n], 72, "%s", app->version); n++; }
    if (n < max) { snprintf(keys[n], 16, "框架"); snprintf(vals[n], 72, "%s", appfw_framework_version()); n++; }
    if (n < max) { snprintf(keys[n], 16, "WiFi"); snprintf(vals[n], 72, "%s(%d dBm)", st.cur_ssid, st.rssi); n++; }
    if (n < max) { snprintf(keys[n], 16, "IP"); snprintf(vals[n], 72, "%s", st.ip[0] ? st.ip : "未连接"); n++; }
    if (n < max) {
        snprintf(keys[n], 16, "AI 地址");
        snprintf(vals[n], 72, st.ip[0] ? "%s:%d/mcp" : "联网后可用",
                 st.ip, appfw_mcp_server_port());
        n++;
    }
    if (n < max) {
        const size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        const size_t free_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        snprintf(keys[n], 16, "运行内存");
        snprintf(vals[n], 72, "占用 %u/%uKB 最大块 %uKB",
                 (unsigned)((total - free_) / 1024), (unsigned)(total / 1024),
                 (unsigned)(largest / 1024));
        n++;
    }
    if (n < max) {
        const esp_partition_t *run = esp_ota_get_running_partition();
        nvs_stats_t ns = { 0 };
        const bool nvs_ok = (nvs_get_stats(NULL, &ns) == ESP_OK && ns.total_entries);
        snprintf(keys[n], 16, "存储内存");
        snprintf(vals[n], 72, "程序 %.2f/%.2fMB",
                 run ? (double)(appfw_storage_app_image_used(run) / (1024.0 * 1024.0)) : 0.0,
                 run ? (double)(run->size / (1024.0 * 1024.0)) : 0.0);
        if (nvs_ok) {
            snprintf(vals[n] + strlen(vals[n]), 72 - strlen(vals[n]),
                     " · NVS %u%%",
                     (unsigned)(ns.used_entries * 100 / ns.total_entries));
        }
        n++;
    }
    if (s_cfg.info_rows && n < max) n += s_cfg.info_rows(keys + n, vals + n, max - n);
    return n;
}

// 纯文字信息行:键(dem 色)在上,值(text 色)在下,整行宽可读。
static void make_text_row(lv_obj_t *page, int y, const char *k, const char *v)
{
    lv_obj_t *kl = lv_label_create(page);
    style_label(kl, &s_font16, COL_DIM);
    lv_obj_set_pos(kl, 12, y);
    lv_label_set_text(kl, k);
    lv_obj_t *vl = lv_label_create(page);
    style_label(vl, &s_font16, COL_TEXT);
    lv_obj_set_pos(vl, 12, y + 17);
    lv_obj_set_width(vl, 216);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(vl, v);
}

static void build_info_page(void)
{
    char keys[12][16], vals[12][72];
    const int total = info_collect(keys, vals, 12);

    int y = 44;
    for (int i = 0; i < total; i++) {
        make_text_row(s_ui.page, y, keys[i], vals[i]);
        y += 34;
    }
    // 容器可滚:内容超出屏高时上/下键滚动(LVGL 原生,滚到头自动 clamp)
}


// 重建当前状态页(持锁调用)。
static void rebuild_page(void)
{
    // 旧页面的删除会连带删掉应用挂在页面上的对象;应用通过 page_reset
    // 清掉自己的悬空把手,之后 home_build(UI_MAIN 重建时)再重新创建。
    if (s_cfg.page_reset) s_cfg.page_reset();
    if (s_ui.page) {
        lv_obj_delete(s_ui.page);
        memset(&s_ui, 0, sizeof(s_ui));
    }
    s_ui.page = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ui.page);
    lv_obj_set_size(s_ui.page, 240, 320);

    switch (s_state) {
    case UI_MAIN:
        build_top_bar(s_ui.page, s_cfg.home_title);
        if (s_cfg.home_build) s_cfg.home_build(s_ui.page);
        s_ui.portal = lv_label_create(s_ui.page);
        style_label(s_ui.portal, &s_font16, COL_WARN);
        lv_obj_set_width(s_ui.portal, 216);
        lv_label_set_long_mode(s_ui.portal, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(s_ui.portal, 12, 292);
        lv_label_set_text(s_ui.portal, "");
        break;
    case UI_MENU:
        build_top_bar(s_ui.page, "设置");
        build_menu(s_ui.page);
        break;
    case UI_SUB_REFRESH: {
        build_top_bar(s_ui.page, "刷新周期");
        uint16_t cur = 60;
        appfw_store_get_period(&cur);
        build_option_page(REFRESH_OPTS, REFRESH_LBL, REFRESH_N, cur);
        break;
    }
    case UI_SUB_BRIGHT: {
        uint16_t cur = 100;
        (void)appfw_store_get_brightness(&cur);
        build_option_page(BRIGHT_OPTS, BRIGHT_LBL, BRIGHT_N, cur);
        break;
    }
    case UI_SUB_SOFF: {
        build_top_bar(s_ui.page, "熄屏时间");
        uint16_t cur = 300;
        appfw_store_get_screen_off(&cur);
        build_option_page(SOFF_OPTS, SOFF_LBL, SOFF_N, cur);
        break;
    }
    case UI_SUB_APPOPT: {
        const struct appfw_menu_opt *o = &s_cfg.menu_opts[s_appopt_idx];
        build_top_bar(s_ui.page, o->label);
        for (int i = 0; i < o->count; i++) {
            if (o->lbls) snprintf(s_appopt_lbls[i], sizeof(s_appopt_lbls[i]), "%s", o->lbls[i]);
            else snprintf(s_appopt_lbls[i], sizeof(s_appopt_lbls[i]), "%u", (unsigned)o->opts[i]);
        }
        const char *lblp[8];
        for (int i = 0; i < o->count; i++) lblp[i] = s_appopt_lbls[i];
        uint16_t cur = o->opts[0];
        appfw_store_get_u16(o->key, &cur, o->opts[0]);
        build_option_page(o->opts, lblp, o->count, cur);
        break;
    }
    case UI_SUB_LOGS:
        build_top_bar(s_ui.page, "日志");
        build_logs_page();
        break;
    case UI_SUB_WIFI:
        build_top_bar(s_ui.page, "WiFi 管理");
        build_wifi_page();
        break;
    case UI_SUB_PROV:
        build_top_bar(s_ui.page, "配网");
        build_prov_page();
        break;
    case UI_SUB_INFO:
        build_top_bar(s_ui.page, "设备信息");
        build_info_page();
        break;
    }
}

static void refresh_rows_cursor(int sel)
{
    for (int i = 0; i < s_ui.row_count; i++) {
        lv_obj_t *row = s_ui.rows[i];
        if (!row) continue;
        bool cursor = (i == sel);
        lv_obj_set_style_bg_color(row, lv_color_hex(cursor ? COL_SEL_BG : COL_CARD), 0);
        if (cursor) lv_obj_set_style_border_width(row, 1, 0);
        else lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_t *sym = lv_obj_get_child(row, 0);
        if (sym) lv_obj_set_style_text_color(sym, lv_color_hex(cursor ? COL_OK : COL_DIM), 0);
    }
}

// ---------------------------------------------------------------- 轮询

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    appfw_net_status_t net;
    appfw_net_get_status(&net);
    if (s_ui.battery) {
        int soc = bsp_battery_soc();
        if (soc >= 0) lv_label_set_text_fmt(s_ui.battery, "%d%%", soc);
        else lv_label_set_text(s_ui.battery, "--");
    }
    if (s_ui.clock) {
        time_t now = time(NULL);
        if (now > 1000000000) { // 已对时(2001-09 之后)才显示,避免 1970 误导
            struct tm tm_utc, tm_local;
            gmtime_r(&now, &tm_utc);
            // 东八区(设备无时区配置,按国内使用固定 +8)
            time_t local = now + 8 * 3600;
            gmtime_r(&local, &tm_local);
            lv_label_set_text_fmt(s_ui.clock, "%02d:%02d",
                                  tm_local.tm_hour, tm_local.tm_min);
        } else {
            lv_label_set_text(s_ui.clock, "--:--");
        }
    }
    if (s_ui.warn) {
        if (net.state != APPFW_NET_ONLINE) lv_obj_clear_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_ui.warn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_state == UI_MAIN) {
        if (s_cfg.home_poll) s_cfg.home_poll();
    }
    if (s_state == UI_MAIN && s_ui.portal) {
        if (net.portal_active && net.state != APPFW_NET_ONLINE) {
            lv_label_set_text_fmt(s_ui.portal,
                                  "配网中:连接热点 %s,电脑打开 192.168.4.1",
                                  net.ap_ssid);
            lv_obj_clear_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void appfw_ui_apply_brightness(uint8_t pct)
{
    bsp_display_backlight(pct);
}

// ---------------------------------------------------------------- 熄屏/唤醒

void appfw_screen_wake(void)
{
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel) {
        (void)esp_lcd_panel_disp_sleep(panel, false);
        vTaskDelay(pdMS_TO_TICKS(30));
        (void)esp_lcd_panel_disp_on_off(panel, true);
    }
    lvgl_port_resume();
    uint16_t bl = 100;
    (void)appfw_store_get_brightness(&bl);
    appfw_ui_apply_brightness((uint8_t)bl);
    atomic_store(&s_screen_off, false);
}

void appfw_screen_sleep(void)
{
    bsp_display_backlight(0);
    (void)lvgl_port_stop();
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel) {
        (void)esp_lcd_panel_disp_on_off(panel, false);
        (void)esp_lcd_panel_disp_sleep(panel, true);
    }
    atomic_store(&s_screen_off, true);
}

// ---------------------------------------------------------------- 按键

void appfw_ui_on_key(int btn, int ev)
{
    s_last_input_us = esp_timer_get_time();

    // 事件规整:入参是 bsp 原始事件(PRESS=0 按下瞬间/CLICK=1 单击/DOUBLE=2/LONG=3)。
    // 按下瞬间只记活动、不进状态机——否则按下即响应,抬起后的 CLICK 再到会被当成
    // 第二次按键(设备信息页"进页即退"即此因);规整后 0=单击 2=双击 3=长按。
    if (ev == BSP_BTN_PRESS) return;
    if (ev == BSP_BTN_CLICK) ev = 0;
    else if (ev == BSP_BTN_DOUBLE) ev = 2;
    else if (ev == BSP_BTN_LONG) ev = 3;
    else return;

    if (atomic_load(&s_screen_off)) {
        appfw_screen_wake();
        return;
    }

    bool do_sleep = false;
    if (!bsp_lvgl_lock(300)) return;

    switch (s_state) {
    case UI_MAIN:
        // 应用接管了主页按键(列表选择/播放控制等多键交互的应用)。
        // 回调必须在锁外调用,因此先让出锁,再按返回结果决定框架动作。
        if (s_cfg.home_key) {
            bsp_lvgl_unlock();
            switch (s_cfg.home_key(btn, ev)) {
            case APPFW_KEY_CONSUMED:
                return;
            case APPFW_KEY_MENU:
                if (bsp_lvgl_lock(300)) {
                    s_state = UI_MENU;
                    rebuild_page();
                    bsp_lvgl_unlock(); // 提前返回前必须归还锁,否则 LVGL 任务饿死
                }
                return;
            case APPFW_KEY_DEFAULT:
            default:
                break;
            }
            if (!bsp_lvgl_lock(300)) return;
        }
        // 设置入口键应用可配置:0=默认下键,1/2=上/OK,0xFF=无默认入口。
        const int menu_btn = s_cfg.menu_open_btn ? (int)s_cfg.menu_open_btn : 1;
        if (menu_btn != 0xFF && btn == menu_btn && ev == 0) { // 进设置菜单
            s_state = UI_MENU;
            rebuild_page();
        } else if (btn == 2 && ev == 0) {
            do_sleep = true;
        } else if (ev == 0 && btn == 0) {
            if (s_cfg.home_up) { bsp_lvgl_unlock(); s_cfg.home_up(); return; }
        } else if (ev == 3) {
            // 主页长按动作表(long_press_up/long_press_down/long_press_ok):按需打开对应页。
            const uint8_t act = (btn == 0) ? s_cfg.long_press_up
                              : (btn == 1) ? s_cfg.long_press_down : s_cfg.long_press_ok;
            if (act != APPFW_LONG_PRESS_DO_NOTHING) {
                switch (act) {
                case APPFW_LONG_PRESS_OPEN_MENU: s_state = UI_MENU; break;
                case APPFW_LONG_PRESS_OPEN_WIFI_MANAGER: s_state = UI_SUB_WIFI; break;
                case APPFW_LONG_PRESS_OPEN_DEVICE_INFO: s_state = UI_SUB_INFO; break;
                case APPFW_LONG_PRESS_OPEN_PROVISIONING: s_state = UI_SUB_PROV; break;
                case APPFW_LONG_PRESS_OPEN_AI_ADMIN:   // 页面已并入设备信息
                    s_state = UI_SUB_INFO;
                    break;
                case APPFW_LONG_PRESS_OPEN_APP_OPTION_1:
                case APPFW_LONG_PRESS_OPEN_APP_OPTION_2: {
                    const uint8_t idx = (uint8_t)(act - APPFW_LONG_PRESS_OPEN_APP_OPTION_1);
                    if (idx >= s_cfg.menu_opts_count) break;   // 未注册:无动作
                    s_appopt_idx = idx;
                    s_appopt_direct = true;
                    const struct appfw_menu_opt *o = &s_cfg.menu_opts[idx];
                    uint16_t cur = o->opts[0];
                    appfw_store_get_u16(o->key, &cur, o->opts[0]);
                    s_opt_sel = 0;
                    for (int i = 0; i < o->count; i++)
                        if (o->opts[i] == cur) { s_opt_sel = i; break; }
                    s_state = UI_SUB_APPOPT;
                    break;
                }
                default: break;
                }
                rebuild_page();
            }
        }
        break;

    case UI_MENU:
        if (ev == 3) {
            s_state = UI_MAIN;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_menu_sel = (s_menu_sel + MENU_N - 1) % MENU_N;
            rebuild_page();
        } else if (ev == 0 && btn == 1) {
            s_menu_sel = (s_menu_sel + 1) % MENU_N;
            rebuild_page();
        } else if (ev == 0 && btn == 2) {
            if (s_menu_sel == menu_rows() - 1) s_state = UI_MAIN; // 返回行
            else if (s_menu_sel >= s_builtin_n &&
                     s_menu_sel < s_builtin_n + s_cfg.menu_opts_count) {
                // 应用选项页:只需记下是哪一个,进页后再选具体档位。
                s_appopt_idx = (uint8_t)(s_menu_sel - s_builtin_n);
                s_appopt_direct = false;
                s_opt_sel = 0; s_wifi_sel = 0; s_wifi_off = 0; s_prov_sel = 0;
                s_info_sel = 0;
                s_state = UI_SUB_APPOPT;
            } else {
                s_opt_sel = 0; s_wifi_sel = 0; s_wifi_off = 0; s_prov_sel = 0;
                s_state = k_builtin[s_builtin_idx[s_menu_sel]].page;
            }
            rebuild_page();
        }
        break;

    case UI_SUB_REFRESH:
    case UI_SUB_SOFF:
    case UI_SUB_BRIGHT:
    case UI_SUB_APPOPT: {
        const struct appfw_menu_opt *o = (s_state == UI_SUB_APPOPT)
                                             ? &s_cfg.menu_opts[s_appopt_idx] : NULL;
        const uint16_t *opts = o ? o->opts
                                 : (s_state == UI_SUB_REFRESH) ? REFRESH_OPTS
                                 : (s_state == UI_SUB_BRIGHT) ? BRIGHT_OPTS : SOFF_OPTS;
        int n = o ? o->count
                  : (s_state == UI_SUB_REFRESH) ? REFRESH_N
                  : (s_state == UI_SUB_BRIGHT) ? BRIGHT_N : SOFF_N;
        if (ev == 3) {
            s_state = s_appopt_direct ? UI_MAIN : UI_MENU;
            s_appopt_direct = false;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_opt_sel = (s_opt_sel + n) % (n + 1);
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == 1) {
            s_opt_sel = (s_opt_sel + 1) % (n + 1);
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_opt_sel == n) {
                s_state = s_appopt_direct ? UI_MAIN : UI_MENU;
                s_appopt_direct = false;
                rebuild_page();
            } else {
                bool ok;
                if (o) {
                    // 应用选项页:存 NVS;on_change 延迟到锁外执行 —— 回调里
                    // 可能有 I2C(音量)这类耗时/持锁敏感操作,不能在按键任务
                    // 持 LVGL 锁时做(曾疑似引发真机/仿真器崩溃)。
                    ok = appfw_store_set_u16(o->key, opts[s_opt_sel]);
                    ESP_LOGI(TAG, "选项[%u]保存 %u:%s", (unsigned)s_appopt_idx,
                             (unsigned)opts[s_opt_sel], ok ? "ok" : "fail");
                    if (ok && o->on_change) {
                        s_pending_opt = s_appopt_idx;
                        s_pending_val = opts[s_opt_sel];
                        s_pending_fire = true;
                    }
                } else {
                    if (s_state == UI_SUB_REFRESH) {
                        ok = appfw_store_set_period(opts[s_opt_sel]);
                    } else if (s_state == UI_SUB_SOFF) {
                        ok = appfw_store_set_screen_off(opts[s_opt_sel]);
                    } else if (s_state == UI_SUB_BRIGHT) {
                        ok = appfw_store_set_brightness(opts[s_opt_sel]);
                        if (ok) appfw_ui_apply_brightness((uint8_t)opts[s_opt_sel]);
                    } else {
                        ok = false;
                    }
                }
                s_state = UI_MENU;
                rebuild_page();
                show_toast(ok ? "已保存并生效" : "保存失败");
                if (!o) appfw_client_refresh_now();   // 刷新周期才需要立刻拉一次
            }
        }
        break;
    }

    case UI_SUB_LOGS:
        // 纯信息页:任意键离页,无服务开关(AI 入口常驻,与页面无关)。
        if (ev == 3 || (ev == 0 && (btn == 0 || btn == 1 || btn == 2))) {
            s_state = UI_MENU;
            rebuild_page();
        }
        break;

    case UI_SUB_WIFI: {
        int total = s_wifi_cache_n + 1;
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == 0 && total > 1) {
            s_wifi_sel = (s_wifi_sel + total - 1) % total;
            rebuild_page();
        } else if (ev == 0 && btn == 1 && total > 1) {
            s_wifi_sel = (s_wifi_sel + 1) % total;
            rebuild_page();
        } else if (ev == 0 && btn == 2) {
            if (s_wifi_sel == s_wifi_cache_n) {
                s_state = UI_MENU;
                rebuild_page();
            } else if (s_wifi_cache_n > 0) {
                appfw_net_connect_ssid(s_wifi_cache[s_wifi_sel]);
                rebuild_page();
                show_toast("正在连接,请稍候…");
            }
        }
        break;
    }

        case UI_SUB_PROV:
        // 两行光标:0 = 开启/关闭配网(随状态),1 = 返回
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && (btn == 0 || btn == 1)) {
            s_prov_sel = (s_prov_sel + 1) % 2; // 两行,上/下键都在 0/1 间切换
            refresh_rows_cursor(s_prov_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_prov_sel == 0) {
                appfw_net_status_t st2;
                appfw_net_get_status(&st2);
                if (st2.portal_active) appfw_net_stop_portal();
                else appfw_net_start_portal();
                rebuild_page();
                appfw_net_get_status(&st2);
                show_toast(st2.portal_active ? "热点已开启" : "热点已关闭");
            } else {
                s_state = UI_MENU;
                rebuild_page();
            }
        }
        break;

    case UI_SUB_INFO:
        // 滚动浏览:上/下滚内容(每按 ~90px,滚到头 clamp),OK 或长按返回。
        if (ev == 3 || (ev == 0 && btn == 2)) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            lv_obj_scroll_by(s_ui.page, 0, -90, LV_ANIM_OFF);
        } else if (ev == 0 && btn == 1) {
            lv_obj_scroll_by(s_ui.page, 0, 90, LV_ANIM_OFF);
        }
        break;
    }

    bsp_lvgl_unlock();

    // 应用选项的 on_change 在锁外执行:I2C(音量)等操作不与 LVGL 交叠。
    if (s_pending_fire) {
        s_pending_fire = false;
        const struct appfw_menu_opt *o = &s_cfg.menu_opts[s_pending_opt];
        ESP_LOGI(TAG, "on_change[%u] = %u", (unsigned)s_pending_opt,
                 (unsigned)s_pending_val);
        if (o->on_change) o->on_change(s_pending_val);
    }

    if (do_sleep) appfw_screen_sleep();
}

// 设置入口不再绑定固定按键:应用(如列表里的「设置」行、门户动作、长按组合)
// 在任意位置调用它进设置菜单。非 LVGL 任务上下文,内部自持锁。
void appfw_ui_open_menu(void)
{
    if (!bsp_lvgl_lock(300)) return;
    s_state = UI_MENU;
    s_menu_sel = 0;
    rebuild_page();
    bsp_lvgl_unlock();
}

// 直接进入应用选项页(如音量):光标落在当前生效档位,返回键回应用主页。
void appfw_ui_open_app_option(int idx)
{
    if (!bsp_lvgl_lock(300)) return;
    if (idx < 0 || idx >= s_cfg.menu_opts_count) {   // 越界兜底:开设置菜单
        s_appopt_direct = false;
        s_state = UI_MENU;
        s_menu_sel = 0;
        rebuild_page();
        bsp_lvgl_unlock();
        return;
    }
    s_appopt_idx = (uint8_t)idx;
    s_appopt_direct = true;
    const struct appfw_menu_opt *o = &s_cfg.menu_opts[idx];
    uint16_t cur = o->opts[0];
    appfw_store_get_u16(o->key, &cur, o->opts[0]);
    s_opt_sel = 0;
    for (int i = 0; i < o->count; i++) {
        if (o->opts[i] == cur) { s_opt_sel = i; break; }
    }
    s_state = UI_SUB_APPOPT;
    rebuild_page();
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------- 秒级维护

void appfw_ui_second_tick(void)
{
    // 配网完全手动(用户定稿 2026-10-05):不再自动进入,也不再自动关闭。
    // 门户/热点的生命周期只有两个出口——门户里点了连接,或设备上手动关闭。
    if (appfw_net_take_prov_done()) {
        // 手机在门户里点了连接:配网结束——回播放首页并提示(用户定稿)。
        if (bsp_lvgl_lock(300)) {
            s_state = UI_MAIN;
            s_prov_sel = 0;
            rebuild_page();
            show_toast("配网完成");
            bsp_lvgl_unlock();
        }
    }
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (!st.portal_active) appfw_portal_stop_dns();

    appfw_netlog_poll();                     // 网络日志推送的延迟恢复(无配置时空操作)
    if (++s_prefs_age >= 30) {
        s_prefs_age = 0;
        uint16_t off_s = IDLE_DEFAULT_S;
        appfw_store_get_screen_off(&off_s);
        if (off_s == 0) return;
        int64_t idle_us = esp_timer_get_time() - s_last_input_us;
        if (!atomic_load(&s_screen_off) && idle_us > (int64_t)off_s * 1000000LL) {
            appfw_screen_sleep();
        }
    }
}

void appfw_ui_init(const appfw_ui_cfg_t *cfg)
{
    menu_rebuild_builtin();
    // 应用选项页最多 2 个:菜单一屏(几何铁律)放不下更多。
    appfw_ui_cfg_t c = *cfg;
    if (c.menu_opts_count > 2) {
        ESP_LOGW(TAG, "menu_opts_count=%u 超过上限 3,多余忽略", (unsigned)c.menu_opts_count);
        c.menu_opts_count = 3;
    }
    cfg = &c;
    s_cfg = *cfg;
    s_font16 = app_font_16;
    s_font16.fallback = &lv_font_montserrat_14;
    s_font24 = app_font_24;
    s_font24.fallback = &lv_font_montserrat_20;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_screen_load(s_scr);

    s_state = UI_MAIN;
    s_menu_sel = 0;
    rebuild_page();
    s_last_input_us = esp_timer_get_time();
    atomic_init(&s_screen_off, false);
    lv_timer_create(poll_timer_cb, 500, NULL);

    // 已存亮度开机即生效(未存=100);在 LVGL 初始化之后调用,不会被
    // 显示初始化的默认背光覆盖。
    uint16_t bl = 100;
    (void)appfw_store_get_brightness(&bl);
    bsp_display_backlight((uint8_t)bl);

    // AI 常驻入口:应用注册了 MCP 工具才拉起(没注册零开销);启动一次,
    // 之后与页面/门户状态无关——AI 随时可管设备。
    appfw_mcp_set_brightness_apply(appfw_ui_apply_brightness);
    appfw_mcp_server_start();
}
