/*
 * Remote display for Open Network Observatory (ONO) on the Guition JC8012P4A1
 * (ESP32-P4 + ESP32-C6, 10.1" 800x1280 touch).
 *
 * Polls the ONO HTTP API over Wi-Fi and shows the overview (devices down, open
 * alerts, traffic, top talkers and services), the event feed and the network
 * inventory. Board, Wi-Fi, OTA and crash-log code come from the ESP32-P4
 * ADS-B display.
 */
#include "board.h"
#include "crashlog.h"
#include "esp_log.h"
#include "fleet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ono_client.h"
#include "ota.h"
#include "settings.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "main";

static void lvgl_task(void *arg)
{
    lv_display_t *disp = board_init();
    ui_init(disp);
    for (;;) {
        uint32_t wait = lv_timer_handler();
        if (wait > 20) wait = 20;
        if (wait < 2) wait = 2;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void app_main(void)
{
    crashlog_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ota_init();
    ota_report_selftest(true);     /* no decoder here; boot is confirmed once the UI is up and Wi-Fi connects */
    settings_init();
    ono_client_start();            /* before the UI so its first snapshot is valid */

    /* LVGL owns core 0's spare time; network work runs on core 1 */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 16384, NULL, 4, NULL, 0);

    wifi_init();
    ota_start_server();
    fleet_start();                 /* p4-fleet: discovery, health, fleet OTA */
    ESP_LOGI(TAG, "started");
}
