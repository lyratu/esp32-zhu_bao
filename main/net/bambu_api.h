#pragma once
/*
 * bambu_api.h - Bambu Lab cloud REST: login + device list.
 *   login  POST {base}/v1/user-service/user/login
 *   devices GET {base}/v1/iot-service/api/user/device   (Bearer token)
 * The accessToken is a JWT whose payload carries the user uid, which is
 * needed as MQTT username "u_{uid}" with password = accessToken.
 */
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"

/* The Bambu accessToken is a JWT and its length is the server's to choose.
 * 1024 was tight; a silently truncated token would still "log in" and then
 * fail much later at the MQTT connect with no obvious cause, so keep some
 * headroom. Both users of this struct are statics, so the extra bytes are
 * .bss, not stack. */
#define BAMBU_TOKEN_MAX 2048

typedef struct {
    char token[BAMBU_TOKEN_MAX];
    char uid[24];
    char api_base[40];
    /* Server-declared token lifetime in seconds (response "expiresIn",
     * default 3600 per the reference client). 0 when the response did not
     * carry it. Informational for now: expiry is otherwise detected via the
     * 401 on /user/bind. */
    int token_expires_in;
} bambu_auth_t;

typedef struct {
    char dev_id[24];
    char name[32];
    char model[16];
    bool online;
    char mqtt_host[64];   /* derived: mqtt_url field or region default */
    int  mqtt_port;
} bambu_dev_t;

/* cfg->account/password -> auth (token+uid+api_base). err gets a short
 * ASCII message suitable for the OLED. */
esp_err_t bambu_login(const app_cfg_t *cfg, bambu_auth_t *auth,
                      char *err, size_t errlen);

/* ----- verification-code login (used by the config portal) ----- */

/* Request a login code for an account. A phone number goes to
 * /v1/user-service/user/sendsmscode with {"phone",...}; an address that
 * contains '@' goes to /v1/user-service/user/sendemail/code with
 * {"email",...} - the split the reference client makes. err = short message. */
esp_err_t bambu_send_sms_code(const char *account, uint8_t region,
                              char *err, size_t errlen);

/* Exchange the SMS code for an access token, then fill `auth`
 * (token + uid + api_base) exactly like bambu_login does. */
esp_err_t bambu_login_with_code(const char *account, const char *code,
                                uint8_t region, bambu_auth_t *auth,
                                char *err, size_t errlen);

/* Fill auth->uid/api_base from an already-known access token.
 *
 * NOTE: this can succeed without a single network call - the uid is decoded
 * from the JWT locally - so it is NOT proof that the cloud accepts the token.
 * Pair it with bambu_verify_token() before persisting a pasted token. */
esp_err_t bambu_auth_from_token(const char *token, uint8_t region,
                                bambu_auth_t *auth);

/* Ask the cloud whether it really accepts auth->token, using a read-only
 * endpoint (GET /v1/iot-service/api/user/bind). ESP_OK means accepted, even
 * for an account with zero printers. *http_status gets the HTTP code. */
esp_err_t bambu_verify_token(const bambu_auth_t *auth, int *http_status);

/* Warm up the TLS stack once the clock is valid. The first HTTPS connection
 * after a cold boot can fail before the root-certificate bundle is ready;
 * calling this right after SNTP removes that one-off error. Idempotent,
 * safe to call from any task. */
void bambu_api_tls_warmup(void);

/* DIAGNOSTIC: start a UDP listener (port 8081) that accepts a 4-byte
 * big-endian Unix epoch and warps the system clock to it. Used to prove
 * whether a TLS "certificate not trusted" failure is a clock problem: the
 * ESP32-C3 has no RTC, so its clock comes only from SNTP and can be wrong.
 * Call once, after the network is up. */
void bambu_time_sync_service_start(void);

/* DIAGNOSTIC: run one verified handshake now and log the outcome. */
void bambu_tls_probe_once(void);

/* Fetch device list; returns count in *n. Devices sorted: online first.
 *
 * `http_status` (optional) receives the HTTP code of the call. It matters
 * because "no devices" and "the cloud rejected our token" both end up as
 * ESP_FAIL here: HTTP 200 with an empty array means the account really has no
 * printers, while 401 means the cached token is dead and the caller should
 * clear it and send the user back to the login page rather than retrying. */
esp_err_t bambu_get_devices(const bambu_auth_t *auth,
                            bambu_dev_t *devs, int max, int *n,
                            int *http_status);
