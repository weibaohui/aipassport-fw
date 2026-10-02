// components/appfw/include/appfw_ui.h —— UI 骨架(通用):状态机/菜单/子页/熄屏。
//
// 应用提供配置:主页面标题与构建/轮询回调、信息页数据行、门户 HTML 注入片段、
// 应用配置 JSON 钩子。框架固定拥有:设置菜单(刷新周期/熄屏时间/WiFi 管理/
// 设备信息/配网/返回)、选项子页、WiFi 管理子页、配网子页、状态栏(电量+⚠)、
// 熄屏/唤醒与静息超时。
//
// 按键约定(全部经 appfw_ui_on_key 规整):
//   主页面:上=手动刷新(回调);下=菜单;OK 单击=熄屏;任意键唤醒
//   菜单/子页:上下移光标;OK 执行/进入;末项「返回」=回上级
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct {
    const char *home_title;                 // 主页面标题
    void (*home_build)(lv_obj_t *page);     // 主页面构建(持锁调用一次;参数为页面 lv_obj_t*)
    void (*home_poll)(void);                // 主页面轮询(LVGL 任务,500ms)
    void (*home_up)(void);                  // 主页面上键(锁外;应用自定,如手动刷新)
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
    // 返回行数(≤max);框架负责渲染。例:API Key=已配置 / 团队=已配置。
    int (*config_rows)(char (*keys)[24], char (*vals)[72], int max);
} appfw_ui_cfg_t;

// 初始化 UI(持 bsp_lvgl_lock 调用一次;内部建轮询定时器)。
void appfw_ui_init(const appfw_ui_cfg_t *cfg);

// 键事件入口(input 任务调用;0/1/2=上/下/OK;ev 直接传 bsp_button.h 的原始
// 事件,内部规整:按下瞬间只记活动,单击/双击/长按才进状态机)。
void appfw_ui_on_key(int btn, int ev);

// 每秒维护(esp_timer 上下文):门户拉活/DNS 收撤 + 熄屏判定。
void appfw_ui_second_tick(void);

// 手动熄屏/唤醒(供按键或外部逻辑调用;非 LVGL 任务上下文)。
void appfw_screen_sleep(void);
void appfw_screen_wake(void);
