#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"

static const char *TAG = "wifi";
#define WIFI_GOT_IP   BIT0
#define CONNECT_TIMEOUT_MS 25000

static EventGroupHandle_t s_ev = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static char s_ap_ssid[24];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "sta disconnected, auto-reconnect pending");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, WIFI_GOT_IP);
    }
}

static esp_err_t wifi_core_init(void)
{
    if (s_ev) {
        return ESP_OK;
    }
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    return ESP_OK;
}

esp_err_t wifi_start_sta(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(wifi_core_init(), TAG, "core");
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_ev, WIFI_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & WIFI_GOT_IP) {
        wifi_time_start_async();
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

/* Apply new STA credentials and (re)connect. Used by the portal when the
 * user edits the WiFi while the AP is up: APSTA mode is preserved so the AP
 * - and therefore the phone's browser session - stays alive.
 * NON-BLOCKING; the portal polls wifi_sta_online() to show progress. */
esp_err_t wifi_sta_reconnect_async(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_ERROR(wifi_core_init(), TAG, "core");
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* accept WPA/WPA2/open */

    xEventGroupClearBits(s_ev, WIFI_GOT_IP);

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_disconnect();
    esp_err_t e = esp_wifi_connect();
    ESP_LOGI(TAG, "STA connect -> %s (%s)", ssid, esp_err_to_name(e));
    return e;
}

/* Block until the station has an IP (or timeout). No-op when already up. */
esp_err_t wifi_wait_ip(uint32_t timeout_ms)
{
    if (wifi_sta_online()) {
        wifi_time_start_async();
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(s_ev, WIFI_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_GOT_IP) {
        wifi_time_start_async();
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_start_portal_ap(char *ap_ssid, size_t outlen)
{
    ESP_RETURN_ON_ERROR(wifi_core_init(), TAG, "core");
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "A1MON-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    /* Keep the station link alive when it is already connected: the portal
     * needs internet access (SMS login) and the domestic DNS relay needs the
     * router's upstream server. Only switch mode when necessary, since
     * esp_wifi_set_mode() tears the STA association down. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    } else {
        bool sta_was_up = wifi_sta_online();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        if (!sta_was_up) {
            ESP_ERROR_CHECK(esp_wifi_start());
        } else {
            /* restarting WiFi drops the DHCP lease info: re-assert config */
            ESP_ERROR_CHECK(esp_wifi_start());
        }
    }

    ESP_LOGI(TAG, "config AP up: %s (portal http://192.168.4.1, sta_online=%d)",
             s_ap_ssid, (int)wifi_sta_online());
    if (ap_ssid && outlen) {
        strlcpy(ap_ssid, s_ap_ssid, outlen);
    }
    return ESP_OK;
}

int wifi_scan(char ssid[][33], int8_t *rssi, int max)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = 20;
    wifi_ap_record_t records[20];
    err = esp_wifi_scan_get_ap_records(&n, records);
    if (err != ESP_OK) {
        return 0;
    }
    /* simple selection sort by rssi desc while copying */
    bool used[20] = { false };
    int cnt = 0;
    for (int i = 0; i < max && (size_t)i < n; i++) {
        int best = -1;
        int best_rssi = -128;
        for (uint16_t j = 0; j < n && j < 20; j++) {
            if (!used[j] && records[j].rssi > best_rssi) {
                best_rssi = records[j].rssi;
                best = j;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        if (records[best].ssid[0] == 0) {
            continue; /* skip hidden */
        }
        /* skip duplicates */
        bool dup = false;
        for (int k = 0; k < cnt; k++) {
            if (!strcmp(ssid[k], (const char *)records[best].ssid)) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        strlcpy(ssid[cnt], (const char *)records[best].ssid, 33);
        rssi[cnt] = records[best].rssi;
        cnt++;
    }
    return cnt;
}

void wifi_get_ip(char *buf, size_t len)
{
    buf[0] = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
}

/* ---------- SNTP time sync (TLS needs a sane clock) ---------- */

#include "esp_sntp.h"
#include "freertos/task.h"
#include <time.h>

static bool s_sntp_started = false;
static bool s_time_ok = false;

static bool time_is_valid(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    return tm_now.tm_year + 1900 >= 2020;
}

void wifi_time_start_async(void)
{
    setenv("TZ", "CST-8", 1);        /* China Standard Time */
    tzset();
    if (s_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (async)");
}

esp_err_t wifi_sync_time(void)
{
    wifi_time_start_async();
    for (int i = 0; i < 20; i++) {          /* up to ~10 s */
        vTaskDelay(pdMS_TO_TICKS(500));
        if (time_is_valid()) {
            s_time_ok = true;
            time_t now = time(NULL);
            ESP_LOGI(TAG, "time synced: %s", ctime(&now));
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "SNTP sync timed out");
    return ESP_ERR_TIMEOUT;
}

/* ---------- helpers used by the captive portal ---------- */

uint32_t wifi_dns_server(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return 0;
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return 0;
    }
    return dns.ip.u_addr.ip4.addr;
}

bool wifi_sta_online(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}
