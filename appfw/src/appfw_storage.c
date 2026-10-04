// components/appfw/src/appfw_storage.c —— NVS 配置存储实现,见 appfw_storage.h。
#include "appfw_storage.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "appfw_store";
static const char *NS = "appfw";

int appfw_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需重建(%s),擦除后重试", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    return err;
}

bool appfw_store_get_str(const char *key, char *buf, size_t buf_len)
{
    if (!key || !buf || buf_len == 0) return false;
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t needed = buf_len;
    esp_err_t err = nvs_get_str(h, key, buf, &needed);
    nvs_close(h);
    if (err != ESP_OK) {
        buf[0] = '\0';
        return false;
    }
    return buf[0] != '\0';
}

bool appfw_store_set_str(const char *key, const char *value)
{
    if (!key) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool appfw_store_get_u16(const char *key, uint16_t *out, uint16_t fallback)
{
    if (!out) return false;
    *out = fallback;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint16_t v = 0;
    esp_err_t err = nvs_get_u16(h, key, &v);
    nvs_close(h);
    // 键存在即合法——值为 0 是真数据(音量 0%、熄屏"永不"都存 0;
    // 旧语义把 0 当未存,导致这两档永远读回 fallback,真机踩过)。
    if (err != ESP_OK) return false;
    *out = v;
    return true;
}

bool appfw_store_set_u16(const char *key, uint16_t value)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u16(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

static bool set_u16_checked(const char *key, uint16_t v, const uint16_t *allowed, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (allowed[i] == v) return appfw_store_set_u16(key, v);
    return false;
}

bool appfw_store_get_period(uint16_t *period_s)
{
    return appfw_store_get_u16("period_s", period_s, 60);
}

bool appfw_store_set_period(uint16_t period_s)
{
    static const uint16_t allowed[] = { 60, 300, 600, 900, 1800, 3600 };
    return set_u16_checked("period_s", period_s, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

bool appfw_store_get_screen_off(uint16_t *screen_off_s)
{
    return appfw_store_get_u16("screen_off_s", screen_off_s, 300);
}

bool appfw_store_set_screen_off(uint16_t screen_off_s)
{
    static const uint16_t allowed[] = { 0, 60, 300, 600, 900, 1800 };
    return set_u16_checked("screen_off_s", screen_off_s, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

bool appfw_store_get_brightness(uint16_t *pct)
{
    return appfw_store_get_u16("brightness", pct, 100);
}

bool appfw_store_set_brightness(uint16_t pct)
{
    static const uint16_t allowed[] = { 10, 30, 50, 70, 100 };
    return set_u16_checked("brightness", pct, allowed, sizeof(allowed) / sizeof(allowed[0]));
}

bool appfw_store_netlist_load(appfw_netlist_t *list)
{
    if (!list) return false;
    appfw_netlist_reset(list);
    char *blob = malloc(APPFW_NETLIST_BLOB_MAX);
    if (!blob) {
        ESP_LOGE(TAG, "无内存读热点列表");
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        free(blob);
        return false;
    }
    size_t needed = APPFW_NETLIST_BLOB_MAX;
    esp_err_t err = nvs_get_str(h, "nets", blob, &needed);
    nvs_close(h);
    if (err != ESP_OK) {
        free(blob);
        return false;
    }
    bool ok = appfw_netlist_deserialize(blob, list);
    free(blob);
    if (!ok) {
        ESP_LOGW(TAG, "已存热点列表损坏,已丢弃");
        appfw_netlist_reset(list);
        return false;
    }
    char sel[APPFW_NETLIST_SSID_MAX] = { 0 };
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t needed2 = sizeof(sel);
        if (nvs_get_str(h, "sel_ssid", sel, &needed2) == ESP_OK && sel[0] != '\0') {
            (void)appfw_netlist_select(list, sel);
        }
        nvs_close(h);
    }
    return true;
}

bool appfw_store_netlist_save(const appfw_netlist_t *list)
{
    if (!list) return false;
    char *blob = malloc(APPFW_NETLIST_BLOB_MAX);
    if (!blob) return false;
    if (!appfw_netlist_serialize(list, blob, APPFW_NETLIST_BLOB_MAX)) {
        ESP_LOGE(TAG, "热点列表序列化失败(不应发生)");
        free(blob);
        return false;
    }
    nvs_handle_t h;
    bool ok = true;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        free(blob);
        return false;
    }
    ok = nvs_set_str(h, "nets", blob) == ESP_OK;
    const char *sel = (list->selected >= 0 && list->selected < (int)list->count)
                          ? list->items[list->selected].ssid : "";
    if (ok) ok = nvs_set_str(h, "sel_ssid", sel) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    free(blob);
    return ok;
}

bool appfw_store_clear_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
