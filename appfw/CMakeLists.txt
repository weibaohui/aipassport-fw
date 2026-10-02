# components/appfw —— AI Passport 联网小程序公共框架。
#
# 从 GLM 用量宝应用中抽取的通用能力:WiFi 引擎(断线自动重连/多热点回退)、
# 配网门户(常驻 HTTP+captive DNS+两阶段网页)、NVS 配置存储、轮询 API 客户端、
# UI 骨架(状态机/光标行/吐司/熄屏)。业务差异(解析、主页内容、扩展配置)
# 由应用通过回调与注入点提供,见各头文件说明。
idf_component_register(
    SRCS "src/appfw_net.c"
         "src/appfw_netlist.c"
         "src/appfw_storage.c"
         "src/appfw_portal.c"
         "src/appfw_ui.c"
         "src/appfw_client.c"
         "src/appfw_files.c"
         "src/appfw_files_http.c"
         "src/appfw_files.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_app_format esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             json esp_http_client esp-tls esp_http_server lwip fatfs mbedtls
)
