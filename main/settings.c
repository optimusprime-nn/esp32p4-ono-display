#include "settings.h"

#include "board.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NS = "ono";

static app_settings_t s_cfg;
static SemaphoreHandle_t s_mx;

typedef struct {
    const char *name;
    const char *posix;
} tz_entry_t;

static const tz_entry_t s_tz[] = {
    {"UTC",                         "UTC0"},
    {"Hawaii (HST)",                "HST10"},
    {"Alaska",                      "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US Pacific",                  "PST8PDT,M3.2.0,M11.1.0"},
    {"US Mountain",                 "MST7MDT,M3.2.0,M11.1.0"},
    {"Arizona",                     "MST7"},
    {"US Central",                  "CST6CDT,M3.2.0,M11.1.0"},
    {"US Eastern",                  "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic",                    "AST4ADT,M3.2.0,M11.1.0"},
    {"UK / Ireland",                "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Central Europe",              "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe",              "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Gulf (UAE)",                  "<+04>-4"},
    {"India",                       "IST-5:30"},
    {"China / Singapore / Perth",   "CST-8"},
    {"Japan / Korea",               "JST-9"},
    {"Australia East",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand",                 "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

int tz_count(void) { return sizeof(s_tz) / sizeof(s_tz[0]); }
const char *tz_name(int i) { return (i >= 0 && i < tz_count()) ? s_tz[i].name : "?"; }

const char *tz_dropdown_options(void)
{
    static char buf[512];
    if (!buf[0]) {
        for (int i = 0; i < tz_count(); i++) {
            if (i) strlcat(buf, "\n", sizeof(buf));
            strlcat(buf, s_tz[i].name, sizeof(buf));
        }
    }
    return buf;
}

void settings_apply_tz(void)
{
    int i = s_cfg.tz < tz_count() ? s_cfg.tz : 0;
    setenv("TZ", s_tz[i].posix, 1);
    tzset();
}

void settings_lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
void settings_unlock(void) { xSemaphoreGive(s_mx); }

static void load_defaults(app_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->wifi_ssid, CONFIG_ONO_WIFI_SSID, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, CONFIG_ONO_WIFI_PASSWORD, sizeof(c->wifi_pass));
    strlcpy(c->server_url, CONFIG_ONO_SERVER_URL, sizeof(c->server_url));
    strlcpy(c->token, CONFIG_ONO_TOKEN, sizeof(c->token));
#if CONFIG_ONO_TLS_VERIFY
    c->tls_verify = true;
#endif
    c->poll_s = CONFIG_ONO_POLL_S;
    c->wake_on_alert = true;
    c->brightness = 80;
    c->vcom = BOARD_VCOM_DEFAULT;
    c->tz = CONFIG_ONO_DEFAULT_TZ;
    c->flip = false;
    c->saver = true;
    c->saver_min = 10;
}

void settings_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    load_defaults(&s_cfg);
    settings_apply_tz();

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }
    size_t len;
    len = sizeof(s_cfg.wifi_ssid);  nvs_get_str(h, "ssid", s_cfg.wifi_ssid, &len);
    len = sizeof(s_cfg.wifi_pass);  nvs_get_str(h, "pass", s_cfg.wifi_pass, &len);
    len = sizeof(s_cfg.server_url); nvs_get_str(h, "url", s_cfg.server_url, &len);
    len = sizeof(s_cfg.token);      nvs_get_str(h, "token", s_cfg.token, &len);

    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "tlsv", &u8) == ESP_OK) s_cfg.tls_verify = u8 != 0;
    if (nvs_get_u16(h, "poll", &u16) == ESP_OK && u16 >= 5 && u16 <= 300) s_cfg.poll_s = u16;
    if (nvs_get_u8(h, "wake", &u8) == ESP_OK) s_cfg.wake_on_alert = u8 != 0;
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK && u8 >= 5 && u8 <= 100) s_cfg.brightness = u8;
    if (nvs_get_u8(h, "vcom", &u8) == ESP_OK && u8 >= BOARD_VCOM_MIN && u8 <= BOARD_VCOM_MAX) s_cfg.vcom = u8;
    if (nvs_get_u8(h, "tz", &u8) == ESP_OK && u8 < tz_count()) s_cfg.tz = u8;
    if (nvs_get_u8(h, "flip", &u8) == ESP_OK) s_cfg.flip = u8 != 0;
    if (nvs_get_u8(h, "saver", &u8) == ESP_OK) s_cfg.saver = u8 != 0;
    if (nvs_get_u16(h, "saver_min", &u16) == ESP_OK && u16 >= 1 && u16 <= 1440) s_cfg.saver_min = u16;
    nvs_close(h);
    settings_apply_tz();
    /* never log the token or Wi-Fi password */
    ESP_LOGI(TAG, "loaded: ssid='%s' server=%s token=%s poll=%us", s_cfg.wifi_ssid, s_cfg.server_url,
             s_cfg.token[0] ? "set" : "none", s_cfg.poll_s);
}

app_settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed");
        return;
    }
    nvs_set_str(h, "ssid", s_cfg.wifi_ssid);
    nvs_set_str(h, "pass", s_cfg.wifi_pass);
    nvs_set_str(h, "url", s_cfg.server_url);
    nvs_set_str(h, "token", s_cfg.token);
    nvs_set_u8(h, "tlsv", s_cfg.tls_verify ? 1 : 0);
    nvs_set_u16(h, "poll", s_cfg.poll_s);
    nvs_set_u8(h, "wake", s_cfg.wake_on_alert ? 1 : 0);
    nvs_set_u8(h, "bright", s_cfg.brightness);
    nvs_set_u8(h, "vcom", s_cfg.vcom);
    nvs_set_u8(h, "tz", s_cfg.tz);
    nvs_set_u8(h, "flip", s_cfg.flip ? 1 : 0);
    nvs_set_u8(h, "saver", s_cfg.saver ? 1 : 0);
    nvs_set_u16(h, "saver_min", s_cfg.saver_min);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
}
