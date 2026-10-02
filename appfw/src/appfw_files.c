// components/appfw/src/appfw_files.c —— 文件管理实现,见 appfw_files.h。
#include "appfw_files.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "wear_levelling.h"

static const char *TAG = "appfw_files";
static const char *MOUNT = "/files";
static const char *PWD_KEY = "files_pwd"; // 框架 NVS:SHA-256 hex(64 字符)

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_unlocked;
static bool s_mounted;

// ---- 名称校验(纯逻辑,主机可测) ----
bool appfw_files_valid_name(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n > 64) return false;
    if (name[0] == '.') return false; // 防隐藏/路径穿越
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    // 防 ".." 穿越:扁平命名空间 + 无 '/' 已足够,双重点名禁止更稳。
    if (strcmp(name, "..") == 0 || strcmp(name, ".") == 0) return false;
    return true;
}

// ---- 挂载 ----
int appfw_files_init(void)
{
    if (s_mounted) return 0;
    // format_if_mount_failed:首次启动(空分区)自动格式化。
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, "files", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "files 分区挂载失败:%s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "files 分区已挂载(%s)", MOUNT);
    return 0;
}

// ---- 密码(SHA-256 → NVS hex) ----
static void sha256_hex(const char *in, char *hex_out /*65 字节*/)
{
    unsigned char digest[32];
    mbedtls_sha256((const unsigned char *)in, strlen(in), digest, 0);
    static const char *HEX = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2] = HEX[digest[i] >> 4];
        hex_out[i * 2 + 1] = HEX[digest[i] & 0xF];
    }
    hex_out[64] = '\0';
}

static bool pwd_hash_matches(const char *pw)
{
    char stored[65] = { 0 }, calc[65];
    nvs_handle_t h;
    if (nvs_open("appfw", NVS_READONLY, &h) != ESP_OK) return false;
    size_t need = sizeof(stored);
    esp_err_t err = nvs_get_str(h, PWD_KEY, stored, &need);
    nvs_close(h);
    if (err != ESP_OK) return false;
    sha256_hex(pw, calc);
    return strcmp(stored, calc) == 0;
}

bool appfw_files_has_password(void)
{
    char tmp[8];
    nvs_handle_t h;
    if (nvs_open("appfw", NVS_READONLY, &h) != ESP_OK) return false;
    size_t need = sizeof(tmp);
    esp_err_t err = nvs_get_str(h, PWD_KEY, tmp, &need);
    nvs_close(h);
    return err == ESP_OK;
}

bool appfw_files_set_password(const char *old_pw, const char *new_pw)
{
    if (!new_pw || strlen(new_pw) < 4 || strlen(new_pw) > 32) return false;
    bool has = appfw_files_has_password();
    if (has) {
        // 已设密码:旧密码必须正确(开锁态或持旧密码均可改,双通道之一)。
        if (!old_pw || !pwd_hash_matches(old_pw)) return false;
    }
    char hex[65];
    sha256_hex(new_pw, hex);
    nvs_handle_t h;
    if (nvs_open("appfw", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, PWD_KEY, hex);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool appfw_files_unlock(const char *password)
{
    if (!password) return false;
    if (!pwd_hash_matches(password)) return false;
    s_unlocked = true;
    return true;
}

void appfw_files_lock(void) { s_unlocked = false; }
bool appfw_files_is_unlocked(void) { return s_unlocked; }

// ---- 文件操作 ----

static void full_path(const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%s", MOUNT, name);
}

int appfw_files_list(char (*names)[64], int max)
{
    if (!s_mounted || !s_unlocked) return -1;
    DIR *d = opendir(MOUNT);
    if (!d) return -1;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        if (appfw_files_valid_name(ent->d_name)) {
            strncpy(names[n], ent->d_name, 63);
            names[n][63] = '\0';
            n++;
        }
    }
    closedir(d);
    return n;
}

bool appfw_files_write(const char *name, const char *data, size_t len)
{
    if (!s_unlocked || !appfw_files_valid_name(name)) return false;
    if (len > 512 * 1024) return false;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "写入失败:%s", path);
        return false;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

bool appfw_files_read(const char *name, char *buf, size_t buf_len, size_t *out_len)
{
    if (!s_unlocked || !appfw_files_valid_name(name)) return false;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t r = fread(buf, 1, buf_len - 1, f);
    fclose(f);
    buf[r] = '\0';
    if (out_len) *out_len = r;
    return true;
}

bool appfw_files_delete(const char *name)
{
    if (!s_unlocked || !appfw_files_valid_name(name)) return false;
    char path[96];
    full_path(name, path, sizeof(path));
    return unlink(path) == 0;
}
