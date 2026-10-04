<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport Application Framework

Shared framework for FoloToy AI Passport (ESP32-C3, no PSRAM) applications. It
owns everything that is not product behavior: the AI entry point, provisioning
portal, WiFi engine, streaming pipeline, UI skeleton, and board drivers. A new
application only writes its own data, screens, and tool table — **without
touching the framework**.

Applications running on it:

- [aipassport-radio](https://github.com/weibaohui/aipassport-radio) — internet
  radio (345 built-in stations + NVS custom stations, 23 MCP tools,
  always-on AI control)
- GLM Usage Meter (a dashboard app on the same hardware)

## What the framework provides

| Capability | Module | Notes |
| --- | --- | --- |
| **Resident MCP server** | `appfw_mcp.c` + `appfw_mcp_srv.c` | The full AI entry point. Protocol core and transport are split: the application registers a tool table, the framework dispatches JSON-RPC. Transport is a hand-rolled minimal TCP server (single endpoint `POST /mcp`, Connection: close, no sessions, no SSE) that is **always on** at ~6KB idle cost. With no tools registered it costs nothing |
| **Configurable portal** | `appfw_portal.c` | Captive provisioning (port 80 + DNS hijack) + REST config endpoints. Exists only during provisioning; unloads itself when idle online, returning ~10KB. Built-in menu items are "off by default, opt-in" |
| **WiFi engine** | `appfw_net.c` / `appfw_netlist.c` | Multi-AP fallback in order, auto reconnect, scan, SoftAP, heap avoidance during connection windows |
| **Generic stream pipeline** | `appfw_stream.c` + `appfw_hls/icy/frame.c` | URL → pure audio bytes: HTTP(S), 30x redirects (including the uppercase-scheme trap), HLS live segment rotation, in-place ICY metadata stripping, automatic http-mirror retry when https is cut. Decoding and I2S stay in the application |
| **NVS storage** | `appfw_storage.c` | Key-value, u16, hotspot list, config import/export (in-memory stub for host tests) |
| **UI skeleton** | `appfw_ui.c` | State machine, cursor-row lists, settings menu (per-item opt-in, off by default), long-press action table, toasts, screen sleep |
| **Board drivers** | `bsp/` | Display/LVGL (8-line draw buffer), resistor-ladder buttons, I2C, ES8311 audio, fuel gauge; `bsp_pins.h` is the single source of truth for pins |

**Removed**: the FAT file-partition layer (password lock + `/api/files/*`
endpoints). Persistence is now the application's call — the radio chose
"rodata catalog + NVS", and the framework no longer assumes applications need
a filesystem.

## What the application provides

All product behavior is injected through three config structs — **a normal
application never edits the framework**:

| Struct | Header | Purpose |
| --- | --- | --- |
| `appfw_ui_cfg_t` | `appfw_ui.h` | Home build/poll/keys, info rows, menu enable bitmask, long-press action table |
| `appfw_client_cfg_t` | `appfw_client.h` | `make_request` (URL/auth/extra headers) and `on_result` |
| `appfw_prov_cfg_t` | `appfw_portal.h` | Portal fragment, import/export hooks, private endpoint registration (`on_httpd_ready`) |

MCP tools are registered rather than injected:

```c
// Application tool table (static const; descriptions ARE the AI's manual)
appfw_mcp_set_tools(TOOLS, count);
// Framework built-in tools mounted by menu enable bits (= appfw_menu_item_t)
appfw_mcp_set_builtin_tools(menu_show_mask);
// The resident server starts automatically in appfw_ui_init (idempotent)
```

### Settings toggles: `menu_show_mask`

The framework ships six settings-menu items, **all off by default** (`0`);
the application opts in per bit. **A disabled item also disables the runtime
behavior behind it** (without "Provisioning", the portal never auto-starts
when offline; without "AI Admin" the info page is hidden but the resident
server still runs):

| Bit | Menu item | Runtime behavior it controls |
| --- | --- | --- |
| `1 << 0` | Refresh period | Polling client period |
| `1 << 1` | Screen-off time | Idle auto screen sleep |
| `1 << 2` | WiFi manager | Scan/connect interaction (the fallback engine itself is always on) |
| `1 << 3` | Device info | — |
| `1 << 4` | Provisioning | Auto-start the portal when offline |
| `1 << 5` | AI admin | Info page only (mounts no MCP tools; AI is already in) |

```c
// Example (radio): five items; refresh period is meaningless, left off
#define APP_MENU_SHOW_MASK (APPFW_MENU_ITEM_ALL & ~APPFW_MENU_ITEM_REFRESH_PERIOD)
ucfg.menu_show_mask = APP_MENU_SHOW_MASK;   // same mask feeds MCP builtin tools
```

### Key customization

Two layers, priority: `home_key` callback > long-press table > framework
defaults.

**Long-press action table** (one action per home key, all `DO_NOTHING` by
default):

```c
ucfg.long_press_up   = APPFW_LONG_PRESS_OPEN_MENU;         // up = settings menu
ucfg.long_press_down = APPFW_LONG_PRESS_OPEN_APP_OPTION_1; // down = volume page
ucfg.long_press_ok   = APPFW_LONG_PRESS_DO_NOTHING;        // app takes it over
```

Available actions: `OPEN_MENU` / `OPEN_WIFI_MANAGER` / `OPEN_DEVICE_INFO` /
`OPEN_PROVISIONING` / `OPEN_AI_ADMIN` / `OPEN_APP_OPTION_1` / `_2` (jumps
straight to `menu_opts[n]`, back returns home).

**Full takeover** (`home_key`): applications needing multi-key interaction
(list selection, playback control) take over all home keys. Events are
normalized to `ev`: `0`=click, `2`=double-click, `3`=long-press (the initial
press is discarded by the framework); `btn`: `0`=up, `1`=down, `2`=OK.
Return `APPFW_KEY_CONSUMED` to swallow, `APPFW_KEY_DEFAULT` to fall through
to the long-press table and defaults, `APPFW_KEY_MENU` to open the settings
menu.

### Application option pages: `menu_opts`

Append up to 2 application-defined option pages to the settings menu (one
screen of rows is the limit). Selecting with OK: persists to NVS (`key` in
the `appfw` namespace) + callback + toast + back; the application makes the
setting take effect in `on_change`:

```c
static const uint16_t k_vols[] = {0, 20, 40, 60, 80, 100};
static const char *const k_lbls[] = {"0%","20%","40%","60%","80%","100%"};
static const appfw_menu_opt_t k_opts[] = {{
    .key = "opt_volume", .label = "Volume",
    .symbol = LV_SYMBOL_VOLUME_MID,
    .opts = k_vols, .lbls = k_lbls, .count = 6,
    .on_change = apply_volume,       // the app actually applies it (codec…)
}};
ucfg.menu_opts = k_opts;
ucfg.menu_opts_count = 1;
```

The portal REST endpoints (`/api/config/export|import`) and the app config
card read/write application fields through the `app_config_fill` /
`app_config_apply` hooks.

## Creating an application

Applications live in their own repositories and mount the framework as a
submodule — fix once, every application benefits:

```bash
git clone https://github.com/<you>/aipassport-<app>.git
cd aipassport-<app>
git submodule add https://github.com/weibaohui/aipassport-fw.git components/framework
git submodule update --init --recursive
```

The application's root `CMakeLists.txt` must add the component search path
explicitly (ESP-IDF does not recurse):

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")
```

The application repository owns: `main/`, `assets/` (font subsets), its host
tests, root build inputs (`CMakeLists.txt`, `sdkconfig.defaults`,
`partitions.csv`), and a thin `tools/validate.sh` that forwards the gate to
this repository.

## Validation

`tools/validate.sh` is the single implementation of the gate for both layouts:

```bash
./tools/validate.sh --static    # repo checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged image verification
./tools/validate.sh             # full gate
```

Applications register their host tests in `tests/host_tests.txt` and the gate
picks them up; framework modules (`appfw_mcp` dispatch semantics,
`appfw_netlist`, …) have host tests here that applications may compile
directly against framework sources.

## Boundary rules

`appfw/` and `bsp/` contain no business vocabulary, field names, brand
strings, or per-application branching — portal HTML templates included. When
an application needs something the framework lacks, write it in the
application repo first; move it into a config-struct injection point (default
`NULL`) once a second application needs it. Never add `if (app == ...)`
branches.

## License

Same as upstream
[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport).
