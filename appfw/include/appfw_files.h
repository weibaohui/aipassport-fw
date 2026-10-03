// components/appfw/include/appfw_files.h —— 文件管理(FATFS 分区 + 密码锁)。
//
// 独立 files 分区(FATFS+磨损均衡):分段刷机(0x0 bootloader + 0x8000 分区表 +
// 0x10000 应用)不会触碰该分区,文件在刷机后保留;整体 0x0 合并镜像刷写会清除。
//
// 安全模型:上传/下载/删除/列表都需要先"开锁"——提供正确密码。密码经
// SHA-256 后存于框架 NVS(files_pwd);首次未设密码时必须先设置密码
// (首次设置不需要旧密码,之后修改需要)。重启后自动回到锁定态。
//
// 文件名:扁平命名空间,仅 [A-Za-z0-9._-],长度 1~64,不以 '.' 开头(防路径穿越)。
// 单文件 ≤ 512KB(分区 1MB,预留 FAT 与磨损均衡空间)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 挂载 files 分区(失败时自动格式化;首次启动即格式化空分区)。
// 返回 0 成功,否则 ESP_ERR 码。
int appfw_files_init(void);

// 确保已挂载(幂等,懒挂载)。应用绕过本模块的读写接口、用自己的 stdio
// 直接访问 /files 下文件前调用;已挂载时零开销。false = 挂载失败。
bool appfw_files_ensure_mounted(void);

// ---- 锁与密码 ----
bool appfw_files_has_password(void);              // 是否已设置密码
bool appfw_files_set_password(const char *old_pw, const char *new_pw); // 首次设置 old 传 NULL
bool appfw_files_unlock(const char *password);    // 密码正确→开锁
void appfw_files_lock(void);                      // 上锁
bool appfw_files_is_unlocked(void);               // 当前是否开锁

// ---- 文件操作(全部要求已开锁;未开锁返回 false 且 errno 不变,调用方自行提示) ----
bool appfw_files_valid_name(const char *name);    // 文件名合法性(纯逻辑,主机可测)
int appfw_files_list(char (*names)[64], int max); // 返回文件数(≤max)
bool appfw_files_write(const char *name, const char *data, size_t len); // 覆盖写;≤512KB
bool appfw_files_read(const char *name, char *buf, size_t buf_len, size_t *out_len);
bool appfw_files_delete(const char *name);

// ---- 受信操作(设备本地按键触发,绕过密码锁;HTTP 端点仍走带锁版本) ----
// 操作完成后建议调用 appfw_files_unmount() 释放 FATFS/磨损均衡占用的 RAM
// (约 8KB,否则后续 TLS 握手可能内存不足)。
int appfw_files_list_trusted(char (*names)[64], int max);
bool appfw_files_write_trusted(const char *name, const char *data, size_t len);
bool appfw_files_read_trusted(const char *name, char *buf, size_t buf_len, size_t *out_len);
bool appfw_files_delete_trusted(const char *name);
int appfw_files_unmount(void); // 卸载并释放内存;下次文件操作自动重新挂载
