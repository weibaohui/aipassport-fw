<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# AI Passport 应用框架

FoloToy AI Passport(ESP32-C3)应用的共享框架。凡是不属于产品行为的能力都归它,
新应用只写自己的页面、解析和门户卡片。

## 仓库结构

```text
appfw/            可复用应用框架(与应用无关)
bsp/              板级驱动:显示/LVGL、按键、I2C、音频、电池
bsp/include/bsp_pins.h
                  引脚与硬件参数的唯一事实来源
tools/            validate.sh、check_repo.py、固件归档与校验
skills/           passport-develop、passport-setup、passport-build、
                  passport-device-test、passport-debug
tests/            框架主机测试及其桩
docs/             工程、硬件与参考文档
```

## 框架提供什么

| 能力 | 模块 |
| --- | --- |
| WiFi 引擎、多热点回退、扫描、配网 SoftAP | `appfw/src/appfw_net.c` |
| 常驻 HTTP 门户 + captive DNS、两阶段网页、14 个 REST 端点 | `appfw/src/appfw_portal.c` |
| NVS 键值、框架设置、配置导入导出 | `appfw/src/appfw_storage.c` |
| 轮询客户端:等联网 → SNTP → HTTPS → 周期 → 错误分类 | `appfw/src/appfw_client.c` |
| FATFS 分区、密码锁、上传下载 | `appfw/src/appfw_files.c` |
| UI 状态机、设置菜单、状态栏、熄屏 | `appfw/src/appfw_ui.c` |

## 应用提供什么

全部产品相关行为,通过三张配置结构体注入。普通应用**不需要改框架**:

| 结构体 | 文件 | 用途 |
| --- | --- | --- |
| `appfw_ui_cfg_t` | `appfw/include/appfw_ui.h` | 主页构建/轮询/按键、信息行、门户 HTML、配置 JSON 钩子 |
| `appfw_client_cfg_t` | `appfw/include/appfw_client.h` | `make_request`(URL/认证/附加头)与 `on_result` |
| `appfw_prov_cfg_t` | `appfw/include/appfw_portal.h` | 门户片段、导入导出钩子、私有端点注册 |

## 新建应用

应用是独立仓库,不 fork 本仓,而是把框架作为 submodule 挂进来 —— 这样修一次,
所有应用都拿到。

```bash
git clone https://github.com/<你>/aipassport-<app>.git
cd aipassport-<app>
git submodule add https://github.com/<你>/aipassport-fw.git components/framework
git submodule update --init --recursive
```

`components/framework` 是**一个**挂载点,里面同时装 `appfw` 和 `bsp`。ESP-IDF 只
扫描 `components/` 的直接子目录、不递归,所以应用根 `CMakeLists.txt` 必须显式把
框架目录加进搜索路径:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")
```

应用仓自己拥有:`main/`、`assets/`(字库子集与字符清单)、自己的主机测试、根构建
输入(`CMakeLists.txt`、`sdkconfig.defaults`、`partitions.csv`、`dependencies.lock`),
以及一个把门禁转发给本仓的薄封装 `tools/validate.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${app_root}/components/framework/tools/validate.sh" --project-root "${app_root}" "$@"
```

## 验证

`tools/validate.sh` 是两种布局的唯一实现:在本仓跑框架主机测试,在应用仓额外跑
应用主机测试与固件构建。

```bash
./tools/validate.sh --static    # 仓库检查 + 主机测试
./tools/validate.sh --firmware  # ESP-IDF 构建 + 合并镜像校验
./tools/validate.sh             # 完整门禁
```

## 边界规则

`appfw/` 与 `bsp/` 不含任何业务词汇、字段名、品牌串或按应用分支的逻辑,门户
HTML 模板也不例外。应用需要框架没有的能力时,先写在自己的仓库里;确定第二个
应用也需要时,再往对应配置结构体上加注入点(默认 `NULL`)。绝不加
`if (app == ...)` 分支。详见 `docs/development/ai-guide.md` 的
[应用与 appfw 的边界](docs/development/ai-guide.zh_CN.md#应用与-appfw-的边界)与
[跨应用复用 appfw](docs/development/ai-guide.zh_CN.md#跨应用复用-appfw)。

## 许可

与上游 [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) 相同。
