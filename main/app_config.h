#pragma once
/*
 * app_config.h - credentials stored in NVS, written via the captive portal.
 */
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#define CFG_REGION_CN     0
#define CFG_REGION_GLOBAL 1

typedef struct {
    char    ssid[33];
    char    pass[65];
    char    account[64];
    char    password[65];
    uint8_t region;         /* CFG_REGION_CN / CFG_REGION_GLOBAL */
    char    token[2048];    /* cached Bambu accessToken (SMS login); empty
                             * = must log in with account/password. Kept in
                             * step with BAMBU_TOKEN_MAX so a long JWT is
                             * never truncated between the API layer and NVS. */
} app_cfg_t;

/* true when all mandatory fields are present (WiFi + account + token/pwd) */
bool app_cfg_valid(const app_cfg_t *c);

/* Fill `c` with whatever is stored, without judging completeness. Returns
 * false only when NVS itself is unavailable. Use this when you need the raw
 * values (to merge blank form fields, or to recover the WiFi password);
 * use app_cfg_load() when you want to know whether the device can run. */
bool       app_cfg_raw_load(app_cfg_t *c);

/* Fill `c` with the stored values; returns true when a WiFi SSID is present
 * (i.e. the device has been provisioned at least once). It deliberately does
 * NOT require the Bambu account - that is collected in a later phase. */
bool       app_cfg_load(app_cfg_t *c);
esp_err_t  app_cfg_save(const app_cfg_t *c);
/* write just the cached accessToken (SMS-code login); "" clears it */
esp_err_t  app_cfg_set_token(const char *token);

/* request re-entering portal mode after reboot (BOOT button long-press) */
void app_cfg_flag_portal_set(void);
/* read-and-clear the portal request flag */
bool app_cfg_flag_portal_take(void);

/* consecutive login failures (persisted across reboots). When it reaches
 * the limit the device reboots into the config portal so the user can fix
 * their account/password. */
uint8_t app_cfg_login_fail_get(void);
void    app_cfg_login_fail_bump(void);
void    app_cfg_login_fail_clear(void);

/* err string shown on the OLED when the device fell back to portal mode
 * because of repeated login failures */
void app_cfg_portal_reason_set(const char *reason);
bool app_cfg_portal_reason_take(char *out, size_t outlen);

/* Request the online Bambu-login page (STA) after the next reboot. Used when
 * the WiFi is known but the account needs SMS-code login. */
void app_cfg_login_page_set(const char *reason);
/* read-and-clear the login-page request; `out` gets the reason (may be "") */
bool app_cfg_login_page_take(char *out, size_t outlen);
