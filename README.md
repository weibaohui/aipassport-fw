<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport Application Framework

Shared framework for FoloToy AI Passport (ESP32-C3) applications. It owns
everything that is not product behavior, so a new application only writes its
own screens, parsers, and portal card.

## Repository layout

```text
appfw/            reusable application framework (application-agnostic)
bsp/              board drivers: display/LVGL, buttons, I2C, audio, battery
bsp/include/bsp_pins.h
                  single source of truth for pins and hardware parameters
tools/            validate.sh, check_repo.py, firmware archive/verify
skills/           passport-develop, passport-setup, passport-build,
                  passport-device-test, passport-debug
tests/            framework host tests and their stubs
docs/             engineering, hardware, and reference documentation
```

## What the framework provides

| Capability | Module |
| --- | --- |
| WiFi engine, multi-AP fallback, scan, provisioning SoftAP | `appfw/src/appfw_net.c` |
| Resident HTTP portal + captive DNS, two-stage pages, 14 REST endpoints | `appfw/src/appfw_portal.c` |
| NVS keys, framework settings, config import/export | `appfw/src/appfw_storage.c` |
| Polling client: wait-net → SNTP → HTTPS → period → error class | `appfw/src/appfw_client.c` |
| FATFS partition, password lock, upload/download | `appfw/src/appfw_files.c` |
| UI state machine, settings menu, status bar, screen sleep | `appfw/src/appfw_ui.c` |

## What an application provides

Everything product-specific, through three configuration structs. No framework
change is needed for a normal application.

| Struct | File | Purpose |
| --- | --- | --- |
| `appfw_ui_cfg_t` | `appfw/include/appfw_ui.h` | home page build/poll/keys, info rows, portal HTML, config JSON hooks |
| `appfw_client_cfg_t` | `appfw/include/appfw_client.h` | `make_request` (URL/auth/extra headers) and `on_result` |
| `appfw_prov_cfg_t` | `appfw/include/appfw_portal.h` | portal fragment, import/export hooks, private endpoint registration |

## Creating an application

An application is its own repository. It does not fork this one; it mounts the
framework as a submodule, so a fix here lands once for everyone.

```bash
git clone https://github.com/<you>/aipassport-<app>.git
cd aipassport-<app>
git submodule add https://github.com/<you>/aipassport-fw.git components/framework
git submodule update --init --recursive
```

`components/framework` is a single mount holding both `appfw` and `bsp`. ESP-IDF
scans only the direct children of `components/` and does not recurse, so the
application root `CMakeLists.txt` must opt the framework directory in:

```cmake
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")
```

Then the application repository owns: `main/`, `assets/` (font subsets and the
charset inventory), its own host tests, the root build inputs
(`CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv`, `dependencies.lock`),
and a thin `tools/validate.sh` that forwards to this repository's gate:

```bash
#!/usr/bin/env bash
set -euo pipefail
app_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${app_root}/components/framework/tools/validate.sh" --project-root "${app_root}" "$@"
```

## Validation

`tools/validate.sh` is the single implementation for both layouts. In this
repository it runs the framework host tests; in an application repository it
additionally runs the application host tests and the firmware build.

```bash
./tools/validate.sh --static    # repository checks + host tests
./tools/validate.sh --firmware  # ESP-IDF build + merged image verification
./tools/validate.sh             # complete gate
```

## Boundary rule

`appfw/` and `bsp/` contain no product vocabulary, field names, brand strings,
or per-application branches — the portal HTML template included. When an
application needs a capability the framework lacks, write it in the application
first. When a second application needs it too, add an injection point to the
relevant config struct with a `NULL` default. Never add
`if (app == ...)` branches. See the
[application/appfw boundary](docs/development/ai-guide.md#application-appfw-boundary)
and [reuse across applications](docs/development/ai-guide.md#reusing-appfw-across-applications)
rules in `docs/development/ai-guide.md`.

## License

Same license as the upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport).
