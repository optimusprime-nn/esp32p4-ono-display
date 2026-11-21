#include "ono_client.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "settings.h"
#include "wifi.h"

static const char *TAG = "ono";

#define BODY_MAX       (512 * 1024)
#define HTTP_TIMEOUT   8000
#define EVENTS_LIMIT   ONO_MAX_EVENTS

static ono_data_t *s_data;      /* shared, behind s_mx */
static ono_data_t *s_work;      /* client task only */
static SemaphoreHandle_t s_mx;
static TaskHandle_t s_task;
static QueueHandle_t s_ack_q;

typedef struct {
    char *buf;
    size_t len, cap;
    bool overflow;
} body_t;

const char *ono_state_str(ono_state_t s)
{
    switch (s) {
    case ONO_ST_NO_CONFIG:  return "No server set";
    case ONO_ST_WAIT_WIFI:  return "Waiting for Wi-Fi";
    case ONO_ST_CONNECTING: return "Connecting";
    case ONO_ST_OK:         return "Connected";
    case ONO_ST_AUTH:       return "Sign-in refused";
    case ONO_ST_ERROR:      return "Error";
    }
    return "?";
}

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------- */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !b || evt->data_len <= 0) return ESP_OK;
    if (b->len + evt->data_len + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 16384;
        while (ncap < b->len + evt->data_len + 1) ncap *= 2;
        if (ncap > BODY_MAX) {
            b->overflow = true;
            return ESP_OK;
        }
        char *n = heap_caps_realloc(b->buf, ncap, MALLOC_CAP_SPIRAM);
        if (!n) {
            b->overflow = true;
            return ESP_OK;
        }
        b->buf = n;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    b->buf[b->len] = 0;
    return ESP_OK;
}

typedef struct {
    char base[160];             /* normalised, ends without '/' */
    char auth[144];             /* "Bearer ..." or "" */
    bool tls_verify;
    uint16_t poll_s;
} conn_cfg_t;

static void normalise_url(const char *in, char *out, size_t n)
{
    while (*in && isspace((unsigned char)*in)) in++;
    if (!strstr(in, "://")) {
        snprintf(out, n, "http://%s", in);
    } else {
        strlcpy(out, in, n);
    }
    size_t l = strlen(out);
    while (l && (out[l - 1] == '/' || isspace((unsigned char)out[l - 1]))) out[--l] = 0;
    /* tolerate a pasted ".../api/v1" */
    if (l >= 7 && strcmp(out + l - 7, "/api/v1") == 0) out[l - 7] = 0;
}

static esp_http_client_handle_t http_open(const conn_cfg_t *cc, body_t *body)
{
    char url[224];
    snprintf(url, sizeof(url), "%s/api/v1/health", cc->base);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT,
        .event_handler = http_event,
        .user_data = body,
        .keep_alive_enable = true,
        .disable_auto_redirect = true,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "ono-display",
        .crt_bundle_attach = cc->tls_verify ? esp_crt_bundle_attach : NULL,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && cc->auth[0]) esp_http_client_set_header(c, "Authorization", cc->auth);
    return c;
}

/* Returns HTTP status, or -1 on a transport error (err filled in) */
static int http_request(esp_http_client_handle_t c, const conn_cfg_t *cc, body_t *body, const char *path,
                        const char *post, char *err, size_t errlen)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/api/v1%s", cc->base, path);
    body->len = 0;
    body->overflow = false;
    if (body->buf) body->buf[0] = 0;
    esp_http_client_set_url(c, url);
    if (post) {
        esp_http_client_set_method(c, HTTP_METHOD_POST);
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, post, strlen(post));
    } else {
        esp_http_client_set_method(c, HTTP_METHOD_GET);
        esp_http_client_set_post_field(c, NULL, 0);
    }
    esp_err_t e = esp_http_client_perform(c);
    if (e != ESP_OK) {
        snprintf(err, errlen, "%s: %s", path, esp_err_to_name(e));
        return -1;
    }
    int status = esp_http_client_get_status_code(c);
    if (body->overflow) {
        snprintf(err, errlen, "%s: response too large", path);
        return -1;
    }
    return status;
}

/* GET + parse. Returns NULL on failure with *status / err filled in. */
static cJSON *get_json(esp_http_client_handle_t c, const conn_cfg_t *cc, body_t *body, const char *path, int *status,
                       char *err, size_t errlen)
{
    *status = http_request(c, cc, body, path, NULL, err, errlen);
    if (*status < 0) return NULL;
    if (*status != 200) {
        if (*status >= 300 && *status < 400) {
            snprintf(err, errlen, "HTTP %d (redirect): try https:// in the URL", *status);
        } else {
            /* ONO returns {"error": "..."} on failures */
            cJSON *j = body->len ? cJSON_Parse(body->buf) : NULL;
            const cJSON *m = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
            if (cJSON_IsString(m)) snprintf(err, errlen, "HTTP %d: %.80s", *status, m->valuestring);
            else snprintf(err, errlen, "HTTP %d on %s", *status, path);
            cJSON_Delete(j);
        }
        return NULL;
    }
    cJSON *j = cJSON_Parse(body->len ? body->buf : "");
    if (!j) snprintf(err, errlen, "%s: not JSON (is this an ONO server?)", path);
    return j;
}

/* ------------------------------------------------------------------------- */
/* JSON helpers                                                              */
/* ------------------------------------------------------------------------- */

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    return 0;
}

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : def;
}

static bool jbool(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0);
}

static void jstr(const cJSON *o, const char *k, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) {
        strlcpy(out, v->valuestring, n);
    } else if (cJSON_IsNumber(v)) {
        snprintf(out, n, "%.0f", v->valuedouble);
    } else {
        out[0] = 0;
    }
}

static const cJSON *jobj(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsObject(v) ? v : NULL;
}

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------------- */
/* Parsers                                                                   */
/* ------------------------------------------------------------------------- */

static void parse_items(const cJSON *arr, ono_item_t *out, int max, int *count, const char *detail_key)
{
    *count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (*count >= max) break;
        ono_item_t *d = &out[(*count)++];
        jstr(it, "name", d->name, sizeof(d->name));
        if (!d->name[0]) jstr(it, "ip", d->name, sizeof(d->name));
        if (!d->name[0]) jstr(it, "id", d->name, sizeof(d->name));
        d->detail[0] = 0;
        if (detail_key) jstr(it, detail_key, d->detail, sizeof(d->detail));
        if (!d->detail[0]) jstr(it, "ip", d->detail, sizeof(d->detail));
        if (!strcmp(d->detail, d->name)) d->detail[0] = 0;
    }
}

static void parse_overview(ono_data_t *d, const cJSON *o)
{
    d->have_overview = true;
    d->devices = jint(o, "devices", 0);
    d->devices_up = jint(o, "devices_up", 0);
    d->snmp_devices = jint(o, "snmp_devices", 0);
    d->links = jint(o, "links", 0);
    d->exporters = jint(o, "exporters", 0);
    d->open_alerts = jint(o, "open_alerts", 0);
    d->bytes_last_hour = jnum(o, "bytes_last_hour");
    d->flows_last_hour = jnum(o, "flows_last_hour");

    const cJSON *down = cJSON_GetObjectItemCaseSensitive(o, "down");
    parse_items(down, d->down, ONO_MAX_DOWN, &d->down_count, "ip");
    /* the real total, even when the stored list is truncated */
    d->down_total = cJSON_IsArray(down) ? cJSON_GetArraySize(down) : 0;

    const cJSON *svc = jobj(o, "services");
    d->svc_total = jint(svc, "total", 0);
    d->svc_down = jint(svc, "down", 0);
    d->svc_certs_expiring = jint(svc, "certs_expiring", 0);
    parse_items(cJSON_GetObjectItemCaseSensitive(o, "services_down"), d->svc_down_list, ONO_MAX_DOWN,
                &d->svc_down_count, "detail");

    const cJSON *bk = jobj(o, "config_backups");
    d->bk_total = jint(bk, "total", 0);
    d->bk_failing = jint(bk, "failing", 0);
    d->bk_changed_24h = jint(bk, "changed_24h", 0);

    const cJSON *cp = jobj(o, "compliance");
    d->comp_rules = jint(cp, "rules", 0);
    d->comp_violations = jint(cp, "violations", 0);

    const cJSON *co = jobj(o, "collectors");
    d->coll_total = jint(co, "total", 0);
    d->coll_silent = jint(co, "silent", 0);
    d->coll_devices = jint(co, "devices", 0);
    d->coll_down = jint(co, "down", 0);

    d->kind_count = 0;
    const cJSON *k;
    cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(o, "kinds")) {
        if (d->kind_count >= ONO_MAX_KINDS) break;
        /* [kind, n] pairs */
        const cJSON *name = cJSON_GetArrayItem(k, 0), *n = cJSON_GetArrayItem(k, 1);
        if (!cJSON_IsString(name) || !cJSON_IsNumber(n)) continue;
        strlcpy(d->kind_name[d->kind_count], name->valuestring, sizeof(d->kind_name[0]));
        d->kind_n[d->kind_count] = (int)n->valuedouble;
        d->kind_count++;
    }
}

static void parse_stats(ono_data_t *d, const cJSON *s)
{
    d->have_stats = cJSON_IsObject(s);
    if (!d->have_stats) return;
    const cJSON *nf = jobj(s, "netflow");
    const cJSON *col = jobj(nf, "collector");
    d->nf_running = col != NULL;
    d->nf_datagrams = jnum(col, "datagrams_received");
    d->nf_drops = jnum(col, "queue_drops");
    jstr(nf, "error", d->nf_error, sizeof(d->nf_error));
    const cJSON *di = jobj(s, "discovery");
    d->disc_running = jbool(di, "running");
    d->disc_probed = jint(di, "probed", 0);
    d->disc_total = jint(di, "total", 0);
    jstr(di, "phase", d->disc_phase, sizeof(d->disc_phase));
    d->disc_last_ms = (int64_t)jnum(di, "last_finished_ms");
    d->disc_next_ms = (int64_t)jnum(di, "next_run_ms");
}

static void parse_series(ono_data_t *d, const cJSON *arr)
{
    memset(d->series_bytes, 0, sizeof(d->series_bytes));
    memset(d->series_flows, 0, sizeof(d->series_flows));
    d->have_series = cJSON_IsArray(arr);
    if (!d->have_series) return;
    /* Same as the web UI: per-minute buckets ending at the current minute,
       missing minutes are zero. Before SNTP has synced, anchor on the newest point. */
    int64_t end = 0;
    const cJSON *p;
    if (wifi_is_connected() && time_is_synced()) {
        end = now_ms() / 60000 * 60000;
    } else {
        cJSON_ArrayForEach(p, arr) {
            int64_t t = (int64_t)jnum(p, "t");
            if (t > end) end = t;
        }
        end = end / 60000 * 60000;
    }
    d->series_end_ms = end;
    cJSON_ArrayForEach(p, arr) {
        int64_t t = (int64_t)jnum(p, "t");
        int64_t age = (end - t) / 60000;
        if (t > end || age >= ONO_SERIES_N) continue;
        int i = ONO_SERIES_N - 1 - (int)age;
        d->series_bytes[i] += jnum(p, "bytes");
        d->series_flows[i] += jnum(p, "flows");
    }
}

static void parse_top(const cJSON *arr, ono_top_t *out, int *count)
{
    *count = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        if (*count >= ONO_MAX_TOP) break;
        ono_top_t *t = &out[(*count)++];
        jstr(r, "key", t->key, sizeof(t->key));
        jstr(r, "label", t->label, sizeof(t->label));
        t->port = jint(r, "port", -1);
        t->proto = jint(r, "protocol", -1);
        t->bytes = jnum(r, "bytes");
    }
}

static void parse_events(ono_data_t *d, const cJSON *o)
{
    d->have_events = true;
    d->event_count = 0;
    if (cJSON_GetObjectItemCaseSensitive(o, "open_alerts")) d->open_alerts = jint(o, "open_alerts", d->open_alerts);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(o, "events")) {
        if (d->event_count >= ONO_MAX_EVENTS) break;
        ono_event_t *v = &d->events[d->event_count++];
        v->id = (int64_t)jnum(e, "id");
        v->ts_ms = (int64_t)jnum(e, "ts");
        jstr(e, "severity", v->severity, sizeof(v->severity));
        jstr(e, "kind", v->kind, sizeof(v->kind));
        jstr(e, "subject", v->subject, sizeof(v->subject));
        jstr(e, "message", v->message, sizeof(v->message));
        v->acked = jbool(e, "acknowledged");
    }
}

/* ------------------------------------------------------------------------- */
/* Task                                                                      */
/* ------------------------------------------------------------------------- */

static void publish(void)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(s_data, s_work, sizeof(*s_data));
    xSemaphoreGive(s_mx);
}

static void set_state(ono_state_t st, const char *err)
{
    s_work->state = st;
    strlcpy(s_work->error, err ? err : "", sizeof(s_work->error));
    publish();
}

static void load_cfg(conn_cfg_t *cc)
{
    settings_lock();
    const app_settings_t *s = settings_get();
    normalise_url(s->server_url, cc->base, sizeof(cc->base));
    if (!s->server_url[0]) cc->base[0] = 0;
    if (s->token[0]) snprintf(cc->auth, sizeof(cc->auth), "Bearer %s", s->token);
    else cc->auth[0] = 0;
    cc->tls_verify = s->tls_verify;
    cc->poll_s = s->poll_s < 5 ? 5 : s->poll_s;
    settings_unlock();
}

static void poll_once(const conn_cfg_t *cc, body_t *body)
{
    char err[96] = "";
    int status = 0;
    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t c = http_open(cc, body);
    if (!c) {
        set_state(ONO_ST_ERROR, "HTTP client init failed");
        return;
    }

    /* acknowledgements queued from the Events tab */
    int64_t ack_id;
    while (xQueueReceive(s_ack_q, &ack_id, 0) == pdTRUE) {
        char post[48];
        snprintf(post, sizeof(post), "{\"ids\":[%lld]}", (long long)ack_id);
        int st = http_request(c, cc, body, "/events/ack", post, err, sizeof(err));
        ESP_LOGI(TAG, "ack %lld -> %d", (long long)ack_id, st);
        if (st != 200 && st > 0) ESP_LOGW(TAG, "ack refused (HTTP %d): token may be read-only", st);
    }

    cJSON *o = get_json(c, cc, body, "/overview", &status, err, sizeof(err));
    if (!o) {
        esp_http_client_cleanup(c);
        ESP_LOGW(TAG, "overview failed: %s", err);
        set_state((status == 401 || status == 403) ? ONO_ST_AUTH : ONO_ST_ERROR, err);
        return;
    }
    parse_overview(s_work, o);
    cJSON_Delete(o);

    /* The rest is best effort: a site-limited account gets 403/null for /stats */
    char e2[96];
    cJSON *j;
    if ((j = get_json(c, cc, body, "/stats", &status, e2, sizeof(e2)))) { parse_stats(s_work, j); cJSON_Delete(j); }
    else s_work->have_stats = false;
    if ((j = get_json(c, cc, body, "/traffic/series?minutes=60", &status, e2, sizeof(e2)))) {
        parse_series(s_work, j);
        cJSON_Delete(j);
    }
    if ((j = get_json(c, cc, body, "/traffic/top?by=talkers&minutes=60&limit=8", &status, e2, sizeof(e2)))) {
        parse_top(j, s_work->talkers, &s_work->talker_count);
        cJSON_Delete(j);
    }
    if ((j = get_json(c, cc, body, "/traffic/top?by=ports&minutes=60&limit=8", &status, e2, sizeof(e2)))) {
        parse_top(j, s_work->ports, &s_work->port_count);
        cJSON_Delete(j);
    }
    char ev_path[40];
    snprintf(ev_path, sizeof(ev_path), "/events?limit=%d", EVENTS_LIMIT);
    if ((j = get_json(c, cc, body, ev_path, &status, e2, sizeof(e2)))) { parse_events(s_work, j); cJSON_Delete(j); }

    esp_http_client_cleanup(c);
    s_work->gen++;
    s_work->updated_us = esp_timer_get_time();
    s_work->poll_ms = (uint32_t)((s_work->updated_us - t0) / 1000);
    set_state(ONO_ST_OK, NULL);
}

static void client_task(void *arg)
{
    body_t body = {0};
    for (;;) {
        conn_cfg_t cc;
        load_cfg(&cc);
        uint32_t wait_s = cc.poll_s;
        if (!cc.base[0]) {
            set_state(ONO_ST_NO_CONFIG, "Set the ONO server URL in Settings");
            wait_s = 60;
        } else if (!wifi_is_connected()) {
            set_state(ONO_ST_WAIT_WIFI, NULL);
            wait_s = 2;
        } else {
            if (s_work->gen == 0 && s_work->state != ONO_ST_ERROR && s_work->state != ONO_ST_AUTH) {
                set_state(ONO_ST_CONNECTING, NULL);
            }
            poll_once(&cc, &body);
            /* keep the buffer small between polls */
            if (body.cap > 65536) {
                free(body.buf);
                body = (body_t){0};
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_s * 1000));
    }
}

void ono_client_start(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_data = heap_caps_calloc(1, sizeof(ono_data_t), MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof(ono_data_t), MALLOC_CAP_SPIRAM);
    s_ack_q = xQueueCreate(8, sizeof(int64_t));
    configASSERT(s_data && s_work && s_ack_q);
    s_data->state = s_work->state = ONO_ST_CONNECTING;
    xTaskCreatePinnedToCore(client_task, "ono", 12288, NULL, 5, &s_task, 1);
}

void ono_client_get(ono_data_t *out)
{
    if (!s_mx) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    memcpy(out, s_data, sizeof(*out));
    xSemaphoreGive(s_mx);
}

void ono_client_refresh(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

void ono_client_ack(int64_t event_id)
{
    if (!s_ack_q) return;
    xQueueSend(s_ack_q, &event_id, 0);
    ono_client_refresh();
}
