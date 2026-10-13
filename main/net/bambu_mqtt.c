#include "bambu_mqtt.h"
#include "app_state.h"
#include "esp_log.h"
#include "esp_check.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "cloud_roots.h"
#include "cJSON.h"
#include "esp_mac.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "bambu_mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static char s_topic_report[64];
static char s_topic_request[64];
static char *s_acc = NULL;      /* accumulation buffer for fragmented msgs */
static int   s_acc_len = 0;
static int   s_acc_cap = 0;
static char  s_client_id[24];

/* ---------- json helpers ---------- */

static const cJSON *jf(const cJSON *o, const char *k) { return cJSON_GetObjectItem(o, k); }

static int jint(const cJSON *o, const char *k, int def)
{
    const cJSON *f = jf(o, k);
    if (cJSON_IsNumber(f)) return (int)f->valuedouble;
    if (cJSON_IsString(f) && f->valuestring[0]) return atoi(f->valuestring);
    return def;
}

static float jfloat(const cJSON *o, const char *k, float def)
{
    const cJSON *f = jf(o, k);
    if (cJSON_IsNumber(f)) return (float)f->valuedouble;
    if (cJSON_IsString(f) && f->valuestring[0]) return strtof(f->valuestring, NULL);
    return def;
}

static void jstr(const cJSON *o, const char *k, char *out, size_t len, const char *def)
{
    const cJSON *f = jf(o, k);
    if (cJSON_IsString(f) && f->valuestring[0]) {
        strlcpy(out, f->valuestring, len);
    } else if (def) {
        strlcpy(out, def, len);
    }
}

/* ---------- report parsing ---------- */

static void handle_report(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "report json parse fail (%d bytes)", len);
        return;
    }
    const cJSON *pr = jf(root, "print");
    if (!cJSON_IsObject(pr)) {
        /* "info" / "system" messages are ignored */
        cJSON_Delete(root);
        return;
    }
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    monitor_state_t *s = &g_state;

    jstr(pr, "gcode_state", s->gcode_state, sizeof(s->gcode_state), s->gcode_state);

    int pct = jint(pr, "percent", -1);
    if (pct < 0) pct = jint(pr, "mc_percent", s->percent);
    if (pct >= 0 && pct <= 100) s->percent = pct;

    s->nozzle_temp = jfloat(pr, "nozzle_temper", s->nozzle_temp);
    s->bed_temp = jfloat(pr, "bed_temper", s->bed_temp);
    s->nozzle_target = jint(pr, "nozzle_target_temper", s->nozzle_target);
    s->bed_target = jint(pr, "bed_target_temper", s->bed_target);
    s->remain_min = jint(pr, "mc_remaining_time", s->remain_min);
    s->layer = jint(pr, "layer_num", s->layer);
    s->total_layer = jint(pr, "total_layer_num", s->total_layer);
    /* Report key is "spd_lvl" (verified against a live A1 mini payload and
     * Bambu's Fleet Hub docs; there is no "spd_level" field). Values are
     * 1=silent 2=standard 3=sport 4=ludicrous; ignore anything outside. */
    int sl = jint(pr, "spd_lvl", 0);
    if (sl >= 1 && sl <= 4) {
        s->spd_lvl = sl;
    }
    s->print_error = (uint32_t)jint(pr, "print_error", (int)s->print_error);
    int rssi = jint(pr, "wifi_signal", 0);
    if (rssi < 0) s->printer_rssi = rssi;

    /* AMS lite (optional) */
    const cJSON *ams_top = jf(pr, "ams");
    if (cJSON_IsObject(ams_top)) {
        const cJSON *units = jf(ams_top, "ams");
        if (cJSON_IsArray(units) && cJSON_GetArraySize(units) > 0) {
            const cJSON *u0 = cJSON_GetArrayItem(units, 0);
            s->has_ams = true;
            const cJSON *tn = jf(ams_top, "tray_now");
            int tray = 0;
            if (cJSON_IsString(tn)) tray = atoi(tn->valuestring);
            else if (cJSON_IsNumber(tn)) tray = (int)tn->valuedouble;
            s->tray_now = tray;
            const cJSON *trays = jf(u0, "tray");
            if (cJSON_IsArray(trays) && cJSON_GetArraySize(trays) > tray) {
                jstr(cJSON_GetArrayItem(trays, tray), "tray_type",
                     s->tray_type, sizeof(s->tray_type), s->tray_type);
            }
        }
    }

    s->got_report = true;
    s->last_report_ms = now_ms();
    xSemaphoreGive(g_state_mux);

    /* TEMP diag: every 20th report, show whether the ETA / speed fields are
     * actually present in the printer's payload (-1 = absent). Remove once
     * the fields are confirmed. */
    static int s_diag_n = 0;
    if ((s_diag_n++ % 20) == 0) {
        ESP_LOGI(TAG, "[diag] #%d gcode=%s percent=%d spd_lvl_raw=%d remain_raw=%d",
                 s_diag_n, s->gcode_state, s->percent,
                 jint(pr, "spd_lvl", -1), jint(pr, "mc_remaining_time", -1));
    }

    cJSON_Delete(root);
}

/* ---------- mqtt events ---------- */

static void acc_reset(void)
{
    free(s_acc);
    s_acc = NULL;
    s_acc_len = 0;
    s_acc_cap = 0;
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "mqtt connected");
        esp_mqtt_client_subscribe(s_client, s_topic_report, 1);
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        g_state.mqtt_up = true;
        xSemaphoreGive(g_state_mux);
        bambu_mqtt_request_status();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "mqtt disconnected");
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        g_state.mqtt_up = false;
        xSemaphoreGive(g_state_mux);
        break;
    case MQTT_EVENT_DATA: {
        if (!strstr(ev->topic, "/report")) {
            break;
        }
        if (ev->total_data_len > 0 && ev->current_data_offset + ev->data_len < ev->total_data_len) {
            /* fragmented: accumulate */
            if (ev->current_data_offset == 0) {
                if (s_acc_cap < ev->total_data_len + 1) {
                    free(s_acc);
                    s_acc_cap = ev->total_data_len + 1;
                    s_acc = malloc(s_acc_cap);
                }
                s_acc_len = 0;
            }
            if (s_acc && ev->current_data_offset == s_acc_len) {
                memcpy(s_acc + s_acc_len, ev->data, ev->data_len);
                s_acc_len += ev->data_len;
            }
            break;
        }
        const char *payload = ev->data;
        int plen = ev->data_len;
        char *heap_copy = NULL;
        if (s_acc && s_acc_len > 0) {
            /* final fragment of an accumulated message */
            if (s_acc && ev->current_data_offset == s_acc_len) {
                memcpy(s_acc + s_acc_len, ev->data, ev->data_len);
                s_acc_len += ev->data_len;
            }
            s_acc[s_acc_len] = 0;
            payload = s_acc;
            plen = s_acc_len;
        } else {
            heap_copy = malloc(plen + 1); /* cjson needs termination */
            if (heap_copy) {
                memcpy(heap_copy, ev->data, plen);
                heap_copy[plen] = 0;
                payload = heap_copy;
            }
        }
        handle_report(payload, plen);
        if (s_acc) { acc_reset(); }
        free(heap_copy);
        break;
    }
    case MQTT_EVENT_ERROR: {
        esp_mqtt_error_codes_t *e = ev->error_handle;
        ESP_LOGW(TAG, "mqtt error type=%d rc=%d esp=%d",
                 e ? e->error_type : -1,
                 e ? e->connect_return_code : -1,
                 e ? e->esp_tls_last_esp_err : -1);
        if (e && e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            e->connect_return_code != MQTT_CONNECTION_ACCEPTED) {
            xSemaphoreTake(g_state_mux, portMAX_DELAY);
            g_state.auth_failed = true;
            g_state.mqtt_up = false;
            xSemaphoreGive(g_state_mux);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------- public ---------- */

esp_err_t bambu_mqtt_start(const bambu_auth_t *auth, const bambu_dev_t *dev)
{
    bambu_mqtt_stop();

    snprintf(s_topic_report, sizeof(s_topic_report), "device/%s/report", dev->dev_id);
    snprintf(s_topic_request, sizeof(s_topic_request), "device/%s/request", dev->dev_id);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id), "a1mon-%02X%02X%02X", mac[3], mac[4], mac[5]);

    char uname[40];
    snprintf(uname, sizeof(uname), "u_%s", auth->uid);

    esp_mqtt_client_config_t cfg = { 0 };
    cfg.broker.address.hostname = dev->mqtt_host;
    cfg.broker.address.port = dev->mqtt_port;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    /* Pin the root the broker chain actually validates against (see
     * cloud_roots.h). Verified with openssl 2026-10-09: every broker host
     * (*.mqtt.bambulab.com, CN and global alike) presents
     *   leaf *.mqtt.bambulab.com -> RapidSSL TLS RSA CA G1
     *   -> DigiCert Global Root G2
     * so the GlobalSign R3 root that api.bambulab.cn uses NEVER matches
     * here - mbedtls returned -0x2700 (cert verify fail) on every connect
     * and the device sat on "connecting" forever. The ESP-IDF bundle also
     * fails to match this chain at runtime (see net/bambu_api.c). */
    cfg.broker.verification.crt_bundle_attach = NULL;
    cfg.broker.verification.certificate = DIGICERT_GLOBAL_ROOT_G2_PEM;
    cfg.broker.verification.certificate_len = sizeof(DIGICERT_GLOBAL_ROOT_G2_PEM);
    cfg.credentials.client_id = s_client_id;
    cfg.credentials.username = uname;
    cfg.credentials.authentication.password = auth->token;
    cfg.session.keepalive = 30;
    cfg.network.reconnect_timeout_ms = 5000;
    cfg.task.stack_size = 8192;
    cfg.buffer.size = 6144;
    cfg.buffer.out_size = 512;
    cfg.network.timeout_ms = 10000;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                        mqtt_event_handler, NULL), TAG, "reg event");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "start");
    ESP_LOGI(TAG, "mqtt starting: %s:%d id=%s user=%s topic=%s",
             dev->mqtt_host, dev->mqtt_port, s_client_id, uname, s_topic_report);
    return ESP_OK;
}

void bambu_mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        g_state.mqtt_up = false;
        xSemaphoreGive(g_state_mux);
        ESP_LOGI(TAG, "mqtt stopped");
    }
    acc_reset();
}

void bambu_mqtt_request_status(void)
{
    if (!s_client) return;
    static const char *payload = "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\"}}";
    esp_mqtt_client_publish(s_client, s_topic_request, payload, strlen(payload), 1, 0);
}

bool bambu_mqtt_is_up(void)
{
    return s_client != NULL;
}
