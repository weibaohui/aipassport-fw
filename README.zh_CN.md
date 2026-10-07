<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI Passport 应用框架

FoloToy AI Passport(ESP32-C3,无 PSRAM)应用的共享框架。凡是不属于产品行为的
能力都归它:AI 入口、配网门户、WiFi 引擎、流媒体管线、UI 骨架、板级驱动。
新应用只写自己的数据、页面和工具表——**不改框架一行代码**。

已有应用基于它运行:

- [aipassport-radio](https://github.com/weibaohui/aipassport-radio) —— 网络收音机
  (345 内置台 + NVS 自定义台,23 个 MCP 工具,常驻 AI 控制)
- GLM 用量宝(同硬件看板应用)

## 框架提供什么

| 能力 | 模块 | 说明 |
| --- | --- | --- |
| **MCP 常驻服务** | `appfw_mcp.c` + `appfw_mcp_srv.c` | AI 的完整设备入口。协议核与传输分离:应用注册工具表,框架管 JSON-RPC 分发;传输是自建极简 TCP 服务(单端点 `POST /mcp`,Connection: close,无会话无 SSE),**常驻在线**,空转约 6KB。没注册工具 = 一分钱不花 |
| **配置化门户** | `appfw_portal.c` | captive 配网(80 端口 + DNS 劫持)+ REST 配置端点,仅配网期存在,联网空闲自动卸载回收 ~10KB。支持"默认全关、显式开启"的内置菜单声明 |
| **WiFi 引擎** | `appfw_net.c` / `appfw_netlist.c` | 多热点按序回退、断线自动重连、扫描、SoftAP、连接窗口的内存避让 |
| **通用流管线** | `appfw_stream.c` + `appfw_hls/icy/frame.c` | URL→纯音频字节:HTTP(S) 连接、30x 重定向(含大写 scheme 坑)、HLS 直播段轮换、ICY 元数据就地剥离、https 被掐自动换 http 分身。解码与 I2S 留给应用 |
| **NVS 存储** | `appfw_storage.c` | 键值、u16、热点表、配置导入导出(内存版桩可测) |
| **UI 骨架** | `appfw_ui.c` | 状态机、光标行列表、设置菜单(逐项可配置、默认全关)、长按动作表、吐司、息屏 |
| **板级驱动** | `bsp/` | 显示/LVGL(8 行绘制缓冲)、电阻梯按键、I2C、ES8311 音频、电量计;`bsp_pins.h` 是引脚唯一事实来源 |

**已移除**:FAT 文件分区层(密码锁 + `/api/files/*` 端点)。应用持久化自决——
收音机选择了"rodata 台目 + NVS",框架不再假设应用需要文件系统。

## 应用提供什么

全部产品行为经三张配置结构体注入,**普通应用不需要改框架**:

| 结构体 | 头文件 | 用途 |
| --- | --- | --- |
| `appfw_ui_cfg_t` | `appfw_ui.h` | 主页构建/轮询/按键、信息行、菜单使能位掩码、长按动作表 |
| `appfw_client_cfg_t` | `appfw_client.h` | `make_request`(URL/认证/附加头)与 `on_result` |
| `appfw_prov_cfg_t` | `appfw_portal.h` | 门户片段、导入导出钩子、私有端点注册(`on_httpd_ready`) |

MCP 工具走注册而非结构体:

```c
// 应用工具表(static const,描述就是给 AI 的使用说明)
appfw_mcp_set_tools(TOOLS, count);
// 框架内置功能工具按菜单使能位自动挂载(位序 = appfw_menu_item_t)
appfw_mcp_set_builtin_tools(menu_show_mask);
// UI 初始化时自动拉起常驻服务(appfw_mcp_server_start,幂等)
```

### 设置项开关:`menu_show_mask`

框架自带六个设置菜单项,默认**一项都不显示**(`0`),应用按需用位或组合。
**没开的项连背后的运行行为也不会启动**(不开"配网" = 设备离线也不会自动
拉配网门户;不开"AI 管理"只是不显示信息页,常驻服务照常在):

| 位 | 菜单项 | 控制的运行行为 |
| --- | --- | --- |
| `1 << 0` | 刷新周期 | 轮询客户端周期设置 |
| `1 << 1` | 息屏时间 | 空闲自动息屏 |
| `1 << 2` | WiFi 管理 | 扫描/连接交互(多热点回退引擎本身常开) |
| `1 << 3` | 设备信息 | — |
| `1 << 4` | 配网 | 离线时自动开启配网门户 |
| `1 << 5` | AI 管理 | 信息页(不挂 MCP 工具,AI 本来就在) |

```c
// 例(收音机):要五项,刷新周期无意义不开
#define APP_MENU_SHOW_MASK (APPFW_MENU_ITEM_ALL & ~APPFW_MENU_ITEM_REFRESH_PERIOD)
ucfg.menu_show_mask = APP_MENU_SHOW_MASK;   // 同一个掩码同时喂 MCP 内置工具
```

### 按键自定义

两层机制,优先级:`home_key` 回调 > 长按动作表 > 框架默认约定。

**长按动作表**(主页三键各一个动作,默认全 `DO_NOTHING`):

```c
ucfg.long_press_up   = APPFW_LONG_PRESS_OPEN_MENU;         // 长按上=设置菜单
ucfg.long_press_down = APPFW_LONG_PRESS_OPEN_APP_OPTION_1; // 长按下=音量页
ucfg.long_press_ok   = APPFW_LONG_PRESS_DO_NOTHING;        // 应用自己接管
```

可选动作:`OPEN_MENU` / `OPEN_WIFI_MANAGER` / `OPEN_DEVICE_INFO` /
`OPEN_PROVISIONING` / `OPEN_AI_ADMIN` / `OPEN_APP_OPTION_1` / `_2`(直达
`menu_opts[n]` 选项页,返回回主页)。

**整体接管**(`full_key`,新应用推荐):需要列表选择/播放控制、按住类
动作这类多键交互的应用,用它接管完整按键生命周期。事件是
`appfw_key_event_t`:`PRESS`、`CLICK`、`DOUBLE`、`LONG`、`LONG_UP`;
`btn`:`0`=上、`1`=下、`2`=OK。`CLICK/DOUBLE/LONG` 只在主页派发;
`PRESS` 只在主页开始按住动作;`LONG_UP` 始终派发,即使长按已打开菜单,
应用也能收到“松开”。返回 `APPFW_KEY_CONSUMED` 吃掉事件,返回
`APPFW_KEY_DEFAULT` 交给长按表与默认约定,返回 `APPFW_KEY_MENU`
要求开设置菜单。

**旧接口**(`home_key`)仍兼容:事件规整为 `ev`:`0`=单击、`2`=双击、
`3`=长按,按下瞬间和松开被丢弃。配置 `full_key` 后 `home_key` 不再调用。

### 应用选项页:`menu_opts`

往设置菜单追加最多 2 个应用自定义选项(一屏行数所限)。OK 选中即:
存 NVS(`appfw` 命名空间的 `key`)+ 回调 + 吐司 + 回菜单,应用在
`on_change` 里让配置生效:

```c
static const uint16_t k_vols[] = {0, 20, 40, 60, 80, 100};
static const char *const k_lbls[] = {"0%","20%","40%","60%","80%","100%"};
static const appfw_menu_opt_t k_opts[] = {{
    .key = "opt_volume", .label = "音量",
    .symbol = LV_SYMBOL_VOLUME_MID,
    .opts = k_vols, .lbls = k_lbls, .count = 6,
    .on_change = apply_volume,       // 应用负责真正生效(如写 codec)
}};
ucfg.menu_opts = k_opts;
ucfg.menu_opts_count = 1;
```

门户 REST(`/api/config/export|import`)与应用配置卡片经
`app_config_fill` / `app_config_apply` 钩子读写应用自有字段。

## 新建应用

应用是独立仓库,把框架作为 submodule 挂进来——框架修一次,所有应用都拿到:

```bash
git clone https://github.com/<你>/aipassport-<app>.git
cd aipassport-<app>
git submodule add https://github.com/weibaohui/aipassport-fw.git components/framework
git submodule update --init --recursive
```

应用根 `CMakeLists.txt` 需显式加入组件搜索路径(ESP-IDF 不递归扫描):

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")
```

应用仓自己拥有:`main/`、`assets/`(字库子集)、自己的主机测试、根构建输入
(`CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv`),以及一个把门禁
转发给本仓的薄封装 `tools/validate.sh`。

## 验证

`tools/validate.sh` 是框架与应用两种布局的唯一门禁实现:

```bash
./tools/validate.sh --static    # 仓库检查 + 主机测试
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```

应用仓把自己的主机测试写进 `tests/host_tests.txt`,门禁自动跑;框架模块
(`appfw_mcp` 的分发语义、`appfw_netlist` 等)在本仓 `tests/` 有主机测试,
应用仓可直接引用框架源编译。

## 边界规则

`appfw/` 与 `bsp/` 不含任何业务词汇、字段名、品牌串或按应用分支的逻辑。
应用需要框架没有的能力时,先写在自己仓里;确定第二个应用也需要时,再往配置
结构体上加注入点(默认 `NULL`)。绝不加 `if (app == ...)` 分支。

## 许可

与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 相同。
