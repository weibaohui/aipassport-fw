// components/appfw/include/appfw_ui.h —— UI 骨架(通用):状态机/菜单/子页/熄屏。
//
// 应用提供配置:主页面标题与构建/轮询回调、信息页数据行、门户 HTML 注入片段、
// 应用配置 JSON 钩子。框架固定拥有:设置菜单(刷新周期/熄屏时间/WiFi 管理/
// 设备信息/配网/返回)、选项子页、WiFi 管理子页、配网子页、设备信息子页
// (键值行+应用配置行)、状态栏(电量+⚠)、熄屏/唤醒与静息超时。
// 框架每个页面自带「返回」行,统一光标交互(上下移光标,OK 执行;返回=回上级)。
//
// 按键约定(全部经 appfw_ui_on_key 规整):
//   主页面:上=手动刷新(回调);下=菜单;OK 单击=熄屏;任意键唤醒
//   菜单/子页:上下移光标;OK 执行/进入;末项「返回」=回上级
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

// 主页按键的处置结果(供 home_key 回调返回)。
typedef enum {
    APPFW_KEY_DEFAULT = 0, // 交回框架按默认约定处理
    APPFW_KEY_CONSUMED,    // 应用已处理,框架不再动作
    APPFW_KEY_MENU,        // 应用要求打开设置菜单
} appfw_key_action_t;

// 应用选项页描述符(见 appfw_ui_cfg_t::menu_opts):档位式应用设置
// (音量/亮度这类"一堆选项,选哪个就是哪个")交给框架渲染与持久化。
struct appfw_menu_opt {
    const char *key;              // NVS 键(appfw 命名空间),如 "opt_volume"
    const char *label;            // 设置菜单条目标题,如 "音量"
    const char *symbol;           // 菜单行图标(LV_SYMBOL_*,经 montserrat 回退
                                  // 渲染);NULL = 无图标
    const uint16_t *opts;         // 候选值数组
    const char *const *lbls;      // 候选显示文本;NULL = 按 "%u" 显示数值
    uint8_t count;                // 候选数(≤8;页高受 320 限制)
    void (*on_change)(uint16_t value);  // OK 选中即回调(应用负责让配置生效)
};
typedef struct appfw_menu_opt appfw_menu_opt_t;

typedef struct {
    const char *home_title;                 // 主页面标题
    void (*home_build)(lv_obj_t *page);     // 主页面构建(持锁调用一次;参数为页面 lv_obj_t*)
    void (*home_poll)(void);                // 主页面轮询(LVGL 任务,500ms)
    void (*home_up)(void);                  // 主页面上键(锁外;应用自定,如手动刷新)
    // 主页按键接管:需要列表选择、播放控制等多键交互的应用用它接管整个主页。
    // 锁外调用(input 任务上下文,与 home_up 相同);返回值决定框架是否继续动作。
    // 为 NULL 时框架沿用默认约定:上=home_up / 下=设置菜单 / OK单击=熄屏;
    // 长按动作不在默认约定里,走 long_press_up/long_press_down/long_press_ok 配置表。
    // 回调里不要直接改 UI 状态,需要重绘时让 home_poll 自然刷新或返回 APPFW_KEY_MENU。
    appfw_key_action_t (*home_key)(int btn, int ev);
    // 信息页数据行(框架渲染;框架先填自己的基础行,再把 keys/vals 推进到当前
    // 行数传入——应用从下标 0 追加、返回追加行数,勿假设拿到的是数组起点)
    int (*info_rows)(char (*keys)[16], char (*vals)[72], int max);
    // 门户阶段二页面:应用 HTML 注入片段(嵌入卡片,可 NULL)
    const char *(*app_config_html)(void);
    // 导入/应用配置保存:读 JSON root,处理应用自有字段,返回是否成功
    bool (*app_config_apply)(void *cjson_root);
    // 状态/导出回显:把应用配置字段加入 JSON 对象(可空操作)
    void (*app_config_fill)(void *cjson_obj);
    // 设备信息页「配置」区:应用定义要显示哪些配置项(名称+值)。
    // 返回行数(≤max);框架负责渲染。例:某凭据=已配置 / 某订阅=已配置。
    int (*config_rows)(char (*keys)[24], char (*vals)[72], int max);
    // ---- 应用选项页(基础功能):注册后设置菜单自动多出对应条目,最多 2 个
    // (菜单一屏行数所限)。选中条目进入通用选项列表,OK 选择即:存 NVS
    // (appfw 命名空间,descriptor.key)+ 回调 on_change + 吐司,然后回菜单。
    // 数组生命周期须与运行期一致(建议 static const)。
    const struct appfw_menu_opt *menu_opts;
    uint8_t menu_opts_count;                // 0..2
    // ---- 设置菜单:显示哪些框架自带项(基础功能) ----
    // 位掩码,默认 0 = 一项都不显示;想要哪项就用哪项的位(APPFW_MENU_ITEM_*,
    // APPFW_MENU_ITEM_ALL = 全部)。没显示的项连背后的运行行为也不会启动
    // (例如"配网"不显示 = 设备离线时也不会自动开启配网门户)。
    uint8_t menu_show_mask;
    // ---- 主页三个键"长按"时打开什么(基础功能) ----
    // 长按上键/下键/OK键各自一个动作(APPFW_LONG_PRESS_*),默认全 DO_NOTHING。
    // home_key 回调优先级更高:它返回 APPFW_KEY_DEFAULT 才轮到这张表。
    uint8_t long_press_up;
    uint8_t long_press_down;
    uint8_t long_press_ok;
    // 页面重建回调(基础功能):框架每次整体重建页面(进菜单/返回/切子页)
    // 时,旧页面对象连同应用挂在它上面的图层一起被删除——应用必须在把手上
    // 置空,否则定时器轮询会摸到悬空指针(use-after-free,曾致菜单页
    // "设备信息"等文字随机消失/白色块)。持锁调用,只清指针,别建对象。
    void (*page_reset)(void);
    // 设置菜单的默认入口键:无 home_key 的应用在主页按此键进设置菜单。
    // 0=下键(默认,兼容既有行为)1=下键... 取值 0/1/2;0xFF=不设默认入口
    //(入口完全由应用接管:home_key 返回 APPFW_KEY_MENU,或调 appfw_ui_open_menu)。
    uint8_t menu_open_btn;
} appfw_ui_cfg_t;

// 设置菜单内置行的使能位(appfw_ui_cfg_t::menu_show_mask 用)。默认全关、显式
// 打开:未使能的项不进菜单也不占任何运行行为(如未使能配网=离线也不自启
// 门户)。子系统的内存本来就是按需的(页面导航即建/删,门户/FAT 空闲自卸),
// 使能位买的是语义显式与行为一致。
typedef enum {
    APPFW_MENU_ITEM_REFRESH_PERIOD = 1 << 0,   // 刷新周期
    APPFW_MENU_ITEM_SCREEN_OFF    = 1 << 1,   // 熄屏时间
    APPFW_MENU_ITEM_WIFI_MANAGER    = 1 << 2,   // WiFi 管理
    APPFW_MENU_ITEM_DEVICE_INFO    = 1 << 3,   // 设备信息
    APPFW_MENU_ITEM_PROVISIONING    = 1 << 4,   // 配网
    APPFW_MENU_ITEM_AI_ADMIN       = 1 << 5,   // AI 管理(常驻入口的纯信息页)
    APPFW_MENU_ITEM_BRIGHTNESS     = 1 << 6,   // 屏幕亮度
    APPFW_MENU_ITEM_LOGS           = 1 << 7,   // 日志(网络日志的纯状态页)
    APPFW_MENU_ITEM_ALL     = 0xFF,
} appfw_menu_item_t;

// 主页三键"长按"动作表(appfw_ui_cfg_t::long_press_up/long_press_down/long_press_ok 用)。
// 默认 APPFW_LONG_PRESS_DO_NOTHING;应用按需声明"长按某键打开某个页",不再在 home_key
// 里逐键硬编码。APPOPT0/1 对应 menu_opts[0/1],未注册则无动作。
typedef enum {
    APPFW_LONG_PRESS_DO_NOTHING = 0,
    APPFW_LONG_PRESS_OPEN_MENU,
    APPFW_LONG_PRESS_OPEN_WIFI_MANAGER,
    APPFW_LONG_PRESS_OPEN_DEVICE_INFO,
    APPFW_LONG_PRESS_OPEN_PROVISIONING,
    APPFW_LONG_PRESS_OPEN_AI_ADMIN,           // AI 管理信息页(AI 入口常驻,页面只报地址)
    APPFW_LONG_PRESS_OPEN_APP_OPTION_1,
    APPFW_LONG_PRESS_OPEN_APP_OPTION_2,
} appfw_long_press_action_t;

// 应用屏幕亮度(0-100,直接作用于背光 PWM)。设置项保存、MCP 设置、
// 唤醒恢复都经它——BSP 背光调用收敛在 appfw_ui 一层(主机测试无 BSP)。
void appfw_ui_apply_brightness(uint8_t pct);

// 初始化 UI(持 bsp_lvgl_lock 调用一次;内部建轮询定时器)。
void appfw_ui_init(const appfw_ui_cfg_t *cfg);

// 键事件入口(input 任务调用;0/1/2=上/下/OK;ev 直接传 bsp_button.h 的原始
// 事件,内部规整:按下瞬间只记活动,单击/双击/长按才进状态机)。
void appfw_ui_on_key(int btn, int ev);

// 打开设置菜单(基础功能:设置入口不再绑定固定按键,应用可在任意位置触发;
// 非 LVGL 任务上下文调用,内部自持锁;建议在 input 任务/home_key 回调里用)。
void appfw_ui_open_menu(void);

// 直接打开应用选项页(如音量),不经设置菜单;返回键直接回应用主页。
// idx 为 menu_opts 下标,越界时兜底打开设置菜单。光标落在当前生效值上。
// 非 LVGL 任务上下文调用,内部自持锁。
void appfw_ui_open_app_option(int idx);

// 每秒维护(esp_timer 上下文):门户拉活/DNS 收撤 + 熄屏判定。
void appfw_ui_second_tick(void);

// 手动熄屏/唤醒(供按键或外部逻辑调用;非 LVGL 任务上下文)。
void appfw_screen_sleep(void);
void appfw_screen_wake(void);
