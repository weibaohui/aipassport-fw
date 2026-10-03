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
    // 为 NULL 时框架沿用默认约定:上=home_up / 下=设置菜单 / OK单击=熄屏 / OK长按=配网。
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
    // 设置菜单的默认入口键:无 home_key 的应用在主页按此键进设置菜单。
    // 0=下键(默认,兼容既有行为)1=下键... 取值 0/1/2;0xFF=不设默认入口
    //(入口完全由应用接管:home_key 返回 APPFW_KEY_MENU,或调 appfw_ui_open_menu)。
    uint8_t menu_open_btn;
} appfw_ui_cfg_t;

// 初始化 UI(持 bsp_lvgl_lock 调用一次;内部建轮询定时器)。
void appfw_ui_init(const appfw_ui_cfg_t *cfg);

// 键事件入口(input 任务调用;0/1/2=上/下/OK;ev 直接传 bsp_button.h 的原始
// 事件,内部规整:按下瞬间只记活动,单击/双击/长按才进状态机)。
void appfw_ui_on_key(int btn, int ev);

// 打开设置菜单(基础功能:设置入口不再绑定固定按键,应用可在任意位置触发;
// 非 LVGL 任务上下文调用,内部自持锁;建议在 input 任务/home_key 回调里用)。
void appfw_ui_open_menu(void);

// 每秒维护(esp_timer 上下文):门户拉活/DNS 收撤 + 熄屏判定。
void appfw_ui_second_tick(void);

// 手动熄屏/唤醒(供按键或外部逻辑调用;非 LVGL 任务上下文)。
void appfw_screen_sleep(void);
void appfw_screen_wake(void);
