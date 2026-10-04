#!/usr/bin/env bash
# 统一门禁。本文件是唯一实现,同时服务两种仓库布局:
#
#   框架仓(aipassport-fw)  —— appfw/ bsp/ tools/ skills/ docs/ tests/
#   应用仓(aipassport-<app>)—— main/ assets/ tests/ + components/framework/(submodule)
#
# 在应用仓里由同目录的薄封装 tools/validate.sh 传入 --project-root 复用本实现。
# 应用仓通过 EXTRA_COMPONENT_DIRS 发现 submodule 里的 appfw 与 bsp,ESP-IDF 只扫描
# components/ 的直接子目录,不做递归,因此 submodule 挂一个根而不是两个。
set -euo pipefail

mode="--all"
project_root=""

# 参数与顺序无关:--all/--static/--firmware 选模式,--project-root 指定应用仓根。
while [[ $# -gt 0 ]]; do
    case "$1" in
        --all|--static|--firmware)
            mode="$1"; shift
            ;;
        --project-root)
            project_root="${2:?--project-root 需要一个目录参数}"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--all|--static|--firmware] [--project-root DIR]" >&2
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            echo "Usage: $0 [--all|--static|--firmware] [--project-root DIR]" >&2
            exit 2
            ;;
    esac
done

fw_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${project_root}" ]]; then
    project_root="$(cd -- "${project_root}" && pwd)"
else
    project_root="${fw_root}"
fi

# 框架目录:框架仓自身即根;应用仓里框架挂在 components/framework。
if [[ "${fw_root}" == "${project_root}" ]]; then
    fw="${project_root}"
else
    fw="${project_root}/components/framework"
fi
if [[ ! -d "${fw}/appfw" || ! -d "${fw}/bsp" ]]; then
    echo "ERROR: 未找到框架组件(appfw/bsp),期望在 ${fw}" >&2
    exit 1
fi

# 应用仓判定:有根 CMakeLists.txt 且有 main/。
is_app=0
if [[ -f "${project_root}/CMakeLists.txt" && -d "${project_root}/main" ]]; then
    is_app=1
fi

host_test() {
    # 编译并立即运行一个主机测试。用法:host_test <可执行名> <额外 include...> -- <源文件...>
    local name="$1"; shift
    local includes=() sources=()
    local seen_sep=0
    for arg in "$@"; do
        if [[ "${arg}" == "--" ]]; then seen_sep=1; continue; fi
        if [[ "${seen_sep}" -eq 0 ]]; then includes+=("${arg}"); else sources+=("${arg}"); fi
    done
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${includes[@]}" \
        "${sources[@]}" -o "${test_dir}/${name}"
    "${test_dir}/${name}"
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    # shellcheck disable=SC2064
    trap "rm -rf '${test_dir}'" RETURN

    # ---- 仓库结构检查:框架仓与应用仓各查自己 ----
    ( cd "${project_root}" && \
        CHECK_REPO_ROOT="${project_root}" \
        CHECK_REPO_LAYOUT="$([[ ${is_app} -eq 1 ]] && echo app || echo framework)" \
        python3 "${fw}/tools/check_repo.py" )

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$("${fw}/tools/install-actionlint.sh")"
    fi
    if [[ ${is_app} -eq 0 ]]; then
        "${actionlint_bin}" -color "${fw}"/.github/workflows/*.yml
    fi

    # ---- 框架主机测试(始终跑,来自框架仓) ----
    host_test test_app_netlist \
        "-I${fw}/appfw/include" -I"${fw}/tests" -- \
        "${fw}/tests/test_app_netlist.c" "${fw}/appfw/src/appfw_netlist.c"
    host_test test_appfw_m3u \
        "-I${fw}/appfw/include" -I"${fw}/tests" -- \
        "${fw}/tests/test_appfw_m3u.c" "${fw}/appfw/src/appfw_m3u.c"
    host_test test_appfw_icy \
        "-I${fw}/appfw/include" -I"${fw}/tests" -- \
        "${fw}/tests/test_appfw_icy.c" "${fw}/appfw/src/appfw_icy.c"
    host_test test_appfw_frame \
        "-I${fw}/appfw/include" -I"${fw}/tests" -- \
        "${fw}/tests/test_appfw_frame.c" "${fw}/appfw/src/appfw_frame.c"
    host_test test_appfw_hls \
        "-I${fw}/appfw/include" -I"${fw}/tests" -- \
        "${fw}/tests/test_appfw_hls.c" "${fw}/appfw/src/appfw_hls.c"
    host_test test_bsp_display_rounding \
        "-I${fw}/bsp/src" -- \
        "${fw}/tests/test_bsp_display_rounding.c" "${fw}/bsp/src/bsp_display_rounding.c"
    host_test test_bsp_es8311_sleep_check \
        "-I${fw}/bsp/src" -- \
        "${fw}/tests/test_bsp_es8311_sleep_check.c" "${fw}/bsp/src/bsp_es8311_sleep_check.c"
    host_test test_bsp_button \
        "-I${fw}/tests/bsp_stubs" "-I${fw}/bsp/include" -- \
        "${fw}/tests/test_bsp_button.c"
    host_test test_bsp_lvgl_init \
        "-I${fw}/tests/bsp_stubs" "-I${fw}/bsp/include" -- \
        "${fw}/tests/test_bsp_lvgl_init.c" "${fw}/bsp/src/bsp_display_rounding.c"
    host_test test_bsp_audio_recovery \
        "-I${fw}/tests/audio_stubs" "-I${fw}/bsp/include" "-I${fw}/bsp/src" -- \
        "${fw}/tests/test_bsp_audio_recovery.c" "${fw}/bsp/src/bsp_es8311_sleep_check.c"

    # ---- 框架 Python 测试 ----
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_portal_template.py"
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_check_repo.py"
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_verify_firmware.py"
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_archive_firmware.py"
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tests/test_install_passport_skills.py"

    # ---- 应用测试(仅应用仓):按约定发现,不写死任何应用的名字 ----
    #
    # 框架不能知道第三个应用叫什么、测什么。这里只约定两条:
    #   tests/test_*.py  —— 一律直接跑;
    #   tests/test_<stem>.c —— 编译时把 main/<stem>.c 一起带上(存在就带);
    #     名字对不上时(被测文件叫别的名字、还要第三方源码),在
    #     tests/host_tests.txt 里写一行映射:<stem> <源文件...>。
    # 框架自己那组测试是显式列出的,因为它们需要各自的桩目录。
    if [[ ${is_app} -eq 1 ]]; then
        local py c stem srcs line f
        for py in "${project_root}"/tests/test_*.py; do
            [[ -e "${py}" ]] || continue
            echo "  ${py##*/}"
            PYTHONDONTWRITEBYTECODE=1 python3 "${py}"
        done

        for c in "${project_root}"/tests/test_*.c; do
            [[ -e "${c}" ]] || continue
            stem="$(basename "${c}" .c)"; stem="${stem#test_}"
            srcs=()
            if [[ -f "${project_root}/tests/host_tests.txt" ]]; then
                line="$(awk -v s="${stem}" '$1 == s { $1=""; sub(/^ +/,""); print; exit }' \
                        "${project_root}/tests/host_tests.txt")"
                if [[ -n "${line}" ]]; then
                    for f in ${line}; do
                        srcs+=("${project_root}/${f}")
                    done
                fi
            fi
            if [[ ${#srcs[@]} -eq 0 && -f "${project_root}/main/${stem}.c" ]]; then
                srcs=("${project_root}/main/${stem}.c")
            fi
            echo "  ${c##*/}"
            host_test "${stem}" \
                "-I${project_root}/main" "-I${project_root}/tests" \
                "-I${fw}/appfw/include" \
                "-I${project_root}/tests/thirdparty/cJSON" -- \
                "${c}" "${srcs[@]}"
        done
    fi

    if [[ ${is_app} -eq 1 ]]; then
        echo "Host tests: PASS (framework + application)"
    else
        echo "Host tests: PASS (framework)"
    fi
}

run_firmware_checks() {
    if [[ ${is_app} -eq 0 ]]; then
        echo "Firmware build: SKIP (framework repository has no application)" >&2
        return 0
    fi
    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.x first." >&2
        return 1
    fi

    local validation_build_dir
    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' RETURN

    SDKCONFIG_DEFAULTS="${project_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 "${fw}/tools/verify_firmware.py" "${validation_build_dir}"
    PYTHONDONTWRITEBYTECODE=1 python3 "${fw}/tools/archive_firmware.py" create \
        "${validation_build_dir}" --archive-root "${project_root}/build/firmware"
    mkdir -p "${project_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${project_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
}

case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        echo "Usage: $0 [--all|--static|--firmware] [--project-root DIR]" >&2
        exit 2
        ;;
esac
