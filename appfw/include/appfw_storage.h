// components/appfw/include/appfw_storage.h —— NVS 配置存储(通用)。
//
// 命名空间 "appfw";框架自有键:period_s(刷新周期)/screen_off_s(熄屏)/brightness(亮度)/
// nets+sel_ssid(热点列表)。应用自有键用 appfw_store_get_str/set_str 等通用
// API 存取(如 api_key/org_id/proj_id)。所有 blob 缓冲走堆,防调用方栈溢出。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "appfw_netlist.h"

// 初始化 NVS。分区新建/版本变化时擦除重试;其他错误原样返回。
int appfw_store_init(void);

// ---- 通用键值(应用自有配置) ----
bool appfw_store_get_str(const char *key, char *buf, size_t buf_len); // 未存=false(buf 置空)
bool appfw_store_set_str(const char *key, const char *value);        // 空串=清除
bool appfw_store_get_u16(const char *key, uint16_t *out, uint16_t fallback);
bool appfw_store_set_u16(const char *key, uint16_t value);

// ---- 框架设置:刷新周期(秒,合法 60/300/600/900/1800/3600;默认 60) ----
bool appfw_store_get_period(uint16_t *period_s);
bool appfw_store_set_period(uint16_t period_s);
// ---- 框架设置:熄屏超时(秒,合法 0/60/300/600/900/1800;0=永不;默认 300) ----
bool appfw_store_get_screen_off(uint16_t *screen_off_s);
bool appfw_store_set_screen_off(uint16_t screen_off_s);

// 屏幕亮度 0-100(get 未存=100;set 仅收档位 10/30/50/70/100)。
bool appfw_store_get_brightness(uint16_t *pct);
bool appfw_store_set_brightness(uint16_t pct);

// ---- 已保存热点列表(列表纯逻辑见 appfw_netlist.h) ----
// 屏幕亮度(档位 10/30/50/70/100;get 未存=100)。
bool appfw_store_get_brightness(uint16_t *pct);
bool appfw_store_set_brightness(uint16_t pct);

bool appfw_store_netlist_load(appfw_netlist_t *list);  // 无记录/损坏=false(list 复位)
bool appfw_store_netlist_save(const appfw_netlist_t *list); // 含点选状态

// 清空框架与应用全部配置(门户"清除配置"用)。
bool appfw_store_clear_all(void);

// ---- 存储占用(信息页/诊断用) ----
// App 镜像在分区中的实际占用(镜像头+段头链+校验尾,近似值)。p 用
// esp_ota_get_running_partition() 取;读取失败返回 0。
uint32_t appfw_storage_app_image_used(const void *p);
