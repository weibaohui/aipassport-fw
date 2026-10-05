# components/appfw —— AI Passport 联网小程序公共框架。
#
# 与业务无关的通用能力:WiFi 引擎(断线自动重连/多热点回退)、
# 配网门户(常驻 HTTP+captive DNS+两阶段网页)、NVS 配置存储、轮询 API 客户端、
# UI 骨架(状态机/光标行/吐司/熄屏)、MCP 常驻服务。业务差异(解析、主页内容、
# 扩展配置、门户卡片)一律由应用通过回调与注入点提供,见各头文件说明。
#
# 边界规则:本目录不得出现任何具体应用的业务词汇、字段名或品牌串。
#   需要新能力时加注入点,不要加 if(app==X) 分支。
# 框架版本:构建期从本仓 git describe 烧进固件(submodule 自身的工作区状态)。
execute_process(
    COMMAND git log -1 --format=%h.%cd --date=format:%y%m%d
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
    OUTPUT_VARIABLE APPFW_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APPFW_GIT_VERSION)
    set(APPFW_GIT_VERSION "unknown")
endif()
idf_component_register(
    SRCS "src/appfw_net.c"
         "src/appfw_netlist.c"
         "src/appfw_storage.c"
         "src/appfw_portal.c"
         "src/appfw_ui.c"
         "src/appfw_client.c"
         "src/appfw_icy.c"
         "src/appfw_frame.c"
         "src/appfw_hls.c"
         "src/appfw_stream.c"
         "src/appfw_mcp.c"
         "src/appfw_viz.c"
         "src/appfw_loudness.c"
         "src/appfw_netlog.c"
         "src/music_vibe.c"
         "src/appfw_bars.c"
         "src/appfw_mcp_srv.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update bsp esp_app_format esp_event esp_hw_support esp_netif esp_partition esp_timer esp_wifi nvs_flash
             json esp_http_client esp-tls esp_http_server lwip mbedtls
)

# register 之后才允许 target 级定义(全局 add_compile_definitions 会污染
# IDF 的 build_properties,引号被当语法错误)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE APPFW_VERSION="${APPFW_GIT_VERSION}")

