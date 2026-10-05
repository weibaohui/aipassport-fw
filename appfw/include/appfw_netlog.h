// appfw/include/appfw_netlog.h —— 网络日志(环形缓冲 + 可选 UDP syslog 推送)。
//
// 两个能力共享同一个 esp_log 钩子:
//   B(常开):所有 ESP_LOG 行落入 RAM 环形缓冲(丢最旧),MCP 工具
//           get_recent_logs 随时可取——AI 排查问题不需要任何采集设施。
//   A(按需):把每行以 syslog 格式 UDP 发到局域网接收端(fire-and-forget,
//           没人听就丢包,绝不阻塞日志路径、绝不积压内存)。目的地经
//           MCP set_netlog 或 NVS(netlog_on / netlog_dest)配置。
//
// 控制台输出不受影响(钩子透传给原 vprintf)。init 幂等;未 init 时
// 零开销、零内存。UDP 接收端示例:nc -kul 5514(macOS 免 sudo)或任意
// syslog 服务。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 安装日志钩子并建环形缓冲;读 NVS(netlog_on/netlog_dest)恢复推送配置。
// 之后日志照常上控制台,同时进缓冲(与可选的 UDP 推送)。
void appfw_netlog_init(void);

// 取最近 max_lines 行(时间正序,写入 out,自动按 cap 截断)。
// 返回实际行数;dropped 非空时带回因缓冲满被丢弃的行数(累计)。
int appfw_netlog_recent(char *out, size_t cap, int max_lines, uint32_t *dropped);

// 配置 UDP 推送并持久化。on=false 关闭推送(ip/port 忽略,存空)。
// 端口默认/常用 5514(标准 syslog 514 在桌面系统需 root 才能监听)。
bool appfw_netlog_push_configure(bool on, const char *ip, uint16_t port);

// UDP 推送当前是否在推送(目的地已配置)。
bool appfw_netlog_push_active(void);

// 每秒调用(框架 second_tick):把开机挂起的推送恢复延到联网稳定后。
void appfw_netlog_poll(void);

// 环形缓冲现状(状态页用):alive = 当前存活行数,dropped = 累计被挤掉的行数。
void appfw_netlog_stats(uint32_t *alive, uint32_t *dropped);

// 当前推送目的地 "ip:port"(未配置为空串)。buf ≥ 24。
void appfw_netlog_push_dest(char *buf, size_t cap);
