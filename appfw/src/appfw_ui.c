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
#include "appfw_net.h"
#include "appfw_netlist.h"
#include "appfw_portal.h"
#include "appfw_storage.h"
#include "bsp_battery.h"
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

// 中文字库由应用资产提供(生成管线见 assets/fonts);框架弱依赖,
// 未链接时回退 Montserrat(英文界面仍可用)。
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
    UI_MAIN = 0, UI_MENU, UI_SUB_REFRESH, UI_SUB_SOFF, UI_SUB_WIFI, UI_SUB_PROV,
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
static const char *MENU_LBL[] = {
    LV_SYMBOL_REFRESH "  刷新周期",
    LV_SYMBOL_BELL "  熄屏时间",
    LV_SYMBOL_WIFI "  WiFi 管理",
    LV_SYMBOL_LIST "  设备信息",
    LV_SYMBOL_HOME "  配网",
    LV_SYMBOL_LEFT "  返回",
};
#define MENU_BUILTIN 5               // 固定条目数(返回行之前)
#define MENU_N (menu_rows())         // 兼容旧引用:固定项 + 应用项 + 返回行
// 应用选项页的数值显示缓冲(菜单/子页渲染时从描述符格式化而来)。
static char s_appopt_lbls[8][12];
static uint8_t s_appopt_idx;         // 当前进入的应用选项页下标
static uint8_t s_pending_opt;        // 待生效的应用选项(锁外执行 on_change)
static uint16_t s_pending_val;
static bool s_pending_fire;

static int menu_rows(void)
{
    return MENU_BUILTIN + s_cfg.menu_opts_count + 1;
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

static void build_menu(lv_obj_t *page)
{
    const int rows = menu_rows();
    // 几何铁律(先算再写):48 起排,行高 ≤ 行距,整页 ≤ 320。
    // ≤6 行维持 40px;7 行 36px;8 行 32px(应用选项最多 2 个,不会更多)。
    const int pitch = rows > 7 ? 32 : (rows > 6 ? 36 : 40);
    const int rh    = rows > 7 ? 30 : (rows > 6 ? 34 : 40);
    for (int i = 0; i < rows; i++) {
        const char *lbl = MENU_LBL[MENU_BUILTIN];   // 返回行
        char opt_lbl[64];
        if (i < MENU_BUILTIN) {
            lbl = MENU_LBL[i];
        } else if (i < rows - 1) {
            // 与内置行同构:图标嵌在文字开头(图标+两空格),整行从同一 x 起排,
            // 图标/文字才能与上下行严格对齐(独立图标槽的 x 会随内容漂移)。
            const struct appfw_menu_opt *o = &s_cfg.menu_opts[i - MENU_BUILTIN];
            if (o->symbol) snprintf(opt_lbl, sizeof(opt_lbl), "%s  %s", o->symbol, o->label);
            else snprintf(opt_lbl, sizeof(opt_lbl), "  %s", o->label);
            lbl = opt_lbl;
        }
        lv_obj_t *row = make_row_h(page, 48 + i * pitch, rh, i == s_menu_sel, " ", lbl);
        lv_obj_t *arrow = lv_label_create(row);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -10, 0);
        s_ui.rows[i] = row;
    }
    s_ui.row_count = rows;
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

static void build_prov_page(void)
{
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    int y = 52;
    const struct { const char *k; char v[72]; } rows[] = {
        { "状态", { 0 } },
        { "热点", { 0 } },
        { "管理页", { 0 } },
        { "已连设备", { 0 } },
        { "本机 IP", { 0 } },
    };
    snprintf((char *)rows[0].v, sizeof(rows[0].v), "%s", st.portal_active ? "已开启" : "未开启");
    snprintf((char *)rows[1].v, sizeof(rows[1].v), "%s", st.ap_ssid);
    snprintf((char *)rows[2].v, sizeof(rows[2].v), "http://192.168.4.1");
    wifi_sta_list_t sta;
    int n = -1;
    if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) n = (int)sta.num;
    snprintf((char *)rows[3].v, sizeof(rows[3].v), n >= 0 ? "%d" : "--", n);
    snprintf((char *)rows[4].v, sizeof(rows[4].v), "%s", st.ip[0] ? st.ip : "未连接");

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *k = lv_label_create(s_ui.page);
        style_label(k, &s_font16, COL_DIM);
        lv_label_set_text(k, rows[i].k);
        lv_obj_set_pos(k, 14, y);
        lv_obj_t *v = lv_label_create(s_ui.page);
        style_label(v, &s_font16, COL_TEXT);
        lv_obj_set_width(v, 140);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_label_set_text(v, rows[i].v);
        lv_obj_set_pos(v, 96, y);
        y += 27;
    }
    s_ui.rows[0] = make_row(s_ui.page, y + 8, s_prov_sel == 0, LV_SYMBOL_RIGHT,
                            st.portal_active ? "关闭配网" : "开启配网");
    s_ui.rows[1] = make_row(s_ui.page, y + 58, s_prov_sel == 1, LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = 2;
}

// 信息页行高与数据行上限:46 + 8 行×28 + 返回行 28 = 298 ≤ 320(几何铁律先算再写)。
#define INFO_ROW_H 28
#define INFO_DATA_MAX 8

// 键值行:容器卡片风格与其他子页一致;child 0=键名(光标行变绿,配合 refresh_rows_cursor)。
static lv_obj_t *make_info_row(lv_obj_t *page, int y, bool cursor,
                               const char *k, const char *v)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 216, INFO_ROW_H);
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
    lv_obj_t *kl = lv_label_create(row);
    style_label(kl, &s_font16, COL_DIM);
    lv_label_set_text(kl, k);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 10, 0);
    // URL 用 Montserrat(防换行重叠);非 URL 值(如"联网后可用")是中文,必须用中文字库
    bool is_url = (strncmp(v, "http", 4) == 0);
    lv_obj_t *vl = lv_label_create(row);
    style_label(vl, is_url ? &lv_font_montserrat_14 : &s_font16, COL_TEXT);
    lv_obj_set_width(vl, is_url ? 150 : 118);
    lv_obj_set_height(vl, 20);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
    lv_label_set_text(vl, v);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, -10, 0);
    return row;
}

static void build_info_page(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    appfw_net_status_t st;
    appfw_net_get_status(&st);

    char keys[INFO_DATA_MAX][16];
    char vals[INFO_DATA_MAX][72];
    int n = 0;
    snprintf(keys[n], 16, "固件");
    snprintf(vals[n], 72, "%s", app->version); n++;
    snprintf(keys[n], 16, "WiFi");
    snprintf(vals[n], 72, "%s(%d dBm)", st.cur_ssid, st.rssi); n++;
    snprintf(keys[n], 16, "IP");
    snprintf(vals[n], 72, "%s", st.ip[0] ? st.ip : "未连接"); n++;
    snprintf(keys[n], 16, "管理地址");
    snprintf(vals[n], 72, "%s", st.ip[0] ? st.ip : "联网后可用"); n++;
    // 指针推进到当前行数再交给应用:应用从 0 追加,否则会覆盖框架行,
    // 且 n+=返回值 后多出的槽位是未初始化栈垃圾(空键名/残留值)。
    if (s_cfg.info_rows) n += s_cfg.info_rows(keys + n, vals + n, INFO_DATA_MAX - n);
    if (n > INFO_DATA_MAX) n = INFO_DATA_MAX;
    if (s_info_sel < 0) s_info_sel = 0;

    int y = 46;
    int total = 0; // 数据行与返回行的统一光标序号
    for (int i = 0; i < n; i++) {
        s_ui.rows[total] = make_info_row(s_ui.page, y, total == s_info_sel,
                                         keys[i], vals[i]);
        total++;
        y += INFO_ROW_H;
    }
    // 应用配置行直接用原数组渲染(键宽 24,收窄拷贝既截断又触编译警告)。
    if (s_cfg.config_rows) {
        char ckeys[4][24], cvals[4][72];
        int cn = s_cfg.config_rows(ckeys, cvals, 4);
        for (int i = 0; i < cn && total < INFO_DATA_MAX; i++) {
            s_ui.rows[total] = make_info_row(s_ui.page, y, total == s_info_sel,
                                             ckeys[i], cvals[i]);
            total++;
            y += INFO_ROW_H;
        }
    }
    // 返回行:框架每页统一自带(设备信息页此前靠"任意键返回",现改为标准光标交互)。
    if (s_info_sel > total) s_info_sel = total;
    s_ui.rows[total] = make_row_h(s_ui.page, y, INFO_ROW_H, s_info_sel == total,
                                  LV_SYMBOL_LEFT, "返回");
    s_ui.row_count = total + 1;
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
    bsp_display_backlight(100);
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
        }
        break;

    case UI_MENU:
        if (ev == 3) {
            s_state = UI_MAIN;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_menu_sel = (s_menu_sel + MENU_N - 1) % MENU_N;
            refresh_rows_cursor(s_menu_sel);
        } else if (ev == 0 && btn == 1) {
            s_menu_sel = (s_menu_sel + 1) % MENU_N;
            refresh_rows_cursor(s_menu_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_menu_sel == menu_rows() - 1) s_state = UI_MAIN; // 返回行
            else if (s_menu_sel >= MENU_BUILTIN &&
                     s_menu_sel < MENU_BUILTIN + s_cfg.menu_opts_count) {
                // 应用选项页:只需记下是哪一个,进页后再选具体档位。
                s_appopt_idx = (uint8_t)(s_menu_sel - MENU_BUILTIN);
                s_opt_sel = 0; s_wifi_sel = 0; s_wifi_off = 0; s_prov_sel = 0;
                s_info_sel = 0;
                s_state = UI_SUB_APPOPT;
            } else {
                s_opt_sel = 0; s_wifi_sel = 0; s_wifi_off = 0; s_prov_sel = 0;
                s_info_sel = 0;
                s_state = (s_menu_sel == 0) ? UI_SUB_REFRESH
                        : (s_menu_sel == 1) ? UI_SUB_SOFF
                        : (s_menu_sel == 2) ? UI_SUB_WIFI
                        : (s_menu_sel == 3) ? UI_SUB_INFO : UI_SUB_PROV;
            }
            rebuild_page();
        }
        break;

    case UI_SUB_REFRESH:
    case UI_SUB_SOFF:
    case UI_SUB_APPOPT: {
        const struct appfw_menu_opt *o = (s_state == UI_SUB_APPOPT)
                                             ? &s_cfg.menu_opts[s_appopt_idx] : NULL;
        const uint16_t *opts = o ? o->opts
                                 : (s_state == UI_SUB_REFRESH) ? REFRESH_OPTS : SOFF_OPTS;
        int n = o ? o->count : (s_state == UI_SUB_REFRESH) ? REFRESH_N : SOFF_N;
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_opt_sel = (s_opt_sel + n) % (n + 1);
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == 1) {
            s_opt_sel = (s_opt_sel + 1) % (n + 1);
            refresh_rows_cursor(s_opt_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_opt_sel == n) {
                s_state = UI_MENU;
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
                    ok = (s_state == UI_SUB_REFRESH)
                             ? appfw_store_set_period(opts[s_opt_sel])
                             : appfw_store_set_screen_off(opts[s_opt_sel]);
                }
                s_state = UI_MENU;
                rebuild_page();
                show_toast(ok ? "已保存并生效" : "保存失败");
                if (!o) appfw_client_refresh_now();   // 刷新周期才需要立刻拉一次
            }
        }
        break;
    }

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
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_prov_sel = (s_prov_sel + 1) % 2;
            refresh_rows_cursor(s_prov_sel);
        } else if (ev == 0 && btn == 1) {
            s_prov_sel = (s_prov_sel + 1) % 2;
            refresh_rows_cursor(s_prov_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_prov_sel == 1) {
                s_state = UI_MENU;
                rebuild_page();
            } else {
                appfw_net_status_t st;
                appfw_net_get_status(&st);
                if (st.portal_active) appfw_net_stop_portal();
                else appfw_net_start_portal();
                rebuild_page();
                show_toast(st.portal_active ? "配网已关闭" : "配网已开启");
            }
        }
        break;

    case UI_SUB_INFO:
        // 与其他子页一致的光标交互:上下移动,OK 仅在「返回」行生效(数据行无动作)。
        if (ev == 3) {
            s_state = UI_MENU;
            rebuild_page();
        } else if (ev == 0 && btn == 0) {
            s_info_sel = (s_info_sel + s_ui.row_count - 1) % s_ui.row_count;
            refresh_rows_cursor(s_info_sel);
        } else if (ev == 0 && btn == 1) {
            s_info_sel = (s_info_sel + 1) % s_ui.row_count;
            refresh_rows_cursor(s_info_sel);
        } else if (ev == 0 && btn == 2) {
            if (s_info_sel == s_ui.row_count - 1) {
                s_state = UI_MENU;
                rebuild_page();
            }
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

// ---------------------------------------------------------------- 秒级维护

void appfw_ui_second_tick(void)
{
    if (!appfw_portal_running()) (void)appfw_portal_start();
    appfw_net_status_t st;
    appfw_net_get_status(&st);
    if (!st.portal_active) appfw_portal_stop_dns();

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
    // 应用选项页最多 2 个:菜单一屏(几何铁律)放不下更多。
    appfw_ui_cfg_t c = *cfg;
    if (c.menu_opts_count > 2) {
        ESP_LOGW(TAG, "menu_opts_count=%u 超过上限 2,多余忽略", (unsigned)c.menu_opts_count);
        c.menu_opts_count = 2;
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

}
