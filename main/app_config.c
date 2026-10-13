#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cfg";
#define NS "bambu-mon"

bool app_cfg_valid(const app_cfg_t *c)
{
    /* a token alone is enough to run (SMS-login account without password) */
    bool creds = c->ssid[0] != 0 && c->account[0] != 0 &&
                 (c->password[0] != 0 || c->token[0] != 0);
    return creds;
}

/* Read the stored fields verbatim. The return value is true when the NVS
 * namespace could be opened - it says NOTHING about whether the config is
 * complete. Use this when you only want the stored values (e.g. to merge
 * blank form fields); use app_cfg_valid()/app_cfg_load() to decide whether
 * the device is ready to run. */
bool app_cfg_raw_load(app_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->region = CFG_REGION_CN;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len;
    len = sizeof(c->ssid);     nvs_get_str(h, "ssid", c->ssid, &len);
    len = sizeof(c->pass);     nvs_get_str(h, "pass", c->pass, &len);
    len = sizeof(c->account);  nvs_get_str(h, "acct", c->account, &len);
    len = sizeof(c->password); nvs_get_str(h, "pwd", c->password, &len);
    len = sizeof(c->token);    nvs_get_str(h, "token", c->token, &len);
    uint8_t r = CFG_REGION_CN;
    nvs_get_u8(h, "region", &r);
    c->region = r;
    nvs_close(h);
    return true;
}

bool app_cfg_load(app_cfg_t *c)
{
    app_cfg_raw_load(c);
    /* NOTE: the return value only reports whether the SSID exists.
     *
     * It used to return app_cfg_valid(), which additionally demands the Bambu
     * account plus a token/password. A freshly provisioned device has only
     * the WiFi saved (the account is collected later on the login page), so
     * the old behaviour made app_main believe there was no configuration at
     * all and bounce straight back to the config AP - the "saved WiFi but
     * still lands on 192.168.4.1" bug. Readiness is now checked explicitly
     * where it matters (app_main / cloud_task). */
    return c->ssid[0] != 0;
}

esp_err_t app_cfg_save(const app_cfg_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "acct", c->account);
    nvs_set_str(h, "pwd", c->password);
    if (c->token[0]) {
        nvs_set_str(h, "token", c->token);
    } else {
        nvs_erase_key(h, "token");
    }
    nvs_set_u8(h, "region", c->region);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "config saved (ssid=%s region=%u token=%s)",
             c->ssid, c->region, c->token[0] ? "yes" : "no");
    return err;
}

esp_err_t app_cfg_set_token(const char *token)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (token && token[0]) {
        nvs_set_str(h, "token", token);
    } else {
        nvs_erase_key(h, "token");
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void app_cfg_flag_portal_set(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "portal", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool app_cfg_flag_portal_take(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    nvs_get_u8(h, "portal", &v);
    if (v) {
        nvs_erase_key(h, "portal");
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}

/* ---- login-failure counter ------------------------------------------
 *
 * These flags change on every failed login cycle, and a naive
 * read-modify-commit would wear the NVS page out (one erase cycle each
 * time). Two guards:
 *   1. keep the current value in RAM so reads never touch flash;
 *   2. only write when the value actually changes.
 * `s_lfail_valid` tells whether the cache was primed from NVS already. */
static uint8_t s_lfail = 0;
static bool    s_lfail_valid = false;

uint8_t app_cfg_login_fail_get(void)
{
    if (s_lfail_valid) {
        return s_lfail;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        s_lfail = 0;
        s_lfail_valid = true;
        return 0;
    }
    uint8_t v = 0;
    nvs_get_u8(h, "lfail", &v);
    nvs_close(h);
    s_lfail = v;
    s_lfail_valid = true;
    return v;
}

void app_cfg_login_fail_bump(void)
{
    uint8_t cur = app_cfg_login_fail_get();
    if (cur >= 255) {
        return;                     /* saturated: nothing to persist */
    }
    s_lfail = cur + 1;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lfail", s_lfail);
        nvs_commit(h);
        nvs_close(h);
    }
}

void app_cfg_login_fail_clear(void)
{
    if (s_lfail_valid && s_lfail == 0) {
        return;                     /* already zero: skip the flash write */
    }
    s_lfail = 0;
    s_lfail_valid = true;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lfail", 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void app_cfg_portal_reason_set(const char *reason)
{
    /* skip the flash write when the same reason is already stored */
    static char last[64];
    const char *r = reason ? reason : "";
    if (!strcmp(last, r)) {
        return;
    }
    strlcpy(last, r, sizeof(last));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "preason", r);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool app_cfg_portal_reason_take(char *out, size_t outlen)
{
    if (out && outlen) {
        out[0] = 0;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    char buf[64] = { 0 };
    size_t len = sizeof(buf);
    esp_err_t e = nvs_get_str(h, "preason", buf, &len);
    if (e == ESP_OK && buf[0]) {
        if (out && outlen) {
            strlcpy(out, buf, outlen);
        }
        nvs_erase_key(h, "preason");
        nvs_commit(h);
        nvs_close(h);
        return true;
    }
    nvs_close(h);
    return false;
}

void app_cfg_login_page_set(const char *reason)
{
    static bool wrote = false;
    static char last[64];
    const char *r = reason ? reason : "";
    /* this flag is set right before a reboot and cleared right after, so a
     * repeated identical request would otherwise rewrite flash needlessly */
    if (wrote && !strcmp(last, r)) {
        return;
    }
    wrote = true;
    strlcpy(last, r, sizeof(last));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lpage", 1);
        nvs_set_str(h, "lreason", r);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool app_cfg_login_page_take(char *out, size_t outlen)
{
    if (out && outlen) {
        out[0] = 0;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    nvs_get_u8(h, "lpage", &v);
    if (v) {
        char buf[64] = { 0 };
        size_t len = sizeof(buf);
        if (nvs_get_str(h, "lreason", buf, &len) == ESP_OK && out && outlen) {
            strlcpy(out, buf, outlen);
        }
        nvs_erase_key(h, "lpage");
        nvs_erase_key(h, "lreason");
        nvs_commit(h);
    }
    nvs_close(h);
    return v != 0;
}
