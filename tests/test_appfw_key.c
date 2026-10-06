#include "appfw_key.h"

#include <assert.h>
#include <stdio.h>

static void assert_mapping(int bsp_event, appfw_key_event_t expected)
{
    appfw_key_event_t actual = (appfw_key_event_t)-1;
    assert(appfw_key_event_from_bsp(bsp_event, &actual));
    assert(actual == expected);
}

int main(void)
{
    assert_mapping(0, APPFW_KEY_EV_PRESS);
    assert_mapping(1, APPFW_KEY_EV_CLICK);
    assert_mapping(2, APPFW_KEY_EV_DOUBLE);
    assert_mapping(3, APPFW_KEY_EV_LONG);
    assert_mapping(4, APPFW_KEY_EV_LONG_UP);

    appfw_key_event_t value = APPFW_KEY_EV_CLICK;
    assert(!appfw_key_event_from_bsp(-1, &value));
    assert(value == APPFW_KEY_EV_CLICK);
    assert(!appfw_key_event_from_bsp(99, &value));
    assert(!appfw_key_event_from_bsp(1, NULL));

    assert(appfw_key_event_is_lifecycle(APPFW_KEY_EV_PRESS));
    assert(appfw_key_event_is_lifecycle(APPFW_KEY_EV_LONG_UP));
    assert(!appfw_key_event_is_lifecycle(APPFW_KEY_EV_CLICK));
    assert(!appfw_key_event_is_lifecycle(APPFW_KEY_EV_DOUBLE));
    assert(!appfw_key_event_is_lifecycle(APPFW_KEY_EV_LONG));

    puts("appfw_key: PASS");
    return 0;
}
