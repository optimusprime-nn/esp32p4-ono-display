#pragma once

/*
 * Polls the Open Network Observatory (ONO) HTTP API - the same endpoints its
 * own web dashboard uses on the Overview and Events pages:
 *
 *   GET  /api/v1/overview                            devices, down list, alerts, services, kinds...
 *   GET  /api/v1/stats                               NetFlow collector + discovery (null for site-limited accounts)
 *   GET  /api/v1/traffic/series?minutes=60           per-minute {t, bytes, packets, flows}
 *   GET  /api/v1/traffic/top?by=talkers&minutes=60&limit=8
 *   GET  /api/v1/traffic/top?by=ports&minutes=60&limit=8
 *   GET  /api/v1/events?limit=N                      {events:[...], open_alerts}
 *   POST /api/v1/events/ack  {"ids":[id]}            acknowledge from the display
 *
 * Authentication: "Authorization: Bearer <token>" when a token is set.
 * Everything runs in one task on core 1; the UI copies a snapshot.
 */

#include <stdbool.h>
#include <stdint.h>

#define ONO_MAX_DOWN    12
#define ONO_MAX_TOP     8
#define ONO_MAX_KINDS   12
#define ONO_MAX_EVENTS  40
#define ONO_SERIES_N    60      /* one point per minute, oldest first */

typedef enum {
    ONO_ST_NO_CONFIG = 0,   /* no server URL set */
    ONO_ST_WAIT_WIFI,
    ONO_ST_CONNECTING,      /* first poll not finished yet */
    ONO_ST_OK,
    ONO_ST_AUTH,            /* 401/403: token missing or wrong */
    ONO_ST_ERROR,           /* network / HTTP / JSON error */
} ono_state_t;

typedef struct {
    char name[48];
    char detail[72];
} ono_item_t;

typedef struct {
    char key[48];           /* talker: IP address */
    char label[48];         /* talker: device name, "" if unknown */
    int  port;              /* ports: port number, -1 if none */
    int  proto;             /* ports: IP protocol number, -1 if none */
    double bytes;
} ono_top_t;

typedef struct {
    int64_t id;
    int64_t ts_ms;          /* Unix ms */
    char severity[12];      /* "critical" | "warning" | "info" */
    char kind[24];          /* device_down, new_device, service_down... */
    char subject[64];
    char message[160];
    bool acked;
} ono_event_t;

typedef struct {
    ono_state_t state;
    char error[96];
    uint32_t gen;               /* bumps on every successful poll */
    int64_t updated_us;         /* esp_timer time of the last successful poll */
    uint32_t poll_ms;           /* duration of the last poll */

    /* /overview */
    bool have_overview;
    int devices, devices_up, snmp_devices, links, exporters;
    int open_alerts;
    double bytes_last_hour, flows_last_hour;
    int down_total;             /* monitored devices down */
    int down_count;             /* entries stored in down[] (<= ONO_MAX_DOWN) */
    ono_item_t down[ONO_MAX_DOWN];
    int svc_total, svc_down, svc_certs_expiring;
    int svc_down_count;
    ono_item_t svc_down_list[ONO_MAX_DOWN];
    int bk_total, bk_failing, bk_changed_24h;
    int comp_rules, comp_violations;
    int coll_total, coll_silent, coll_devices, coll_down;
    int kind_count;
    char kind_name[ONO_MAX_KINDS][20];
    int kind_n[ONO_MAX_KINDS];

    /* /stats (absent for site-limited accounts) */
    bool have_stats;
    bool nf_running;
    double nf_datagrams, nf_drops;
    char nf_error[64];
    bool disc_running;
    int disc_probed, disc_total;
    char disc_phase[24];
    int64_t disc_last_ms, disc_next_ms;

    /* /traffic/series */
    bool have_series;
    double series_bytes[ONO_SERIES_N];
    double series_flows[ONO_SERIES_N];
    int64_t series_end_ms;      /* minute of the newest point */

    /* /traffic/top */
    int talker_count;
    ono_top_t talkers[ONO_MAX_TOP];
    int port_count;
    ono_top_t ports[ONO_MAX_TOP];

    /* /events */
    bool have_events;
    int event_count;
    ono_event_t events[ONO_MAX_EVENTS];
} ono_data_t;

void ono_client_start(void);
/* Copy the latest data (thread-safe) */
void ono_client_get(ono_data_t *out);
/* Poll now instead of waiting for the interval (after a settings change) */
void ono_client_refresh(void);
/* Queue an acknowledgement; the result shows up in the next poll */
void ono_client_ack(int64_t event_id);
const char *ono_state_str(ono_state_t s);
