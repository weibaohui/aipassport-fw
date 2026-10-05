# components/appfw —— AI Passport 联网小程序公共框架。
#
# 与业务无关的通用能力:WiFi 引擎(断线自动重连/多热点回退)、
# 配网门户(常驻 HTTP+captive DNS+两阶段网页)、NVS 配置存储、轮询 API 客户端、
# UI 骨架(状态机/光标行/吐司/熄屏)、MCP 常驻服务。业务差异(解析、主页内容、
# 扩展配置、门户卡片)一律由应用通过回调与注入点提供,见各头文件说明。
#
# 边界规则:本目录不得出现任何具体应用的业务词汇、字段名或品牌串。
#   需要新能力时加注入点,不要加 if(app==X) 分支。
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
         "src/appfw_bars.c"
         "src/appfw_mcp_srv.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_app_format esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             json esp_http_client esp-tls esp_http_server lwip mbedtls
)
