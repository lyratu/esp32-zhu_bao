#pragma once
/*
 * wifi_manager.h - WiFi STA connection + APSTA config-mode AP + scan.
 */
#include <stdbool.h>
#include <esp_err.h>

/* Blocking: connect in STA mode. Returns ESP_OK once the station got an IP,
 * ESP_ERR_TIMEOUT after ~25 s (auto reconnect keeps running in background). */
esp_err_t wifi_start_sta(const char *ssid, const char *pass);

/* Switch to APSTA and start the config AP; fills ap_ssid (e.g. "A1MON-XXXX"). */
esp_err_t wifi_start_portal_ap(char *ap_ssid, size_t outlen);

/* Scan (works in APSTA mode). Writes up to max entries sorted by RSSI.
 * Returns number of entries written. */
int wifi_scan(char ssid[][33], int8_t *rssi, int max);

/* Copy current station IP to buf ("" when unavailable). */
void wifi_get_ip(char *buf, size_t len);

/* Sync the system clock via SNTP (needed for TLS certificate validation).
 * Blocking, up to ~10 s. Returns ESP_OK once time is set. */
esp_err_t wifi_sync_time(void);

/* Kick off SNTP in the background without waiting (safe to call as soon as
 * an IP is available). Idempotent. */
void wifi_time_start_async(void);

/* Upstream DNS server handed out by the router (network byte order);
 * 0 when unknown / station not connected. The captive-portal DNS relay
 * forwards non-hijacked queries there. */
uint32_t wifi_dns_server(void);

/* Tell whether the station currently holds an IP (portal mode uses this to
 * decide if cloud requests can go out). */
bool wifi_sta_online(void);

/* Re-apply STA credentials and reconnect in the background, keeping the AP
 * alive (APSTA). Used by the portal when the user edits the WiFi. */
esp_err_t wifi_sta_reconnect_async(const char *ssid, const char *pass);

/* Block until the station has an IP (or timeout). No-op when already up. */
esp_err_t wifi_wait_ip(uint32_t timeout_ms);
