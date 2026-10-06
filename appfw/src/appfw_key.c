// appfw_key.c —— 按键事件映射的唯一事实来源。
#include "appfw_key.h"

#include <stddef.h>

#include "bsp_button.h"

bool appfw_key_event_from_bsp(int bsp_event, appfw_key_event_t *out)
{
    if (out == NULL) {
        return false;
    }

    switch ((bsp_btn_ev_t)bsp_event) {
    case BSP_BTN_PRESS:
        *out = APPFW_KEY_EV_PRESS;
        return true;
    case BSP_BTN_CLICK:
        *out = APPFW_KEY_EV_CLICK;
        return true;
    case BSP_BTN_DOUBLE:
        *out = APPFW_KEY_EV_DOUBLE;
        return true;
    case BSP_BTN_LONG:
        *out = APPFW_KEY_EV_LONG;
        return true;
    case BSP_BTN_LONG_UP:
        *out = APPFW_KEY_EV_LONG_UP;
        return true;
    default:
        return false;
    }
}

bool appfw_key_event_is_lifecycle(appfw_key_event_t event)
{
    return event == APPFW_KEY_EV_PRESS || event == APPFW_KEY_EV_LONG_UP;
}
