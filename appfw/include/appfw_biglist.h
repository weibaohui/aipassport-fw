#pragma once
// appfw/include/appfw_biglist.h —— 大清单:files 分区整份清单 + 偏移索引,
// 外加"内置台目"兜底(2026-10-04 自 aipassport-radio 上移)。
//
// 场景:上千条目的清单装不进 NVS(24KB),放 1MB FAT files 分区(门户可
// 上传)。RAM 只留条数;读取走"索引 4B + 文件两行",FATFS 上 1-3ms。
// 机制要点(全部真机踩坑换来,勿删):
//   - 索引有效性 = 魔数/版本/条数自洽且记录的文件字节数一致;不一致自动重建;
//   - 重建前要求文件大小连续两轮一致(上传中不动手);
//   - 只有网络稳定(ONLINE/掉线重试/开机 8 秒)才碰 FAT——连接窗口堆最碎,
//     挂载会借走播放器的大块预留;
//   - FAT 挂载要 ~8KB 连续堆:不够先请应用让位(内存钩子),避免"先失败
//     一次"——失败的挂载会漏磨损均衡句柄(上限 8,漏光瘫痪到重启);
//   - 空闲 10s 卸载 FAT 并归还借出的内存;
//   - 无文件时落回"内置台目"(应用经 set_catalog 注册,纯 rodata 零 RAM),
//     available() 恒真——设备永远处于大清单模式,NVS 小清单路径不会复活。
//
// 清单是"名称\tURL"两行的播放列表形态,尺寸与 appfw_m3u 的上限一致。
#pragma once

#include <stdbool.h>

#include "appfw_m3u.h"

// 每次开机调用一次;此后 poll 由高频入口(radio_store_count 等)带动。
void appfw_biglist_init(void);

// 复检清单/索引是否就绪。就绪态每次调用只做一次 fopen+ftell 轻探;大小
// 连续两轮一致才校验/重建(上传中的文件每轮都在长,不动);文件不存在 →
// 落回内置台目,每 5s 一探是否新上传。返回模式是否可用(台目兜底时恒真)。
bool appfw_biglist_poll(void);

// 大清单模式是否可用(台目注册后恒真;应用据此决定只读与委托)。
bool appfw_biglist_available(void);

// 条数(文件模式=文件条数;台目模式=台目条数;都没有=0)。
int appfw_biglist_count(void);

// 读第 idx 条(name/url 各自的缓冲由调用方给,cap 用 APPFW_M3U_*_MAX)。
bool appfw_biglist_get(int idx, char *name, size_t name_cap,
                       char *url, size_t url_cap);

// 按名找下标:文件模式流式线性扫(兜底路径,主路径用调用方的下标缓存);
// 台目模式内存扫。名字按 APPFW_M3U_NAME_MAX 截断后比较。未找到 -1。
int appfw_biglist_find(const char *name);

// 恢复出厂:删除 m3u 与索引(之后落回台目)。应用在恢复出厂时调用。
void appfw_biglist_discard(void);

// 仅测试用:改写 m3u/idx 所在目录(默认 /files)。
void appfw_biglist_set_dir(const char *dir);

// ---- 应用注入(必须在首次 poll 前调用;都可不注) ----

// 内存钩子:FAT 挂载前堆最大块不足时调 make_room()(应用释放大块预留,
// 返回是否腾出了空间);空闲卸载后调 room_freed()(应用补回预留)。不注
// 册则挂载失败即放弃(10s 退避)。
void appfw_biglist_set_memory_hooks(bool (*make_room)(void), void (*room_freed)(void));

// 内置台目兜底:无文件模式时 count/get 走应用注册的只读数组(通常
// const rodata,零 RAM)。注册即启用;不注册则无文件=真正不可用。
typedef struct {
    char name[APPFW_M3U_NAME_MAX];
    char url[APPFW_M3U_URL_MAX];
} appfw_biglist_entry_t;
void appfw_biglist_set_catalog(int (*count)(void),
                               bool (*get)(int idx, appfw_biglist_entry_t *out));

// 占用查询:轮询在"占用中"(如播放器连接/播放/出错)时绝不发起 FAT 挂载
// (错误态内存最紧,挂载必失败还漏磨损均衡句柄);返回缓存的模式应答。
void appfw_biglist_set_busy_query(bool (*busy)(void));
