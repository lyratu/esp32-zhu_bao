/*
 * app_main.c - Bambu Lab A1 mini status monitor on ESP32-C3 + 0.98" OLED.
 *
 * Flow:
 *   NVS -> OLED -> (config valid?) -> WiFi STA -> Bambu cloud login
 *   -> device list -> cloud MQTT -> OLED monitor
 *   No config / BOOT held / WiFi failed -> config AP + captive portal.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "app_state.h"
#include "app_config.h"
#include "display/ssd1306.h"
#include "display/ui.h"
#include "net/wifi_manager.h"
#include "net/portal.h"
#include "net/bambu_api.h"
#include "net/bambu_mqtt.h"

static const char *TAG = "main";

/* -------- user-adjustable hardware config -------- */
#define OLED_I2C_PORT   0
#define OLED_SDA_GPIO   3
#define OLED_SCL_GPIO   4
#define OLED_FREQ_HZ    100000   /* 100 kHz: 400 kHz can be unreliable on
                                  * long/dupont wires with weak pull-ups */
#define OLED_SH1106     1      /* set 1 if the screen shows garbled/shifted content */
#define BTN_PORTAL_GPIO GPIO_NUM_9   /* BOOT button: hold 3 s -> config portal */

static app_cfg_t s_cfg;

/* ---------------- UI task ---------------- */

static void ui_task(void *arg)
{
    for (;;) {
        ui_render();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ---------------- BOOT button: long-press -> portal ---------------- */

static void button_task(void *arg)
{
    int held = 0;
    gpio_set_direction(BTN_PORTAL_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_PORTAL_GPIO, GPIO_PULLUP_ONLY);
    for (;;) {
        if (gpio_get_level(BTN_PORTAL_GPIO) == 0) {
            if (++held >= 60) {   /* 60 x 50 ms = 3 s */
                ESP_LOGW(TAG, "BOOT held 3s -> reboot into config portal");
                app_cfg_flag_portal_set();
                esp_restart();
            }
        } else {
            held = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------------- cloud supervision task ---------------- */

#define LOGIN_FAIL_LIMIT 2   /* consecutive failures before portal fallback */

static bambu_auth_t s_auth;
static bambu_dev_t  s_devs[8];

static void set_phase(app_phase_t ph)
{
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    g_state.phase = ph;
    xSemaphoreGive(g_state_mux);
}

/* Reboot into the config portal so the user can correct their credentials.
 * `reason` is shown on the 3rd line of the portal screen after reboot. */
static void fallback_to_portal(const char *reason)
{
    ESP_LOGW(TAG, "falling back to config portal: %s", reason);
    app_cfg_portal_reason_set(reason);
    app_cfg_login_fail_clear();
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    g_state.phase = APP_PHASE_PORTAL_WAIT;
    strlcpy(g_state.err, reason, sizeof(g_state.err));
    xSemaphoreGive(g_state_mux);
    vTaskDelay(pdMS_TO_TICKS(3000));   /* let the user read the screen */
    esp_restart();
}

/* Same, but WiFi is fine and only the Bambu account is missing/refused:
 * reboot into the online login page (STA) rather than the config AP. */
static void fallback_to_login_portal(const char *reason)
{
    ESP_LOGW(TAG, "switching to Bambu login page: %s", reason);
    app_cfg_login_page_set(reason);
    app_cfg_login_fail_clear();
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    g_state.phase = APP_PHASE_PORTAL_WAIT;
    strlcpy(g_state.err, reason, sizeof(g_state.err));
    xSemaphoreGive(g_state_mux);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static void cloud_task(void *arg)
{
    char err[28] = { 0 };
    /* how many times in a row we have replaced a rejected token with a fresh
     * password login - see the 401 branch below */
    int token_renews = 0;
    for (;;) {
        /* 1) authenticate: prefer a cached SMS-login token, fall back to
         *    account+password login. */
        set_phase(APP_PHASE_LOGIN);
        bool authed = false;
        if (s_cfg.token[0]) {
            if (bambu_auth_from_token(s_cfg.token, s_cfg.region, &s_auth) == ESP_OK) {
                authed = true;
                ESP_LOGI(TAG, "using cached access token (uid=%s)", s_auth.uid);
            } else {
                ESP_LOGW(TAG, "cached token unusable, clearing");
                app_cfg_set_token("");
                s_cfg.token[0] = 0;
            }
        }
        if (!authed) {
            if (!s_cfg.password[0] && !s_cfg.token[0]) {
                /* nothing to log in with: straight to the login page */
                fallback_to_login_portal("NO ACCOUNT");   /* does not return */
            }
            if (bambu_login(&s_cfg, &s_auth, err, sizeof(err)) != ESP_OK) {
                ESP_LOGE(TAG, "login failed: %s", err);
                xSemaphoreTake(g_state_mux, portMAX_DELAY);
                g_state.phase = APP_PHASE_ERROR;
                strlcpy(g_state.err, err, sizeof(g_state.err));
                xSemaphoreGive(g_state_mux);
                /* Accounts with SMS-code login can never be satisfied with a
                 * password, and a risk-control block is not a credential
                 * problem either: both go to the login page, which also
                 * stops the failure counter from being rewritten on every
                 * loop (NVS wear). */
                if (strstr(err, "SMS CODE") || strstr(err, "2FA") ||
                    strstr(err, "BLOCKED")) {
                    fallback_to_login_portal(err);        /* does not return */
                }
                app_cfg_login_fail_bump();
                if (app_cfg_login_fail_get() >= LOGIN_FAIL_LIMIT) {
                    fallback_to_portal("LOGIN FAIL->CONFIG");  /* no return */
                }
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            /* password login succeeded: cache the token for next boots */
            if (s_auth.token[0]) {
                app_cfg_set_token(s_auth.token);
                strlcpy(s_cfg.token, s_auth.token, sizeof(s_cfg.token));
            }
        }
        ESP_LOGI(TAG, "logged in, region base=%s", s_auth.api_base);

        /* 2) device list */
        set_phase(APP_PHASE_DEVICE);
        int n = 0;
        int dstatus = 0;
        if (bambu_get_devices(&s_auth, s_devs, 8, &n, &dstatus) != ESP_OK || n == 0) {
            ESP_LOGE(TAG, "no device on account (http %d)", dstatus);
            /* A 401 is not "this account has no printers", it is "the cloud
             * does not accept this token". With a cached token that can only
             * mean it expired or was pasted wrong, and the fix is to ask for a
             * new one - so drop the dead token and go to the login page.
             *
             * Without this branch the device instead burned its two retries
             * and fell back to the config AP, which asks for WiFi (already
             * fine) and leaves the stale token in NVS, so the next boot landed
             * in the same place. A pasted token makes that loop easy to hit. */
            if (dstatus == 401 || dstatus == 403) {
                ESP_LOGW(TAG, "cached token rejected by cloud, clearing it");
                app_cfg_set_token("");
                s_cfg.token[0] = 0;
                /* With a password on file the device can renew this by itself:
                 * loop around and the next pass takes the password branch.
                 * That is the whole reason the password is stored - the token
                 * is simply the shorter-lived of the two credentials, and
                 * sending the user to the browser for something the device can
                 * fix alone is unnecessary.
                 *
                 * The counter keeps a token that keeps being rejected (wrong
                 * region, account changed) from turning into a login loop that
                 * hammers the credential endpoint into a risk-control block. */
                if (s_cfg.password[0] && token_renews < 2) {
                    token_renews++;
                    ESP_LOGW(TAG, "renewing token with the stored password (%d/2)",
                             token_renews);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    continue;
                }
                fallback_to_login_portal("TOKEN EXPIRED");   /* does not return */
            }
            app_cfg_login_fail_bump();
            uint8_t fails = app_cfg_login_fail_get();
            xSemaphoreTake(g_state_mux, portMAX_DELAY);
            g_state.phase = APP_PHASE_ERROR;
            strlcpy(g_state.err, "NO DEVICE", sizeof(g_state.err));
            xSemaphoreGive(g_state_mux);
            if (fails >= LOGIN_FAIL_LIMIT) {
                fallback_to_portal("NO DEVICE->CONFIG");
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        /* credentials verified and a device was found: reset the counter */
        app_cfg_login_fail_clear();
        token_renews = 0;
        bambu_dev_t *dev = &s_devs[0];   /* online-first ordering */
        ESP_LOGI(TAG, "using device %s (%s) online=%d", dev->name, dev->dev_id, dev->online);
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        strlcpy(g_state.dev_name, dev->name[0] ? dev->name : dev->model,
                sizeof(g_state.dev_name));
        strlcpy(g_state.dev_model, dev->model, sizeof(g_state.dev_model));
        g_state.dev_online = dev->online;
        xSemaphoreGive(g_state_mux);

        /* 3) mqtt */
        set_phase(APP_PHASE_MQTT_CONN);
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        g_state.auth_failed = false;
        xSemaphoreGive(g_state_mux);
        if (bambu_mqtt_start(&s_auth, dev) != ESP_OK) {
            ESP_LOGE(TAG, "mqtt start failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        /* 4) supervision loop */
        uint32_t last_push = 0;
        int refresh_10min = 0;
        int dead_cycles = 0;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            bool auth_failed, up, got_report;
            uint32_t last_report;
            xSemaphoreTake(g_state_mux, portMAX_DELAY);
            auth_failed = g_state.auth_failed;
            up = g_state.mqtt_up;
            got_report = g_state.got_report;
            last_report = g_state.last_report_ms;
            xSemaphoreGive(g_state_mux);

            if (auth_failed) {
                ESP_LOGW(TAG, "broker refused credentials -> re-login");
                bambu_mqtt_stop();
                /* a stale cached token is the usual cause: drop it */
                if (s_cfg.token[0]) {
                    app_cfg_set_token("");
                    s_cfg.token[0] = 0;
                }
                break;
            }
            if (up) {
                set_phase(APP_PHASE_RUNNING);
                dead_cycles = 0;
                uint32_t t = now_ms();
                if (t - last_push >= 5000) {
                    bambu_mqtt_request_status();
                    last_push = t;
                }
                if (got_report && t - last_report > 90000) {
                    ESP_LOGW(TAG, "no report for 90s -> restart mqtt");
                    bambu_mqtt_stop();
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    break;
                }
                if (++refresh_10min >= 300) {   /* ~10 min: refresh online flag */
                    refresh_10min = 0;
                    int n2 = 0;
                    if (bambu_get_devices(&s_auth, s_devs, 8, &n2, NULL) == ESP_OK && n2 > 0) {
                        for (int i = 0; i < n2; i++) {
                            if (!strcmp(s_devs[i].dev_id, dev->dev_id)) {
                                xSemaphoreTake(g_state_mux, portMAX_DELAY);
                                g_state.dev_online = s_devs[i].online;
                                xSemaphoreGive(g_state_mux);
                                break;
                            }
                        }
                    }
                }
            } else if (++dead_cycles > 90) {    /* ~3 min without broker */
                ESP_LOGW(TAG, "mqtt dead for 3 min -> full retry");
                bambu_mqtt_stop();
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ---------------- boot paths ---------------- */

static void start_display(void)
{
    ssd1306_cfg_t oled = {
        .port = OLED_I2C_PORT,
        .sda = OLED_SDA_GPIO,
        .scl = OLED_SCL_GPIO,
        .freq_hz = OLED_FREQ_HZ,
        .addr = 0x3C,
        .sh1106 = OLED_SH1106,
    };
    esp_err_t e = ssd1306_init(&oled);
    if (e != ESP_OK) {
        oled.addr = 0x3D;
        e = ssd1306_init(&oled);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "OLED not found at 0x3C/0x3D (sda=%d scl=%d)", OLED_SDA_GPIO, OLED_SCL_GPIO);
    } else {
        /* boot self-test: light every pixel for 2 s. If this stays black
         * the panel has a power/wiring problem - firmware is fine. */
        ssd1306_fill_rect(0, 0, SSD1306_W, SSD1306_H, 1);
        ssd1306_flush();
        ESP_LOGI(TAG, "OLED self-test: full white for 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ui_init();
    xTaskCreate(ui_task, "ui", 4096, NULL, 5, NULL);
}

static void run_monitor_mode(void)
{
    wifi_get_ip(g_state.ip, sizeof(g_state.ip));
    /* TLS to the Bambu cloud needs a valid clock. SNTP was already kicked
     * off by wifi_start_sta(); wait briefly so the very first HTTPS request
     * does not hit a certificate-not-yet-valid error, then prime the cert
     * bundle so cloud_task's first call succeeds straight away. */
    set_phase(APP_PHASE_WIFI_CONN);
    wifi_sync_time();
    bambu_api_tls_warmup();
    xTaskCreate(button_task, "btn", 3072, NULL, 4, NULL);
    xTaskCreate(cloud_task, "cloud", 12288, NULL, 4, NULL);
    vTaskDelete(NULL);
}

/* ---------- phase 1: AP provisioning (WiFi only) ---------- */

static void run_portal_mode(const char *reason)
{
    char ap[24];
    wifi_start_portal_ap(ap, sizeof(ap));
    portal_start(false);           /* config-AP flavour: DNS + WiFi page */
    /* `reason` is optional: NULL for a normal first boot or a BOOT
     * long-press, a failure string only when the previous run fell back
     * here (WIFI FAIL->CONFIG and friends). The portal screen shows it as
     * a wrapped line above the AP name; it never replaces those lines. */
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    strlcpy(g_state.ap_ssid, ap, sizeof(g_state.ap_ssid));
    strlcpy(g_state.ip, "192.168.4.1", sizeof(g_state.ip));
    if (reason && reason[0]) {
        strlcpy(g_state.err, reason, sizeof(g_state.err));
    }
    g_state.phase = APP_PHASE_PORTAL;
    xSemaphoreGive(g_state_mux);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* /save handler reboots the chip */
    }
}

/* ---------- phase 2: online login portal (STA) ---------- */

/* WiFi is up but the Bambu account still needs credentials. Serve the login
 * page on the LAN address so the phone (on the same router) can reach it;
 * the IP is on the OLED. SMS login runs on the live connection. */
static void run_login_portal_mode(void)
{
    wifi_get_ip(g_state.ip, sizeof(g_state.ip));
    wifi_sync_time();              /* TLS needs a valid clock */
    bambu_time_sync_service_start();  /* DIAG: allow clock injection over UDP */
    bambu_api_tls_warmup();        /* prime the cert bundle for the page */
    portal_start(true);            /* LAN flavour: no DNS hijack */
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    g_state.phase = APP_PHASE_LOGIN_PORTAL;
    xSemaphoreGive(g_state_mux);
    ESP_LOGW(TAG, "waiting for Bambu login at http://%s/", g_state.ip);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));   /* /save handler reboots the chip */
    }
}
void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    app_state_init();
    start_display();
    ESP_LOGI(TAG, "zhubao boot");

    /* reason (if any) from a login-failure fallback in the previous run */
    char reason[28] = { 0 };
    bool had_reason = app_cfg_portal_reason_take(reason, sizeof(reason));
    if (had_reason) {
        ESP_LOGW(TAG, "portal requested after failure: %s", reason);
    }

    bool force_portal = app_cfg_flag_portal_take();
    /* app_cfg_load() now only requires an SSID - see the note in
     * app_config.c. "have_wifi" therefore means "the user has provisioned
     * this device at least once"; the Bambu credentials are optional here
     * and are collected on the login page afterwards. */
    bool have_wifi = app_cfg_load(&s_cfg);
    bool have_acct = s_cfg.account[0] && (s_cfg.password[0] || s_cfg.token[0]);

    /* A pending "login page" request (Bambu account needs SMS login) routes
     * to phase 2 even when the WiFi is perfectly fine. */
    char lreason[28] = { 0 };
    bool want_login_page = app_cfg_login_page_take(lreason, sizeof(lreason));
    if (want_login_page) {
        ESP_LOGW(TAG, "Bambu login page requested: %s", lreason);
    }

    ESP_LOGI(TAG, "boot: ssid='%s' pass=%s acct='%s' creds=%s portal_req=%d lpage=%d",
             s_cfg.ssid, s_cfg.pass[0] ? "yes" : "no", s_cfg.account,
             s_cfg.token[0] ? "token" : (s_cfg.password[0] ? "password" : "none"),
             (int)force_portal, (int)want_login_page);

    /* ---- phase 1: no WiFi credentials yet -> config AP --------------- */
    if (force_portal || !have_wifi) {
        ESP_LOGW(TAG, "entering config AP (%s)",
                 force_portal ? "BOOT long-press" : "no WiFi in NVS");
        /* Only a failure carried over from the previous boot is worth
         * putting on the screen. "First boot" and "user long-pressed BOOT"
         * are not errors - passing them as the reason used to crowd out the
         * AP name + address lines, leaving just "SETUP WIFI" on the OLED. */
        run_portal_mode(had_reason ? reason : NULL);
    }

    /* ---- connect to WiFi (shared by both remaining paths) ------------ */
    set_phase(APP_PHASE_WIFI_CONN);
    ESP_LOGI(TAG, "connecting to WiFi '%s' ...", s_cfg.ssid);
    if (wifi_start_sta(s_cfg.ssid, s_cfg.pass) != ESP_OK) {
        ESP_LOGW(TAG, "wifi connect failed -> portal");
        xSemaphoreTake(g_state_mux, portMAX_DELAY);
        g_state.phase = APP_PHASE_WIFI_FAIL;
        xSemaphoreGive(g_state_mux);
        vTaskDelay(pdMS_TO_TICKS(2000));
        run_portal_mode("WIFI FAIL->CONFIG");
    }
    ESP_LOGI(TAG, "WiFi connected");

    /* ---- phase 2a: login page requested, or no Bambu credentials ----- */
    if (want_login_page || !have_acct) {
        if (lreason[0]) {
            xSemaphoreTake(g_state_mux, portMAX_DELAY);
            strlcpy(g_state.err, lreason, sizeof(g_state.err));
            xSemaphoreGive(g_state_mux);
        }
        ESP_LOGW(TAG, "opening Bambu login portal on the LAN (%s)",
                 want_login_page ? "requested" : "no account/token yet");
        run_login_portal_mode();
    }

    /* ---- phase 2b: fully configured -> monitor ----------------------- */
    run_monitor_mode();     /* does not return */
}
