/*
 * Hooks the P4 Fleet agent (components from p4-fleet/agent) into this app:
 * OTA and revert go through ota.c, identify blinks the backlight, and the
 * ONO client's state is reported as app status.
 */
#include "fleet.h"

#include <stdlib.h>
#include "board.h"
#include "cJSON.h"
#include "crashlog.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fleet_agent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ono_client.h"
#include "ota.h"
#include "settings.h"
#include "wifi.h"

static void fill_status(cJSON *s)
{
    ota_status_t st;
    ota_get_status(&st);
    cJSON_AddBoolToObject(s, "can_revert", st.can_revert);
    cJSON_AddStringToObject(s, "prev_version", st.prev_version);
    cJSON_AddStringToObject(s, "last_crash", crashlog_text());
    cJSON_AddStringToObject(s, "c6_version", st.c6_version_known ? st.c6_version : "");
    cJSON_AddStringToObject(s, "c6_bundled", st.c6_bundled);
    cJSON_AddBoolToObject(s, "c6_update_available", st.c6_update_available);
    cJSON_AddStringToObject(s, "wifi", wifi_state_str());
    cJSON_AddBoolToObject(s, "time_synced", time_is_synced());
    cJSON_AddBoolToObject(s, "screen_on", !board_screen_is_off());
}

static void fill_app(cJSON *a)
{
    /* ono_data_t is large: keep it off the HTTP server task's stack */
    ono_data_t *d = heap_caps_malloc(sizeof(*d), MALLOC_CAP_SPIRAM);
    if (!d) return;
    ono_client_get(d);
    cJSON_AddStringToObject(a, "ono_state", ono_state_str(d->state));
    cJSON_AddBoolToObject(a, "ono_ok", d->state == ONO_ST_OK);
    if (d->error[0]) cJSON_AddStringToObject(a, "ono_error", d->error);
    if (d->updated_us) {
        cJSON_AddNumberToObject(a, "last_poll_age_s", (double)((esp_timer_get_time() - d->updated_us) / 1000000));
    }
    cJSON_AddNumberToObject(a, "poll_ms", d->poll_ms);
    if (d->have_overview) {
        cJSON_AddNumberToObject(a, "devices", d->devices);
        cJSON_AddNumberToObject(a, "devices_down", d->down_total);
        cJSON_AddNumberToObject(a, "open_alerts", d->open_alerts);
        cJSON_AddNumberToObject(a, "services_down", d->svc_down);
    }
    free(d);
}

static void identify_task(void *arg)
{
    board_screen_wake();
    for (int i = 0; i < 6; i++) {
        board_set_backlight(i % 2 ? 100 : 5);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    board_set_backlight(settings_get()->brightness);
    vTaskDelete(NULL);
}

static void identify(void)
{
    xTaskCreate(identify_task, "fleet_ident", 2560, NULL, 3, NULL);
}

void fleet_start(void)
{
    const fleet_agent_config_t cfg = {
        .label = "ONO display",
        .register_uri = ota_register_uri,
        .ota_receive = ota_receive,
        .revert = ota_revert,
        .identify = identify,
        .rssi = wifi_rssi,
        .fill_status = fill_status,
        .fill_app = fill_app,
    };
    fleet_agent_start(&cfg);
}
