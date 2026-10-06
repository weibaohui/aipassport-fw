// appfw_key.h —— BSP 按键事件到框架语义事件的纯映射。
//
// BSP 暴露完整的按下/抬起生命周期；旧 UI 契约只把单击/双击/长按送进
// home_key。这里把映射独立成纯逻辑，供 appfw_ui 和主机测试共同使用，
// 避免每个应用再维护一套 0/1/2/3/4 魔法数。
#pragma once

#include <stdbool.h>

typedef enum {
    APPFW_KEY_EV_PRESS = 0,   // 按下瞬间；只在主页派发给 full_key
    APPFW_KEY_EV_CLICK,       // 单击
    APPFW_KEY_EV_DOUBLE,      // 双击
    APPFW_KEY_EV_LONG,        // 长按阈值触发
    APPFW_KEY_EV_LONG_UP,     // 长按后松开；始终派发，保证按住类动作能收尾
} appfw_key_event_t;

// BSP 原始事件转换。无效值返回 false 且不改写 out。
bool appfw_key_event_from_bsp(int bsp_event, appfw_key_event_t *out);

// 判断事件是否属于按住动作的生命周期。派发策略由 appfw_ui 决定：
// PRESS 只在主页开始动作，LONG_UP 始终派发以免页面切换吞掉释放。
bool appfw_key_event_is_lifecycle(appfw_key_event_t event);
