#include "app_state.h"
#include "esp_timer.h"
#include <string.h>

monitor_state_t g_state;
SemaphoreHandle_t g_state_mux = NULL;

void app_state_init(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.phase = APP_PHASE_BOOT;
    strlcpy(g_state.gcode_state, "...", sizeof(g_state.gcode_state));
    g_state_mux = xSemaphoreCreateMutex();
}

uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000LL);
}
