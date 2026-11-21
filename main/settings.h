#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[128];    /* ONO base URL, e.g. http://192.168.1.1:8787 */
    char token[128];         /* bearer token; empty = none */
    bool tls_verify;         /* verify HTTPS certificate against the CA bundle */
    uint16_t poll_s;         /* refresh interval, 5..300 s */
    bool wake_on_alert;      /* turn the screen on when a device goes down */
    uint8_t brightness;      /* 5..100 % */
    uint8_t vcom;            /* LCD panel VCOM register (image-retention tuning) */
    uint8_t tz;              /* index into the timezone table */
    bool flip;               /* screen rotated 180 degrees */
    bool saver;              /* screen saver: backlight off after inactivity */
    uint16_t saver_min;      /* screen saver timeout, minutes */
} app_settings_t;

void settings_init(void);
/* Returns a pointer to the live settings. Only the UI task mutates it; the
 * ONO client copies what it needs under settings_lock(). */
app_settings_t *settings_get(void);
void settings_save(void);
void settings_lock(void);
void settings_unlock(void);

/* Timezones offered in Settings (POSIX TZ rules, so DST switches automatically) */
int tz_count(void);
const char *tz_name(int i);
/* Newline-separated list for an LVGL dropdown */
const char *tz_dropdown_options(void);
/* Apply settings->tz to the C library (localtime) */
void settings_apply_tz(void);
