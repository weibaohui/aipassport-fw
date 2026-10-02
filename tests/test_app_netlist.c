// tests/test_appfw_netlist.c —— 已保存热点列表纯逻辑的主机测试。
// 编译(见 tools/validate.sh):
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_appfw_netlist.c main/appfw_netlist.c
#include "appfw_netlist.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static void test_add_and_limit(void)
{
    appfw_netlist_t l;
    appfw_netlist_reset(&l);
    CHECK(l.count == 0 && l.selected == -1);

    CHECK(appfw_netlist_add(&l, "home", "pw1"));
    CHECK(appfw_netlist_add(&l, "office", "pw2"));
    CHECK(l.count == 2);

    // 同名 = 覆盖密码,不新增。
    CHECK(appfw_netlist_add(&l, "home", "pw1b"));
    CHECK(l.count == 2);
    CHECK(strcmp(l.items[0].pwd, "pw1b") == 0);

    // 塞满后再加应失败(循环用不同 SSID,同名会走覆盖路径)。
    char name[16];
    for (int i = 0; l.count < APPFW_NETLIST_MAX; i++) {
        snprintf(name, sizeof(name), "net-%d", i);
        CHECK(appfw_netlist_add(&l, name, ""));
    }
    CHECK(l.count == APPFW_NETLIST_MAX);
    CHECK(!appfw_netlist_add(&l, "overflow", ""));

    // 空SSID / 超长 SSID 拒绝。
    CHECK(!appfw_netlist_add(&l, "", ""));
    char long_ssid[64];
    memset(long_ssid, 'a', sizeof(long_ssid) - 1);
    long_ssid[sizeof(long_ssid) - 1] = '\0';
    CHECK(!appfw_netlist_add(&l, long_ssid, ""));
}

static void test_remove_and_select(void)
{
    appfw_netlist_t l;
    appfw_netlist_reset(&l);
    appfw_netlist_add(&l, "a", "1");
    appfw_netlist_add(&l, "b", "2");
    appfw_netlist_add(&l, "c", "3");
    CHECK(appfw_netlist_select(&l, "b"));
    CHECK(l.selected == 1);

    // 删除被点选项 → selected 复位 -1;顺序保持。
    CHECK(appfw_netlist_remove(&l, 1));
    CHECK(l.count == 2);
    CHECK(strcmp(l.items[0].ssid, "a") == 0);
    CHECK(strcmp(l.items[1].ssid, "c") == 0);
    CHECK(l.selected == -1);

    // 删除未点选项 → selected 追随 SSID 前移。
    CHECK(appfw_netlist_select(&l, "c"));
    CHECK(appfw_netlist_remove(&l, 0)); // 删 a
    CHECK(l.count == 1);
    CHECK(l.selected == 0);
    CHECK(!appfw_netlist_remove(&l, 5)); // 越界
}

static void test_next_target_order(void)
{
    appfw_netlist_t l;
    appfw_netlist_reset(&l);
    appfw_netlist_entry_t t;
    CHECK(!appfw_netlist_next_target(&l, 0, &t)); // 空表

    appfw_netlist_add(&l, "a", "1");
    appfw_netlist_add(&l, "b", "2");
    appfw_netlist_add(&l, "c", "3");

    // 未点选:从保存顺序开始。
    CHECK(appfw_netlist_next_target(&l, 0, &t) && strcmp(t.ssid, "a") == 0);
    CHECK(appfw_netlist_next_target(&l, 1, &t) && strcmp(t.ssid, "b") == 0);
    CHECK(appfw_netlist_next_target(&l, 2, &t) && strcmp(t.ssid, "c") == 0);
    CHECK(appfw_netlist_next_target(&l, 3, &t) && strcmp(t.ssid, "a") == 0); // 轮转

    // 点选 c:第 0 次是 c,其后按保存顺序 a、b。
    appfw_netlist_select(&l, "c");
    CHECK(appfw_netlist_next_target(&l, 0, &t) && strcmp(t.ssid, "c") == 0);
    CHECK(appfw_netlist_next_target(&l, 1, &t) && strcmp(t.ssid, "a") == 0);
    CHECK(appfw_netlist_next_target(&l, 2, &t) && strcmp(t.ssid, "b") == 0);
}

static void test_serialize_roundtrip(void)
{
    appfw_netlist_t l, back;
    appfw_netlist_reset(&l);
    appfw_netlist_add(&l, "home-WiFi", "p@ss:word|123"); // 密码含分隔符也要无损
    appfw_netlist_add(&l, "中文热点", "");
    appfw_netlist_add(&l, "office", "3");

    char blob[APPFW_NETLIST_BLOB_MAX];
    CHECK(appfw_netlist_serialize(&l, blob, sizeof(blob)));
    CHECK(appfw_netlist_deserialize(blob, &back));
    CHECK(back.count == 3);
    CHECK(strcmp(back.items[0].ssid, "home-WiFi") == 0);
    CHECK(strcmp(back.items[0].pwd, "p@ss:word|123") == 0);
    CHECK(strcmp(back.items[1].ssid, "中文热点") == 0);
    CHECK(back.items[1].pwd[0] == '\0');

    // 空表序列化为空串,反序列化回空表。
    appfw_netlist_reset(&l);
    CHECK(appfw_netlist_serialize(&l, blob, sizeof(blob)));
    CHECK(blob[0] == '\0');
    CHECK(appfw_netlist_deserialize(blob, &back));
    CHECK(back.count == 0);

    // 缓冲不足必须失败,不得越界。
    char tiny[8];
    appfw_netlist_add(&l, "this-is-a-long-ssid-name", "password");
    CHECK(!appfw_netlist_serialize(&l, tiny, sizeof(tiny)));

    // 畸形 blob 拒绝。
    CHECK(!appfw_netlist_deserialize("garbage", &back));
    CHECK(!appfw_netlist_deserialize("3:abc:1:", &back));  // 长度声明与实际不符
}

int main(void)
{
    test_add_and_limit();
    test_remove_and_select();
    test_next_target_order();
    test_serialize_roundtrip();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_appfw_netlist: all checks passed\n");
    return 0;
}
