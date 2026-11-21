/*
 * LVGL user interface, 800x1280 portrait.
 *
 *   top bar   ONO connection state, Wi-Fi, clock
 *   Overview  status hero (devices down / open alerts), stat tiles,
 *             traffic last hour, top talkers, top services
 *   Events    ONO event feed, tap an event to see it in full or acknowledge it
 *   Network   devices down, failing service checks, inventory by type,
 *             backups / compliance / collectors, discovery, poll details
 *   Settings  Wi-Fi, ONO server, display, firmware
 *
 * A 1 s timer copies the ONO client's snapshot and refreshes what changed.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ono_client.h"
#include "ota.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "ui";

#define SCR_W      BOARD_LCD_H_RES
#define SCR_H      BOARD_LCD_V_RES
#define TOPBAR_H   60
#define TABBAR_H   76
#define PAD        12

#define COL_BG      lv_color_hex(0x0b0f14)
#define COL_CARD    lv_color_hex(0x151b23)
#define COL_BORDER  lv_color_hex(0x263040)
#define COL_TEXT    lv_color_hex(0xe6edf3)
#define COL_DIM     lv_color_hex(0x8b949e)
#define COL_OK      lv_color_hex(0x3fb950)
#define COL_WARN    lv_color_hex(0xd29922)
#define COL_BAD     lv_color_hex(0xf85149)
#define COL_INFO    lv_color_hex(0x58a6ff)
#define COL_ACCENT  lv_color_hex(0x2f81f7)
#define COL_PORTS   lv_color_hex(0xa371f7)

enum { TAB_OVERVIEW = 0, TAB_EVENTS, TAB_NETWORK, TAB_SETTINGS };

static ono_data_t *s_d;             /* UI snapshot */
static uint32_t s_shown_gen = UINT32_MAX;
static ono_state_t s_shown_state = (ono_state_t)-1;
static int s_prev_down = -1, s_prev_svc_down = -1;
static uint32_t s_tick;

/* top bar */
static lv_obj_t *s_top_state, *s_top_wifi, *s_top_clock;
static lv_obj_t *s_tabview;

/* overview */
static lv_obj_t *s_hero, *s_hero_val, *s_hero_lbl, *s_hero_sub;
typedef struct { lv_obj_t *box, *v, *l, *s; } tile_t;
enum { T_DEVICES, T_SNMP, T_TRAFFIC, T_EXPORTERS, T_SERVICES, T_DISCOVERY, T_COUNT };
static tile_t s_tiles[T_COUNT];
static lv_obj_t *s_chart, *s_chart_info;
static lv_chart_series_t *s_chart_ser;
typedef struct { lv_obj_t *row, *name, *val, *bar; } barrow_t;
static barrow_t s_talk[ONO_MAX_TOP], s_ports[ONO_MAX_TOP];
static lv_obj_t *s_talk_empty, *s_ports_empty;

/* events */
typedef struct { lv_obj_t *row, *subj, *meta, *msg; } evrow_t;
static evrow_t s_ev[ONO_MAX_EVENTS];
static int s_ev_map[ONO_MAX_EVENTS];      /* row -> index into s_d->events */
static lv_obj_t *s_ev_sw_open, *s_ev_count, *s_ev_empty;
static ono_event_t s_ev_sel;              /* event shown in the dialog */
static lv_obj_t *s_ev_dialog;

/* network */
static lv_obj_t *s_net_down_lbl[ONO_MAX_DOWN], *s_net_down_empty, *s_net_down_title;
static lv_obj_t *s_net_svc_lbl[ONO_MAX_DOWN], *s_net_svc_empty, *s_net_svc_title;
static barrow_t s_kinds[ONO_MAX_KINDS];
static lv_obj_t *s_kinds_empty;
static lv_obj_t *s_net_health, *s_net_disc, *s_net_conn;

/* settings */
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_ssid, *s_ta_pass, *s_dd_scan, *s_btn_scan;
static lv_obj_t *s_ta_url, *s_ta_token, *s_sw_tls, *s_dd_poll, *s_sw_wake;
static lv_obj_t *s_sl_bright, *s_sl_vcom, *s_lbl_vcom, *s_dd_tz, *s_sw_flip, *s_sw_saver, *s_dd_saver;
static lv_obj_t *s_set_status, *s_fw_lbl, *s_fw_bar, *s_btn_arm, *s_btn_revert, *s_btn_c6, *s_c6_lbl;
static bool s_scan_pending;

static const uint16_t s_poll_steps[] = {5, 10, 15, 30, 60, 120, 300};
#define POLL_STEPS (int)(sizeof(s_poll_steps) / sizeof(s_poll_steps[0]))
static const uint16_t s_saver_steps[] = {1, 5, 10, 15, 30, 60, 120};
#define SAVER_STEPS (int)(sizeof(s_saver_steps) / sizeof(s_saver_steps[0]))

/* ------------------------------------------------------------------------- */
/* Formatting                                                                */
/* ------------------------------------------------------------------------- */

/* SI units, like ONO's web UI */
static void fmt_bytes(double n, char *out, size_t len, const char *unit)
{
    static const char *pfx[] = {"", "K", "M", "G", "T", "P"};
    int i = 0;
    while ((n >= 1000 || n <= -1000) && i < 5) { n /= 1000; i++; }
    if (i == 0) snprintf(out, len, "%.0f %s", n, unit);
    else snprintf(out, len, "%.*f %s%s", n < 10 ? 2 : n < 100 ? 1 : 0, n, pfx[i], unit);
}

static void fmt_num(double n, char *out, size_t len)
{
    if (n >= 1e6) snprintf(out, len, "%.1fM", n / 1e6);
    else if (n >= 1e4) snprintf(out, len, "%.0fk", n / 1e3);
    else snprintf(out, len, "%.0f", n);
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void fmt_ago(int64_t ms, char *out, size_t len)
{
    if (!ms) { strlcpy(out, "never", len); return; }
    if (!time_is_synced()) {
        time_t t = ms / 1000;
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(out, len, "%m/%d %H:%M", &lt);
        return;
    }
    int64_t s = (now_ms() - ms) / 1000;
    if (s < 0) s = 0;
    if (s < 60) snprintf(out, len, "%llds ago", (long long)s);
    else if (s < 3600) snprintf(out, len, "%lldm ago", (long long)(s / 60));
    else if (s < 86400) snprintf(out, len, "%lldh ago", (long long)(s / 3600));
    else snprintf(out, len, "%lldd ago", (long long)(s / 86400));
}

static const char *port_svc(int p)
{
    switch (p) {
    case 22: return "ssh";      case 23: return "telnet";   case 25: return "smtp";    case 53: return "dns";
    case 67: return "dhcp";     case 80: return "http";     case 110: return "pop3";   case 123: return "ntp";
    case 137: return "netbios"; case 143: return "imap";    case 161: return "snmp";   case 389: return "ldap";
    case 443: return "https";   case 445: return "smb";     case 514: return "syslog"; case 587: return "submission";
    case 853: return "dot";     case 993: return "imaps";   case 1194: return "openvpn"; case 1433: return "mssql";
    case 1883: return "mqtt";   case 2055: return "netflow"; case 3306: return "mysql"; case 3389: return "rdp";
    case 5060: return "sip";    case 5353: return "mdns";   case 5432: return "postgres"; case 8080: return "http-alt";
    case 8443: return "https-alt"; case 51820: return "wireguard";
    default: return NULL;
    }
}

static const char *proto_name(int p)
{
    switch (p) {
    case 1: return "icmp"; case 6: return "tcp"; case 17: return "udp"; case 47: return "gre";
    case 50: return "esp"; case 58: return "icmpv6"; case 132: return "sctp";
    default: return NULL;
    }
}

static void fmt_port(int port, int proto, char *out, size_t len)
{
    char pr[16] = "";
    if (proto >= 0) {
        const char *n = proto_name(proto);
        if (n) snprintf(pr, sizeof(pr), "/%s", n);
        else snprintf(pr, sizeof(pr), "/%d", proto);
    }
    if (port < 0) { snprintf(out, len, "-%s", pr); return; }
    const char *s = port_svc(port);
    if (s) snprintf(out, len, "%d %s%s", port, s, pr);
    else snprintf(out, len, "%d%s", port, pr);
}

static const char *kind_label(const char *k)
{
    static const struct { const char *k, *l; } m[] = {
        {"router", "Router"}, {"firewall", "Firewall"}, {"switch", "Switch"}, {"access_point", "Access point"},
        {"server", "Server"}, {"workstation", "Workstation"}, {"printer", "Printer"}, {"phone", "Phone"},
        {"iot", "IoT"}, {"unknown", "Unknown"}, {"external", "External"},
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) if (!strcmp(k, m[i].k)) return m[i].l;
    return k;
}

static lv_color_t kind_color(int i)
{
    static const uint32_t pal[] = {0x2f81f7, 0x3fb950, 0xd29922, 0xa371f7, 0xdb61a2, 0x39c5cf,
                                   0xf0883e, 0x8b949e, 0x56d364, 0x79c0ff, 0xe3b341, 0xff7b72};
    return lv_color_hex(pal[i % (sizeof(pal) / sizeof(pal[0]))]);
}

static const char *event_label(const char *k)
{
    static const struct { const char *k, *l; } m[] = {
        {"device_down", "Device down"}, {"device_up", "Device up"}, {"new_device", "New device"},
        {"interface_down", "Interface down"}, {"interface_up", "Interface up"},
        {"exporter_silent", "Exporter silent"}, {"exporter_resumed", "Exporter resumed"},
        {"inventory", "Inventory"}, {"syslog", "Syslog"}, {"trap", "SNMP trap"}, {"test", "Test"},
        {"interface_high", "Interface busy"}, {"interface_normal", "Interface normal"},
        {"traffic_high", "High traffic"}, {"traffic_normal", "Traffic normal"},
        {"service_down", "Service failing"}, {"service_up", "Service working"},
        {"cert_expiring", "Certificate expiring"}, {"config_changed", "Configuration changed"},
        {"backup_failed", "Backup failed"}, {"backup_recovered", "Backup working"},
        {"compliance_failed", "Compliance rule broken"}, {"compliance_passed", "Compliance rule met"},
        {"collector_silent", "Collector silent"}, {"collector_resumed", "Collector reporting again"},
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) if (!strcmp(k, m[i].k)) return m[i].l;
    return k;
}

static lv_color_t sev_color(const char *s)
{
    if (!strncmp(s, "crit", 4) || !strcmp(s, "error")) return COL_BAD;
    if (!strncmp(s, "warn", 4)) return COL_WARN;
    return COL_INFO;
}

/* ------------------------------------------------------------------------- */
/* Building blocks                                                           */
/* ------------------------------------------------------------------------- */

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    return l;
}

static lv_obj_t *card(lv_obj_t *parent, const char *title, lv_obj_t **title_out)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, COL_CARD, 0);
    lv_obj_set_style_border_color(c, COL_BORDER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *t = label(c, title, &lv_font_montserrat_20, COL_TEXT);
        if (title_out) *title_out = t;
    }
    return c;
}

static lv_obj_t *tab_page(lv_obj_t *tab)
{
    lv_obj_set_style_pad_all(tab, PAD, 0);
    lv_obj_set_style_pad_row(tab, PAD, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(tab, COL_BG, 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    return tab;
}

static barrow_t bar_row(lv_obj_t *parent, lv_color_t col)
{
    barrow_t r;
    r.row = lv_obj_create(parent);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r.row, 12, 0);
    lv_obj_set_style_pad_ver(r.row, 3, 0);
    r.name = label(r.row, "", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(r.name, 330);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
    r.val = label(r.row, "", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(r.val, 110);
    lv_obj_set_style_text_align(r.val, LV_TEXT_ALIGN_RIGHT, 0);
    r.bar = lv_bar_create(r.row);
    lv_obj_set_height(r.bar, 12);
    lv_obj_set_flex_grow(r.bar, 1);
    lv_bar_set_range(r.bar, 0, 1000);
    lv_obj_set_style_bg_color(r.bar, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r.bar, col, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r.bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(r.bar, 3, LV_PART_INDICATOR);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    return r;
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void tile_set(int i, const char *v, const char *l, const char *s, lv_color_t vcol, bool alert)
{
    lv_label_set_text(s_tiles[i].v, v);
    lv_obj_set_style_text_color(s_tiles[i].v, vcol, 0);
    lv_label_set_text(s_tiles[i].l, l);
    lv_label_set_text(s_tiles[i].s, s);
    lv_obj_set_style_border_color(s_tiles[i].box, alert ? COL_BAD : COL_BORDER, 0);
}

/* ------------------------------------------------------------------------- */
/* Top bar                                                                   */
/* ------------------------------------------------------------------------- */

static void build_topbar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, SCR_W, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 18, 0);

    label(bar, "ONO", &lv_font_montserrat_24, COL_TEXT);
    s_top_state = label(bar, "", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_flex_grow(s_top_state, 1);
    lv_label_set_long_mode(s_top_state, LV_LABEL_LONG_MODE_DOTS);
    s_top_wifi = label(bar, "", &lv_font_montserrat_16, COL_DIM);
    s_top_clock = label(bar, "--:--", &lv_font_montserrat_20, COL_TEXT);
}

static void update_topbar(void)
{
    switch (s_d->state) {
    case ONO_ST_OK:
        lv_label_set_text_fmt(s_top_state, LV_SYMBOL_OK " Updated %llds ago",
                              (long long)((esp_timer_get_time() - s_d->updated_us) / 1000000));
        lv_obj_set_style_text_color(s_top_state, COL_OK, 0);
        break;
    case ONO_ST_CONNECTING:
    case ONO_ST_WAIT_WIFI:
        lv_label_set_text_fmt(s_top_state, LV_SYMBOL_REFRESH " %s", ono_state_str(s_d->state));
        lv_obj_set_style_text_color(s_top_state, COL_DIM, 0);
        break;
    default:
        lv_label_set_text_fmt(s_top_state, LV_SYMBOL_WARNING " %s%s", ono_state_str(s_d->state),
                              s_d->gen ? " (data stale)" : "");
        lv_obj_set_style_text_color(s_top_state, s_d->state == ONO_ST_NO_CONFIG ? COL_WARN : COL_BAD, 0);
        break;
    }
    if (wifi_is_connected()) {
        lv_label_set_text_fmt(s_top_wifi, LV_SYMBOL_WIFI " %d dBm", wifi_rssi());
        lv_obj_set_style_text_color(s_top_wifi, COL_DIM, 0);
    } else {
        lv_label_set_text_fmt(s_top_wifi, LV_SYMBOL_WIFI " %s", wifi_state_str());
        lv_obj_set_style_text_color(s_top_wifi, COL_WARN, 0);
    }
    if (time_is_synced()) {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        lv_label_set_text_fmt(s_top_clock, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        lv_label_set_text(s_top_clock, "--:--");
    }
}

/* ------------------------------------------------------------------------- */
/* Overview                                                                  */
/* ------------------------------------------------------------------------- */

static void goto_tab_cb(lv_event_t *e)
{
    lv_tabview_set_active(s_tabview, (uint32_t)(uintptr_t)lv_event_get_user_data(e), LV_ANIM_OFF);
}

static void build_overview(lv_obj_t *tab)
{
    tab_page(tab);

    /* hero: the one thing to look at from across the room */
    s_hero = card(tab, NULL, NULL);
    lv_obj_set_style_border_width(s_hero, 3, 0);
    lv_obj_set_style_pad_all(s_hero, 20, 0);
    lv_obj_add_flag(s_hero, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hero, goto_tab_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)TAB_EVENTS);
    s_hero_val = label(s_hero, "...", &lv_font_montserrat_48, COL_TEXT);
    s_hero_lbl = label(s_hero, "Connecting to ONO", &lv_font_montserrat_20, COL_DIM);
    s_hero_sub = label(s_hero, "", &lv_font_montserrat_16, COL_DIM);
    lv_obj_set_width(s_hero_sub, lv_pct(100));
    lv_label_set_long_mode(s_hero_sub, LV_LABEL_LONG_MODE_WRAP);

    /* stat tiles, 3 x 2 */
    lv_obj_t *grid = lv_obj_create(tab);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, PAD, 0);
    lv_obj_set_style_pad_column(grid, PAD, 0);
    int tw = (SCR_W - 2 * PAD - 2 * PAD) / 3;
    for (int i = 0; i < T_COUNT; i++) {
        tile_t *t = &s_tiles[i];
        t->box = card(grid, NULL, NULL);
        lv_obj_set_width(t->box, tw);
        lv_obj_set_style_pad_row(t->box, 2, 0);
        t->v = label(t->box, "-", &lv_font_montserrat_32, COL_TEXT);
        t->l = label(t->box, "", &lv_font_montserrat_16, COL_TEXT);
        t->s = label(t->box, "", &lv_font_montserrat_14, COL_DIM);
        lv_obj_set_width(t->s, lv_pct(100));
        lv_label_set_long_mode(t->s, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_add_flag(s_tiles[T_SERVICES].box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tiles[T_SERVICES].box, goto_tab_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)TAB_NETWORK);
    lv_obj_add_flag(s_tiles[T_DEVICES].box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tiles[T_DEVICES].box, goto_tab_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)TAB_NETWORK);

    /* traffic chart */
    lv_obj_t *c = card(tab, "Traffic, last hour", NULL);
    s_chart_info = label(c, "", &lv_font_montserrat_14, COL_DIM);
    s_chart = lv_chart_create(c);
    lv_obj_set_size(s_chart, lv_pct(100), 220);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_chart, ONO_SERIES_N);
    lv_chart_set_div_line_count(s_chart, 4, 0);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_pad_column(s_chart, 2, 0);
    lv_obj_set_style_line_color(s_chart, COL_BORDER, LV_PART_MAIN);
    s_chart_ser = lv_chart_add_series(s_chart, COL_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < ONO_SERIES_N; i++) lv_chart_set_series_value_by_id(s_chart, s_chart_ser, i, 0);
    lv_obj_t *axis = lv_obj_create(c);
    lv_obj_remove_style_all(axis);
    lv_obj_set_size(axis, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *a0 = label(axis, "-60 min", &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(a0, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *a1 = label(axis, "-30", &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(a1, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *a2 = label(axis, "now", &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(a2, LV_ALIGN_RIGHT_MID, 0, 0);

    /* top talkers / services */
    c = card(tab, "Top talkers (1 h)", NULL);
    for (int i = 0; i < ONO_MAX_TOP; i++) s_talk[i] = bar_row(c, COL_ACCENT);
    s_talk_empty = label(c, "No flow data. Point a NetFlow v9 exporter at ONO (UDP 2055).", &lv_font_montserrat_14, COL_DIM);
    c = card(tab, "Top services (1 h)", NULL);
    for (int i = 0; i < ONO_MAX_TOP; i++) s_ports[i] = bar_row(c, COL_PORTS);
    s_ports_empty = label(c, "No flow data.", &lv_font_montserrat_14, COL_DIM);
}

static void update_hero(void)
{
    char buf[256];
    if (!s_d->have_overview) {
        lv_label_set_text(s_hero_val, s_d->state == ONO_ST_NO_CONFIG ? LV_SYMBOL_SETTINGS : "...");
        lv_label_set_text(s_hero_lbl, ono_state_str(s_d->state));
        lv_label_set_text(s_hero_sub, s_d->error[0] ? s_d->error : "");
        lv_obj_set_style_border_color(s_hero, COL_BORDER, 0);
        lv_obj_set_style_text_color(s_hero_val, COL_DIM, 0);
        return;
    }
    lv_color_t col;
    if (s_d->down_total > 0) {
        col = COL_BAD;
        lv_label_set_text_fmt(s_hero_val, LV_SYMBOL_WARNING " %d down", s_d->down_total);
        lv_label_set_text(s_hero_lbl, s_d->down_total == 1 ? "Monitored device down" : "Monitored devices down");
        buf[0] = 0;
        for (int i = 0; i < s_d->down_count; i++) {
            if (i) strlcat(buf, "   ", sizeof(buf));
            strlcat(buf, LV_SYMBOL_CLOSE " ", sizeof(buf));
            strlcat(buf, s_d->down[i].name, sizeof(buf));
        }
        if (s_d->down_total > s_d->down_count) {
            char more[24];
            snprintf(more, sizeof(more), "   +%d more", s_d->down_total - s_d->down_count);
            strlcat(buf, more, sizeof(buf));
        }
        lv_label_set_text(s_hero_sub, buf);
    } else if (s_d->open_alerts > 0) {
        col = COL_WARN;
        lv_label_set_text_fmt(s_hero_val, "%d", s_d->open_alerts);
        lv_label_set_text(s_hero_lbl, s_d->open_alerts == 1 ? "Open alert" : "Open alerts");
        lv_label_set_text(s_hero_sub, "Unacknowledged warnings - tap to see events");
    } else {
        col = COL_OK;
        lv_label_set_text(s_hero_val, LV_SYMBOL_OK " All quiet");
        lv_label_set_text(s_hero_lbl, "No devices down, no open alerts");
        lv_label_set_text(s_hero_sub, "");
    }
    if (s_d->state != ONO_ST_OK) {
        snprintf(buf, sizeof(buf), "%s - showing the last data received. %s", ono_state_str(s_d->state), s_d->error);
        lv_label_set_text(s_hero_sub, buf);
    }
    lv_obj_set_style_text_color(s_hero_val, col, 0);
    lv_obj_set_style_border_color(s_hero, s_d->state == ONO_ST_OK ? col : COL_DIM, 0);
}

static void update_tiles(void)
{
    char v[32], s[96], a[24], b[24];
    if (!s_d->have_overview) {
        for (int i = 0; i < T_COUNT; i++) tile_set(i, "-", "", "", COL_DIM, false);
        return;
    }
    snprintf(v, sizeof(v), "%d", s_d->devices);
    snprintf(s, sizeof(s), "%d up", s_d->devices_up);
    tile_set(T_DEVICES, v, "Devices", s, COL_TEXT, false);

    snprintf(v, sizeof(v), "%d", s_d->snmp_devices);
    snprintf(s, sizeof(s), "%d links mapped", s_d->links);
    tile_set(T_SNMP, v, "Managed (SNMP)", s, COL_TEXT, false);

    fmt_bytes(s_d->bytes_last_hour, v, sizeof(v), "B");
    fmt_num(s_d->flows_last_hour, a, sizeof(a));
    snprintf(s, sizeof(s), "%s flows", a);
    tile_set(T_TRAFFIC, v, "Traffic, 1 h", s, COL_TEXT, false);

    snprintf(v, sizeof(v), "%d", s_d->exporters);
    if (s_d->have_stats && s_d->nf_running) {
        fmt_num(s_d->nf_datagrams, a, sizeof(a));
        fmt_num(s_d->nf_drops, b, sizeof(b));
        snprintf(s, sizeof(s), "%s dgrams, %s dropped", a, b);
    } else if (s_d->have_stats) {
        snprintf(s, sizeof(s), "%s", s_d->nf_error[0] ? s_d->nf_error : "collector off");
    } else {
        s[0] = 0;
    }
    tile_set(T_EXPORTERS, v, "Flow exporters", s, COL_TEXT, false);

    if (s_d->svc_total > 0) {
        if (s_d->svc_down) snprintf(v, sizeof(v), "%d failing", s_d->svc_down);
        else snprintf(v, sizeof(v), "%d ok", s_d->svc_total);
        if (s_d->svc_certs_expiring) snprintf(s, sizeof(s), "%d cert(s) expiring", s_d->svc_certs_expiring);
        else snprintf(s, sizeof(s), "%d checks", s_d->svc_total);
        tile_set(T_SERVICES, v, "Service checks", s, s_d->svc_down ? COL_BAD : COL_OK, s_d->svc_down > 0);
    } else {
        tile_set(T_SERVICES, "-", "Service checks", "none configured", COL_DIM, false);
    }

    if (s_d->have_stats) {
        if (s_d->disc_running) {
            snprintf(v, sizeof(v), "%d/%d", s_d->disc_probed, s_d->disc_total);
            snprintf(s, sizeof(s), "scanning %s", s_d->disc_phase);
        } else {
            fmt_ago(s_d->disc_last_ms, v, sizeof(v));
            if (s_d->disc_next_ms) {
                time_t t = s_d->disc_next_ms / 1000;
                struct tm lt;
                localtime_r(&t, &lt);
                snprintf(s, sizeof(s), "next %02d:%02d", lt.tm_hour, lt.tm_min);
            } else {
                strlcpy(s, "on demand", sizeof(s));
            }
        }
        tile_set(T_DISCOVERY, v, "Discovery", s, COL_TEXT, false);
    } else {
        tile_set(T_DISCOVERY, "-", "Discovery", "no collector stats", COL_DIM, false);
    }
}

static void update_chart(void)
{
    double peak = 0, total = 0;
    for (int i = 0; i < ONO_SERIES_N; i++) {
        if (s_d->series_bytes[i] > peak) peak = s_d->series_bytes[i];
        total += s_d->series_bytes[i];
    }
    /* chart in kbit/s averaged over each minute */
    int32_t max_kbps = (int32_t)(peak * 8 / 60 / 1000) + 1;
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, max_kbps + max_kbps / 10 + 1);
    for (int i = 0; i < ONO_SERIES_N; i++) {
        lv_chart_set_series_value_by_id(s_chart, s_chart_ser, i, (int32_t)(s_d->series_bytes[i] * 8 / 60 / 1000));
    }
    lv_chart_refresh(s_chart);
    char now[24], pk[24], tot[24];
    fmt_bytes(s_d->series_bytes[ONO_SERIES_N - 1] * 8 / 60, now, sizeof(now), "bit/s");
    fmt_bytes(peak * 8 / 60, pk, sizeof(pk), "bit/s");
    fmt_bytes(total, tot, sizeof(tot), "B");
    if (s_d->have_series) lv_label_set_text_fmt(s_chart_info, "Now %s   Peak %s   Total %s", now, pk, tot);
    else lv_label_set_text(s_chart_info, "No data");
}

static void update_top_list(barrow_t *rows, const ono_top_t *items, int n, bool ports, lv_obj_t *empty)
{
    double max = 1;
    for (int i = 0; i < n; i++) if (items[i].bytes > max) max = items[i].bytes;
    for (int i = 0; i < ONO_MAX_TOP; i++) {
        if (i >= n) { set_hidden(rows[i].row, true); continue; }
        char name[112], val[24];
        if (ports) fmt_port(items[i].port, items[i].proto, name, sizeof(name));
        else if (items[i].label[0]) snprintf(name, sizeof(name), "%s  (%s)", items[i].label, items[i].key);
        else strlcpy(name, items[i].key[0] ? items[i].key : "-", sizeof(name));
        lv_label_set_text(rows[i].name, name);
        fmt_bytes(items[i].bytes, val, sizeof(val), "B");
        lv_label_set_text(rows[i].val, val);
        lv_bar_set_value(rows[i].bar, (int32_t)(items[i].bytes / max * 1000), LV_ANIM_OFF);
        set_hidden(rows[i].row, false);
    }
    set_hidden(empty, n > 0);
}

/* ------------------------------------------------------------------------- */
/* Events                                                                    */
/* ------------------------------------------------------------------------- */

static void ev_dialog_close(void)
{
    if (s_ev_dialog) {
        lv_msgbox_close(s_ev_dialog);
        s_ev_dialog = NULL;
    }
}

static void ev_ack_cb(lv_event_t *e)
{
    ono_client_ack(s_ev_sel.id);
    ESP_LOGI(TAG, "acknowledge event %lld", (long long)s_ev_sel.id);
    ev_dialog_close();
}

static void ev_close_cb(lv_event_t *e)
{
    ev_dialog_close();
}

static void ev_row_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    int idx = s_ev_map[row];
    if (idx < 0 || idx >= s_d->event_count) return;
    s_ev_sel = s_d->events[idx];
    ev_dialog_close();

    char when[40], ago[24], body[400];
    time_t t = s_ev_sel.ts_ms / 1000;
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &lt);
    fmt_ago(s_ev_sel.ts_ms, ago, sizeof(ago));
    snprintf(body, sizeof(body), "%s  (%s)\nSeverity: %s\nWhen: %s (%s)\n\n%s", event_label(s_ev_sel.kind),
             s_ev_sel.kind, s_ev_sel.severity, when, ago, s_ev_sel.message);

    s_ev_dialog = lv_msgbox_create(NULL);
    lv_obj_set_width(s_ev_dialog, SCR_W - 80);
    lv_msgbox_add_title(s_ev_dialog, s_ev_sel.subject[0] ? s_ev_sel.subject : "Event");
    lv_obj_t *txt = lv_msgbox_add_text(s_ev_dialog, body);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_20, 0);
    if (!s_ev_sel.acked) {
        lv_obj_t *b = lv_msgbox_add_footer_button(s_ev_dialog, LV_SYMBOL_OK " Acknowledge");
        lv_obj_add_event_cb(b, ev_ack_cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *b = lv_msgbox_add_footer_button(s_ev_dialog, "Close");
    lv_obj_add_event_cb(b, ev_close_cb, LV_EVENT_CLICKED, NULL);
}

static void update_events(void);

static void ev_filter_cb(lv_event_t *e)
{
    update_events();
}

static void build_events(lv_obj_t *tab)
{
    tab_page(tab);
    lv_obj_set_style_pad_row(tab, 8, 0);

    lv_obj_t *hdr = lv_obj_create(tab);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 12, 0);
    s_ev_count = label(hdr, "Events", &lv_font_montserrat_24, COL_TEXT);
    lv_obj_set_flex_grow(s_ev_count, 1);
    label(hdr, "Open only", &lv_font_montserrat_16, COL_DIM);
    s_ev_sw_open = lv_switch_create(hdr);
    lv_obj_add_event_cb(s_ev_sw_open, ev_filter_cb, LV_EVENT_VALUE_CHANGED, NULL);

    for (int i = 0; i < ONO_MAX_EVENTS; i++) {
        evrow_t *r = &s_ev[i];
        /* severity shows as a thick left border */
        r->row = card(tab, NULL, NULL);
        lv_obj_set_style_pad_all(r->row, 12, 0);
        lv_obj_set_style_pad_left(r->row, 18, 0);
        lv_obj_set_style_pad_row(r->row, 4, 0);
        lv_obj_set_style_border_side(r->row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(r->row, 8, 0);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r->row, ev_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        r->subj = label(r->row, "", &lv_font_montserrat_20, COL_TEXT);
        lv_obj_set_width(r->subj, lv_pct(100));
        lv_label_set_long_mode(r->subj, LV_LABEL_LONG_MODE_DOTS);
        r->meta = label(r->row, "", &lv_font_montserrat_14, COL_DIM);
        r->msg = label(r->row, "", &lv_font_montserrat_16, COL_TEXT);
        lv_obj_set_width(r->msg, lv_pct(100));
        lv_label_set_long_mode(r->msg, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
    }
    s_ev_empty = label(tab, "No events.\nDown devices, new devices, interface changes and silent exporters appear here.",
                       &lv_font_montserrat_16, COL_DIM);
}

static void update_events(void)
{
    bool open_only = lv_obj_has_state(s_ev_sw_open, LV_STATE_CHECKED);
    int shown = 0;
    for (int i = 0; i < s_d->event_count && shown < ONO_MAX_EVENTS; i++) {
        const ono_event_t *e = &s_d->events[i];
        if (open_only && e->acked) continue;
        evrow_t *r = &s_ev[shown];
        s_ev_map[shown] = i;
        char ago[24];
        fmt_ago(e->ts_ms, ago, sizeof(ago));
        lv_obj_set_style_border_color(r->row, e->acked ? COL_BORDER : sev_color(e->severity), 0);
        lv_label_set_text(r->subj, e->subject[0] ? e->subject : event_label(e->kind));
        lv_label_set_text_fmt(r->meta, "%s  -  %s  -  %s%s", event_label(e->kind), e->severity, ago,
                              e->acked ? "  -  acknowledged" : "");
        lv_label_set_text(r->msg, e->message);
        lv_obj_set_style_text_color(r->subj, e->acked ? COL_DIM : COL_TEXT, 0);
        set_hidden(r->row, false);
        shown++;
    }
    for (int i = shown; i < ONO_MAX_EVENTS; i++) {
        s_ev_map[i] = -1;
        set_hidden(s_ev[i].row, true);
    }
    set_hidden(s_ev_empty, shown > 0);
    if (s_d->have_overview) lv_label_set_text_fmt(s_ev_count, "Events  -  %d open alert%s", s_d->open_alerts,
                                                  s_d->open_alerts == 1 ? "" : "s");
    else lv_label_set_text(s_ev_count, "Events");
}

/* ------------------------------------------------------------------------- */
/* Network                                                                   */
/* ------------------------------------------------------------------------- */

static void build_network(lv_obj_t *tab)
{
    tab_page(tab);

    lv_obj_t *c = card(tab, "Devices down", &s_net_down_title);
    for (int i = 0; i < ONO_MAX_DOWN; i++) {
        s_net_down_lbl[i] = label(c, "", &lv_font_montserrat_20, COL_BAD);
        lv_obj_add_flag(s_net_down_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_net_down_empty = label(c, LV_SYMBOL_OK " Every monitored device is up", &lv_font_montserrat_16, COL_OK);

    c = card(tab, "Failing service checks", &s_net_svc_title);
    for (int i = 0; i < ONO_MAX_DOWN; i++) {
        s_net_svc_lbl[i] = label(c, "", &lv_font_montserrat_16, COL_BAD);
        lv_obj_set_width(s_net_svc_lbl[i], lv_pct(100));
        lv_label_set_long_mode(s_net_svc_lbl[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_flag(s_net_svc_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_net_svc_empty = label(c, "", &lv_font_montserrat_16, COL_DIM);

    c = card(tab, "Inventory by type", NULL);
    for (int i = 0; i < ONO_MAX_KINDS; i++) s_kinds[i] = bar_row(c, kind_color(i));
    s_kinds_empty = label(c, "No devices yet. Discovery runs a few seconds after ONO starts.", &lv_font_montserrat_16,
                          COL_DIM);

    c = card(tab, "Health", NULL);
    s_net_health = label(c, "", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(s_net_health, lv_pct(100));
    lv_label_set_long_mode(s_net_health, LV_LABEL_LONG_MODE_WRAP);

    c = card(tab, "Discovery", NULL);
    s_net_disc = label(c, "", &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_width(s_net_disc, lv_pct(100));
    lv_label_set_long_mode(s_net_disc, LV_LABEL_LONG_MODE_WRAP);

    c = card(tab, "Connection", NULL);
    s_net_conn = label(c, "", &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(s_net_conn, lv_pct(100));
    lv_label_set_long_mode(s_net_conn, LV_LABEL_LONG_MODE_WRAP);
}

static void update_network(void)
{
    char buf[512], t[48];
    lv_label_set_text_fmt(s_net_down_title, "Devices down (%d)", s_d->down_total);
    for (int i = 0; i < ONO_MAX_DOWN; i++) {
        if (i < s_d->down_count) {
            if (s_d->down[i].detail[0]) lv_label_set_text_fmt(s_net_down_lbl[i], LV_SYMBOL_CLOSE "  %s   (%s)",
                                                              s_d->down[i].name, s_d->down[i].detail);
            else lv_label_set_text_fmt(s_net_down_lbl[i], LV_SYMBOL_CLOSE "  %s", s_d->down[i].name);
        }
        set_hidden(s_net_down_lbl[i], i >= s_d->down_count);
    }
    set_hidden(s_net_down_empty, s_d->down_total > 0 || !s_d->have_overview);

    lv_label_set_text_fmt(s_net_svc_title, "Failing service checks (%d of %d)", s_d->svc_down, s_d->svc_total);
    for (int i = 0; i < ONO_MAX_DOWN; i++) {
        if (i < s_d->svc_down_count) {
            if (s_d->svc_down_list[i].detail[0]) lv_label_set_text_fmt(s_net_svc_lbl[i], LV_SYMBOL_CLOSE "  %s - %s",
                                                                       s_d->svc_down_list[i].name,
                                                                       s_d->svc_down_list[i].detail);
            else lv_label_set_text_fmt(s_net_svc_lbl[i], LV_SYMBOL_CLOSE "  %s", s_d->svc_down_list[i].name);
        }
        set_hidden(s_net_svc_lbl[i], i >= s_d->svc_down_count);
    }
    lv_label_set_text(s_net_svc_empty, s_d->svc_total ? LV_SYMBOL_OK " All checks passing" : "No service checks configured");
    lv_obj_set_style_text_color(s_net_svc_empty, s_d->svc_total ? COL_OK : COL_DIM, 0);
    set_hidden(s_net_svc_empty, s_d->svc_down_count > 0 || !s_d->have_overview);

    int total = 0, max = 1;
    for (int i = 0; i < s_d->kind_count; i++) {
        total += s_d->kind_n[i];
        if (s_d->kind_n[i] > max) max = s_d->kind_n[i];
    }
    for (int i = 0; i < ONO_MAX_KINDS; i++) {
        if (i >= s_d->kind_count) { set_hidden(s_kinds[i].row, true); continue; }
        lv_label_set_text(s_kinds[i].name, kind_label(s_d->kind_name[i]));
        lv_label_set_text_fmt(s_kinds[i].val, "%d", s_d->kind_n[i]);
        lv_bar_set_value(s_kinds[i].bar, s_d->kind_n[i] * 1000 / max, LV_ANIM_OFF);
        set_hidden(s_kinds[i].row, false);
    }
    set_hidden(s_kinds_empty, s_d->kind_count > 0);
    (void)total;

    buf[0] = 0;
    size_t o = 0;
    if (s_d->bk_total) o += snprintf(buf + o, sizeof(buf) - o, "Configuration backups: %d, %d failing, %d changed in 24 h\n",
                                     s_d->bk_total, s_d->bk_failing, s_d->bk_changed_24h);
    if (s_d->comp_rules && o < sizeof(buf))
        o += snprintf(buf + o, sizeof(buf) - o, "Compliance: %d rules, %d violation%s\n", s_d->comp_rules,
                      s_d->comp_violations, s_d->comp_violations == 1 ? "" : "s");
    if (s_d->coll_total && o < sizeof(buf))
        o += snprintf(buf + o, sizeof(buf) - o, "Remote collectors: %d, %d silent, %d devices (%d down)\n", s_d->coll_total,
                      s_d->coll_silent, s_d->coll_devices, s_d->coll_down);
    if (o < sizeof(buf))
        o += snprintf(buf + o, sizeof(buf) - o, "Open alerts: %d   SNMP-managed: %d   Links: %d", s_d->open_alerts,
                      s_d->snmp_devices, s_d->links);
    lv_label_set_text(s_net_health, buf);
    bool bad = s_d->bk_failing || s_d->comp_violations || s_d->coll_silent;
    lv_obj_set_style_text_color(s_net_health, bad ? COL_WARN : COL_TEXT, 0);

    if (!s_d->have_stats) {
        lv_label_set_text(s_net_disc, "Not available (this account is limited to some sites, or /stats failed).");
    } else if (s_d->disc_running) {
        lv_label_set_text_fmt(s_net_disc, "Scanning: %s, %d of %d probed", s_d->disc_phase, s_d->disc_probed, s_d->disc_total);
    } else {
        fmt_ago(s_d->disc_last_ms, t, sizeof(t));
        if (s_d->disc_next_ms) {
            time_t nt = s_d->disc_next_ms / 1000;
            struct tm lt;
            localtime_r(&nt, &lt);
            lv_label_set_text_fmt(s_net_disc, "Idle. Last finished %s. Next scheduled run %02d:%02d.", t, lt.tm_hour, lt.tm_min);
        } else {
            lv_label_set_text_fmt(s_net_disc, "Idle. Last finished %s. Scheduled runs are off.", t);
        }
    }

    char ip[20];
    wifi_ip_str(ip, sizeof(ip));
    settings_lock();
    snprintf(buf, sizeof(buf), "Server: %s\nState: %s%s%s\nLast poll: %lu ms, every %u s\nDisplay IP: %s (%s)",
             settings_get()->server_url[0] ? settings_get()->server_url : "(not set)", ono_state_str(s_d->state),
             s_d->error[0] ? " - " : "", s_d->error, (unsigned long)s_d->poll_ms, settings_get()->poll_s, ip,
             wifi_state_str());
    settings_unlock();
    lv_label_set_text(s_net_conn, buf);
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
        if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    }
}

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_keyboard_set_textarea(s_kb, ta);
        lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *form_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_t *l = label(row, text, NULL, COL_DIM);
    lv_obj_set_width(l, 170);
    return row;
}

static lv_obj_t *form_ta(lv_obj_t *row, const char *txt, int32_t w, bool password, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, txt);
    lv_textarea_set_password_mode(ta, password);
    if (placeholder) lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_width(ta, w);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, NULL);
    return ta;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    label(b, txt, NULL, COL_TEXT);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static int step_index(const uint16_t *steps, int n, uint16_t v)
{
    for (int i = 0; i < n; i++) if (steps[i] >= v) return i;
    return n - 1;
}

static void scan_cb(lv_event_t *e)
{
    wifi_scan_start();
    s_scan_pending = true;
    lv_label_set_text(lv_obj_get_child(s_btn_scan, 0), "Scanning...");
}

static void scan_pick_cb(lv_event_t *e)
{
    char buf[48];
    lv_dropdown_get_selected_str(s_dd_scan, buf, sizeof(buf));
    char *sp = strstr(buf, "  (");
    if (sp) *sp = 0;
    if (buf[0] && strcmp(buf, "Networks") != 0) lv_textarea_set_text(s_ta_ssid, buf);
}

static void poll_scan(void)
{
    if (!s_scan_pending) return;
    wifi_ap_info_t aps[WIFI_SCAN_MAX];
    int n = wifi_scan_results(aps, WIFI_SCAN_MAX);
    if (n < 0) return;
    s_scan_pending = false;
    lv_label_set_text(lv_obj_get_child(s_btn_scan, 0), LV_SYMBOL_REFRESH " Scan");
    static char opts[WIFI_SCAN_MAX * 48];
    strcpy(opts, "Networks");
    for (int i = 0; i < n; i++) {
        char line[80];
        snprintf(line, sizeof(line), "\n%.32s  (%d dBm%s)", aps[i].ssid, aps[i].rssi, aps[i].secure ? "" : ", open");
        strlcat(opts, line, sizeof(opts));
    }
    lv_dropdown_set_options(s_dd_scan, opts);
    if (n) lv_dropdown_open(s_dd_scan);
}

static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_sl_bright);
    settings_get()->brightness = (uint8_t)v;
    board_set_backlight((uint8_t)v);
}

static void vcom_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_sl_vcom);
    lv_label_set_text_fmt(s_lbl_vcom, "0x%02X", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {   /* each change is a few DSI commands */
        settings_get()->vcom = (uint8_t)v;
        board_set_vcom((uint8_t)v);
    }
}

static void flip_cb(lv_event_t *e)
{
    bool f = lv_obj_has_state(s_sw_flip, LV_STATE_CHECKED);
    settings_get()->flip = f;
    board_set_flip(f);
}

static void tz_cb(lv_event_t *e)
{
    settings_get()->tz = (uint8_t)lv_dropdown_get_selected(s_dd_tz);
    settings_apply_tz();
}

static void saver_cb(lv_event_t *e)
{
    app_settings_t *c = settings_get();
    c->saver = lv_obj_has_state(s_sw_saver, LV_STATE_CHECKED);
    c->saver_min = s_saver_steps[lv_dropdown_get_selected(s_dd_saver)];
    lv_display_trigger_activity(NULL);
}

static void save_cb(lv_event_t *e)
{
    app_settings_t *c = settings_get();
    bool wifi_changed = strcmp(c->wifi_ssid, lv_textarea_get_text(s_ta_ssid)) != 0 ||
                        strcmp(c->wifi_pass, lv_textarea_get_text(s_ta_pass)) != 0;
    settings_lock();
    strlcpy(c->wifi_ssid, lv_textarea_get_text(s_ta_ssid), sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, lv_textarea_get_text(s_ta_pass), sizeof(c->wifi_pass));
    strlcpy(c->server_url, lv_textarea_get_text(s_ta_url), sizeof(c->server_url));
    strlcpy(c->token, lv_textarea_get_text(s_ta_token), sizeof(c->token));
    c->tls_verify = lv_obj_has_state(s_sw_tls, LV_STATE_CHECKED);
    c->poll_s = s_poll_steps[lv_dropdown_get_selected(s_dd_poll)];
    c->wake_on_alert = lv_obj_has_state(s_sw_wake, LV_STATE_CHECKED);
    c->saver = lv_obj_has_state(s_sw_saver, LV_STATE_CHECKED);
    c->saver_min = s_saver_steps[lv_dropdown_get_selected(s_dd_saver)];
    settings_unlock();
    settings_save();
    if (wifi_changed) wifi_apply_settings();
    ono_client_refresh();
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_set_status, LV_SYMBOL_OK " Saved");
}

static void arm_cb(lv_event_t *e)
{
    ota_arm();
}

static lv_obj_t *s_confirm;
static int s_confirm_action;    /* 1 = revert, 2 = C6 update */

static void confirm_yes_cb(lv_event_t *e)
{
    if (s_confirm) { lv_msgbox_close(s_confirm); s_confirm = NULL; }
    if (s_confirm_action == 1) ota_revert();
    else if (s_confirm_action == 2) ota_c6_update_start();
}

static void confirm_no_cb(lv_event_t *e)
{
    if (s_confirm) { lv_msgbox_close(s_confirm); s_confirm = NULL; }
}

static void confirm(int action, const char *title, const char *text)
{
    if (s_confirm) lv_msgbox_close(s_confirm);
    s_confirm_action = action;
    s_confirm = lv_msgbox_create(NULL);
    lv_obj_set_width(s_confirm, SCR_W - 120);
    lv_msgbox_add_title(s_confirm, title);
    lv_msgbox_add_text(s_confirm, text);
    lv_obj_add_event_cb(lv_msgbox_add_footer_button(s_confirm, "Yes"), confirm_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_msgbox_add_footer_button(s_confirm, "Cancel"), confirm_no_cb, LV_EVENT_CLICKED, NULL);
}

static void revert_cb(lv_event_t *e)
{
    confirm(1, "Revert firmware", "Restart into the previous firmware?");
}

static void c6_cb(lv_event_t *e)
{
    confirm(2, "Update Wi-Fi chip",
            "Send the bundled ESP-Hosted firmware to the ESP32-C6? Takes about a minute, then the display restarts. "
            "Keep the power connected.");
}

static void update_fw_section(void)
{
    ota_status_t st;
    ota_get_status(&st);
    char ip[20], buf[320];
    wifi_ip_str(ip, sizeof(ip));
    size_t o = snprintf(buf, sizeof(buf), "Firmware %s (%s, %s)%s", st.version, st.partition, st.build_date,
                        st.pending_verify ? " - pending verification" : "");
    if (st.armed_left_s && o < sizeof(buf))
        o += snprintf(buf + o, sizeof(buf) - o, "\nUpload open for %lu s: http://%s/", (unsigned long)st.armed_left_s, ip);
    if (st.message[0] && o < sizeof(buf)) o += snprintf(buf + o, sizeof(buf) - o, "\n%s", st.message);
    lv_label_set_text(s_fw_lbl, buf);
    if (st.state == OTA_RECEIVING && st.total) {
        set_hidden(s_fw_bar, false);
        lv_bar_set_value(s_fw_bar, (int32_t)((uint64_t)st.received * 100 / st.total), LV_ANIM_OFF);
    } else if (st.c6_busy && st.c6_total) {
        set_hidden(s_fw_bar, false);
        lv_bar_set_value(s_fw_bar, (int32_t)((uint64_t)st.c6_sent * 100 / st.c6_total), LV_ANIM_OFF);
    } else {
        set_hidden(s_fw_bar, true);
    }
    if (st.can_revert) lv_obj_remove_state(s_btn_revert, LV_STATE_DISABLED);
    else lv_obj_add_state(s_btn_revert, LV_STATE_DISABLED);
    if (st.c6_update_available && !st.c6_busy) lv_obj_remove_state(s_btn_c6, LV_STATE_DISABLED);
    else lv_obj_add_state(s_btn_c6, LV_STATE_DISABLED);
    lv_label_set_text_fmt(s_c6_lbl, "Wi-Fi chip: %s, bundled %s%s%s", st.c6_version_known ? st.c6_version : "?",
                          st.c6_bundled, st.c6_message[0] ? "\n" : "", st.c6_message);
}

static void build_settings(lv_obj_t *tab)
{
    tab_page(tab);
    lv_obj_set_style_pad_bottom(tab, 460, 0);   /* room to scroll fields above the keyboard */
    const app_settings_t *c = settings_get();
    lv_obj_t *r, *k;

    k = card(tab, "Wi-Fi", NULL);
    r = form_row(k, "Network");
    s_btn_scan = button(r, LV_SYMBOL_REFRESH " Scan", scan_cb);
    s_dd_scan = lv_dropdown_create(r);
    lv_dropdown_set_options(s_dd_scan, "Networks");
    lv_obj_set_flex_grow(s_dd_scan, 1);
    lv_obj_add_event_cb(s_dd_scan, scan_pick_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = form_row(k, "SSID");
    s_ta_ssid = form_ta(r, c->wifi_ssid, 540, false, NULL);
    r = form_row(k, "Password");
    s_ta_pass = form_ta(r, c->wifi_pass, 540, true, NULL);

    k = card(tab, "ONO server", NULL);
    r = form_row(k, "URL");
    s_ta_url = form_ta(r, c->server_url, 540, false, "http://192.168.1.1:8787");
    r = form_row(k, "API token");
    s_ta_token = form_ta(r, c->token, 540, true, "Bearer token (optional)");
    r = form_row(k, "Verify HTTPS");
    s_sw_tls = lv_switch_create(r);
    if (c->tls_verify) lv_obj_add_state(s_sw_tls, LV_STATE_CHECKED);
    label(r, "off for self-signed certificates", &lv_font_montserrat_14, COL_DIM);
    r = form_row(k, "Refresh");
    s_dd_poll = lv_dropdown_create(r);
    lv_dropdown_set_options(s_dd_poll, "every 5 s\nevery 10 s\nevery 15 s\nevery 30 s\nevery 1 min\nevery 2 min\nevery 5 min");
    lv_dropdown_set_selected(s_dd_poll, step_index(s_poll_steps, POLL_STEPS, c->poll_s));
    lv_obj_set_width(s_dd_poll, 260);
    r = form_row(k, "Wake on alert");
    s_sw_wake = lv_switch_create(r);
    if (c->wake_on_alert) lv_obj_add_state(s_sw_wake, LV_STATE_CHECKED);
    label(r, "screen on when a device or check fails", &lv_font_montserrat_14, COL_DIM);

    r = form_row(tab, "");
    lv_obj_set_width(lv_obj_get_child(r, 0), 0);
    lv_obj_t *save = button(r, LV_SYMBOL_SAVE "  Save and apply", save_cb);
    lv_obj_set_style_bg_color(save, COL_ACCENT, 0);
    s_set_status = label(r, "", NULL, COL_OK);

    k = card(tab, "Display", NULL);
    r = form_row(k, "Brightness");
    s_sl_bright = lv_slider_create(r);
    lv_slider_set_range(s_sl_bright, 5, 100);
    lv_slider_set_value(s_sl_bright, c->brightness, LV_ANIM_OFF);
    lv_obj_set_width(s_sl_bright, 480);
    lv_obj_add_event_cb(s_sl_bright, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = form_row(k, "Panel VCOM");
    s_sl_vcom = lv_slider_create(r);
    lv_slider_set_range(s_sl_vcom, BOARD_VCOM_MIN, BOARD_VCOM_MAX);
    lv_slider_set_value(s_sl_vcom, c->vcom, LV_ANIM_OFF);
    lv_obj_set_width(s_sl_vcom, 400);
    lv_obj_add_event_cb(s_sl_vcom, vcom_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_sl_vcom, vcom_cb, LV_EVENT_RELEASED, NULL);
    s_lbl_vcom = label(r, "", NULL, COL_DIM);
    lv_label_set_text_fmt(s_lbl_vcom, "0x%02X", c->vcom);
    r = form_row(k, "Rotate 180");
    s_sw_flip = lv_switch_create(r);
    if (c->flip) lv_obj_add_state(s_sw_flip, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_flip, flip_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = form_row(k, "Screen saver");
    s_sw_saver = lv_switch_create(r);
    if (c->saver) lv_obj_add_state(s_sw_saver, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_sw_saver, saver_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_dd_saver = lv_dropdown_create(r);
    lv_dropdown_set_options(s_dd_saver, "after 1 min\nafter 5 min\nafter 10 min\nafter 15 min\nafter 30 min\n"
                                        "after 1 hour\nafter 2 hours");
    lv_dropdown_set_selected(s_dd_saver, step_index(s_saver_steps, SAVER_STEPS, c->saver_min));
    lv_obj_set_width(s_dd_saver, 260);
    lv_obj_add_event_cb(s_dd_saver, saver_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = form_row(k, "Time zone");
    s_dd_tz = lv_dropdown_create(r);
    lv_dropdown_set_options(s_dd_tz, tz_dropdown_options());
    lv_dropdown_set_selected(s_dd_tz, c->tz);
    lv_obj_set_width(s_dd_tz, 400);
    lv_obj_add_event_cb(s_dd_tz, tz_cb, LV_EVENT_VALUE_CHANGED, NULL);

    k = card(tab, "Firmware", NULL);
    s_fw_lbl = label(k, "", &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_width(s_fw_lbl, lv_pct(100));
    lv_label_set_long_mode(s_fw_lbl, LV_LABEL_LONG_MODE_WRAP);
    s_fw_bar = lv_bar_create(k);
    lv_obj_set_size(s_fw_bar, lv_pct(100), 12);
    lv_obj_add_flag(s_fw_bar, LV_OBJ_FLAG_HIDDEN);
    r = form_row(k, "");
    lv_obj_set_width(lv_obj_get_child(r, 0), 0);
    s_btn_arm = button(r, LV_SYMBOL_UPLOAD "  Allow upload (10 min)", arm_cb);
    s_btn_revert = button(r, LV_SYMBOL_LOOP "  Previous version", revert_cb);
    r = form_row(k, "");
    lv_obj_set_width(lv_obj_get_child(r, 0), 0);
    s_btn_c6 = button(r, LV_SYMBOL_WIFI "  Update Wi-Fi chip", c6_cb);
    s_c6_lbl = label(r, "", &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_flex_grow(s_c6_lbl, 1);
    lv_label_set_long_mode(s_c6_lbl, LV_LABEL_LONG_MODE_WRAP);
    update_fw_section();

    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_kb, SCR_W, 440);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_ALL, NULL);
}

/* ------------------------------------------------------------------------- */
/* Periodic refresh                                                          */
/* ------------------------------------------------------------------------- */

static void screen_saver_tick(void)
{
    const app_settings_t *c = settings_get();
    if (!c->saver || board_screen_is_off()) return;
    if (lv_display_get_inactive_time(NULL) >= (uint32_t)c->saver_min * 60000u) board_screen_off();
}

static void wake_on_alert(void)
{
    if (!s_d->have_overview || s_d->state != ONO_ST_OK) return;
    bool worse = (s_prev_down >= 0 && s_d->down_total > s_prev_down) ||
                 (s_prev_svc_down >= 0 && s_d->svc_down > s_prev_svc_down);
    s_prev_down = s_d->down_total;
    s_prev_svc_down = s_d->svc_down;
    if (worse && settings_get()->wake_on_alert) {
        ESP_LOGI(TAG, "alert: waking screen");
        if (board_screen_is_off()) board_screen_wake();
        else lv_display_trigger_activity(NULL);
        lv_tabview_set_active(s_tabview, TAB_OVERVIEW, LV_ANIM_OFF);
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    s_tick++;
    ono_client_get(s_d);
    poll_scan();
    screen_saver_tick();
    update_topbar();

    bool changed = s_d->gen != s_shown_gen || s_d->state != s_shown_state;
    if (changed) {
        wake_on_alert();
        s_shown_gen = s_d->gen;
        s_shown_state = s_d->state;
    }
    if (board_screen_is_off() && !changed) return;

    uint32_t active = lv_tabview_get_tab_active(s_tabview);
    /* "x ago" texts age, so redraw the visible tab now and then even without new data */
    bool periodic = (s_tick % 10) == 0;
    if (changed || periodic) {
        update_hero();
        update_tiles();
        if (changed) {
            update_chart();
            update_top_list(s_talk, s_d->talkers, s_d->talker_count, false, s_talk_empty);
            update_top_list(s_ports, s_d->ports, s_d->port_count, true, s_ports_empty);
        }
        if (active == TAB_EVENTS || changed) update_events();
        if (active == TAB_NETWORK || changed) update_network();
    }
    if (active == TAB_SETTINGS) update_fw_section();
}

void ui_init(lv_display_t *disp)
{
    s_d = heap_caps_calloc(1, sizeof(ono_data_t), MALLOC_CAP_SPIRAM);
    configASSERT(s_d);
    for (int i = 0; i < ONO_MAX_EVENTS; i++) s_ev_map[i] = -1;

    lv_theme_t *th = lv_theme_default_init(disp, COL_ACCENT, COL_WARN, true, &lv_font_montserrat_16);
    lv_display_set_theme(disp, th);
    board_set_backlight(settings_get()->brightness);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_topbar(scr);

    s_tabview = lv_tabview_create(scr);
    lv_obj_set_size(s_tabview, SCR_W, SCR_H - TOPBAR_H);
    lv_obj_set_pos(s_tabview, 0, TOPBAR_H);
    lv_tabview_set_tab_bar_position(s_tabview, LV_DIR_BOTTOM);
    lv_tabview_set_tab_bar_size(s_tabview, TABBAR_H);
    lv_obj_set_style_bg_color(s_tabview, COL_BG, 0);
    /* no swiping between tabs: it fights with scrolling long lists */
    lv_obj_remove_flag(lv_tabview_get_content(s_tabview), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tb = lv_tabview_get_tab_bar(s_tabview);
    lv_obj_set_style_bg_color(tb, COL_CARD, 0);
    lv_obj_set_style_text_font(tb, &lv_font_montserrat_20, 0);

    build_overview(lv_tabview_add_tab(s_tabview, LV_SYMBOL_HOME " Overview"));
    build_events(lv_tabview_add_tab(s_tabview, LV_SYMBOL_BELL " Events"));
    build_network(lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST " Network"));
    build_settings(lv_tabview_add_tab(s_tabview, LV_SYMBOL_SETTINGS " Settings"));

    lv_timer_create(refresh_timer_cb, 1000, NULL);
    refresh_timer_cb(NULL);
    ota_report_ui_ready();
    ESP_LOGI(TAG, "UI ready");
}
