#include "bambu_api.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cloud_roots.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "bambu_api";

/* Diagnostics for the "certificate not trusted" handshake failure. Flip with
 * `idf.py menuconfig`-free #define so the pairing test can prove whether the
 * failure is time-related or chain/verification-related. */
#define BAMBU_TLS_DIAG          1

#define CN_API    "https://api.bambulab.cn"
#define GLOBAL_API "https://api.bambulab.com"
#define CN_MQTT   "cn.mqtt.bambulab.com"
#define GLOBAL_MQTT "us.mqtt.bambulab.com"

/* Client fingerprint for cloud requests - PrintSphere-style slicer identity.
 *
 * History of this block, so nobody "simplifies" it back:
 *   v1: bare two headers. Wrong theory: that headers caused HTTP 418.
 *   v2: browser-like set (Origin/Referer/desktop-Chrome UA), matching
 *       printsphere-lite's companion server.
 *   v3 (now): the full OrcaSlicer X-BBL-* set used by PrintSphere
 *       (main/src/bambu_cloud_client.cpp, perform_json_request), which is a
 *       shipping firmware that talks to the same endpoints. Measured on this
 *       API (2026-10-09): the 418 risk-control mark is keyed to the outbound
 *       IP / request rate and ignores headers entirely, so the header set is
 *       about looking like a coherent client, not about evading anything.
 *       We follow the reference that demonstrably works end to end.
 *
 * Note these are identity labels, not signatures: no request signing is
 * attempted anywhere in this file. Set to 0 for the bare two-header set. */
#define BAMBU_SLICER_HEADERS 1
#define BAMBU_UA "bambu_network_agent/01.09.05.01"

/* Print what the TLS layer actually sees: local wall clock in UTC + epoch,
 * free heap (the cert bundle needs a large contiguous block), and the size of
 * the bundled root CA list. Called before each handshake attempt. */
static void tls_diag_dump(const char *where)
{
#if BAMBU_TLS_DIAG
    static int n = 0;
    if (n++ > 6) return;             /* keep the log readable */
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc);
    ESP_LOGW(TAG, "[diag %s] utc=%s epoch=%lld heap_free=%u heap_max_block=%u",
             where, buf, (long long)now,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
}


static void set_err(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) {
        strlcpy(err, msg, errlen);
    }
}

/* Cloud risk control answers HTTP 418 with a Geetest challenge:
 *   {"error":"We need to confirm that you are not a robot.", "captchaId":...}
 * It is triggered mainly by the *outbound IP* (and request rate), not by our
 * headers - so every device behind the same router sees it at once, and
 * retrying only makes the mark stickier. Callers must surface this as
 * "blocked" rather than "unreachable" so the user stops hammering the button.
 * Returns true when `status`/`body` look like a risk-control block. */
static bool is_risk_blocked(int status, const char *body)
{
    if (status == 418) {
        return true;
    }
    if (body && strstr(body, "not a robot")) {
        return true;
    }
    if (body && strstr(body, "captchaId")) {
        return true;
    }
    return false;
}

/* Log and classify a non-2xx answer. `what` is a short label for the log. */
static void set_http_err(int status, const char *body, const char *what,
                         char *err, size_t errlen)
{
    if (is_risk_blocked(status, body)) {
        set_err(err, errlen, "CLOUD BLOCKED");
        ESP_LOGE(TAG, "%s: HTTP %d - cloud risk control, retrying will NOT "
                      "help. Wait, or change the outbound IP/network. body: %.180s",
                 what, status, body ? body : "");
        return;
    }
    ESP_LOGW(TAG, "%s: HTTP %d body: %.180s", what, status, body ? body : "");
}

/* ---------- tiny https client ---------- */

typedef struct {
    char  *body;      /* malloc'd, NUL terminated */
    int    len;
    int    status;
} http_resp_t;

/* Pinned trust anchors for the Bambu cloud.
 *
 * The two regions do NOT share a CA, which is why a single pinned root used
 * to work for .cn and fail for .com:
 *
 *   api.bambulab.cn  -> GlobalSign Root CA - R3
 *        (leaf issued by "GlobalSign GCC R3 DV TLS CA 2020")
 *   api.bambulab.com -> DigiCert Global Root G2
 *        (leaf issued by "DigiCert Encryption Everywhere DV TLS CA - G2")
 *
 * The ESP-IDF certificate bundle never matches either chain at runtime on
 * this hardware, even though both verify fine on a PC (openssl verify -> OK).
 * Pinning is therefore per-host: see pinned_root_for(). */
#define BAMBU_USE_PINNED_ROOT 1

/* Pick the CA bundle that matches the host in `url`. MQTT uses the .cn broker
 * on this build, so GlobalSign is the sensible default. */
static const char *pinned_root_for(const char *url)
{
    if (url && strstr(url, "api.bambulab.com")) {
        return DIGICERT_GLOBAL_ROOT_G2_PEM;
    }
    return GLOBALSIGN_ROOT_R3_PEM;
}

/* one attempt; split out so http_request() can retry transparently */
static esp_err_t http_attempt_ex(const char *url, const char *method,
                                 const char *bearer, const char *json_body,
                                 bool insecure, http_resp_t *out)
{
    memset(out, 0, sizeof(*out));
    tls_diag_dump(insecure ? "insecure" : "secure");
    esp_http_client_config_t cfg = {
        .url = url,
        .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = insecure ? NULL : esp_crt_bundle_attach,
        .skip_cert_common_name_check = insecure,
        .keep_alive_enable = false,
        /* Force IPv4. Both api.bambulab.cn and .com publish an AAAA record
         * and this router hands out IPv6, but the device is IPv4-only
         * (CONFIG_LWIP_IPV6_AUTOCONFIG is off, so it never gets a v6
         * address). With LWIP_DNS_MAX_HOST_IP=1 lwIP keeps only the FIRST
         * DNS answer - the IPv6 one - so every request died with
         * ESP_ERR_HTTP_CONNECT while the same host worked from a PC.
         * Restricting the family here makes the client resolve A records
         * only, which is what this board can actually route. */
        .addr_type = HTTP_ADDR_TYPE_INET,
    };
    if (!insecure) {
#if BAMBU_USE_PINNED_ROOT
        /* Pin the root that actually matches this host. The regions use
         * different CAs, so picking by URL is required: pinning only the
         * GlobalSign root made every api.bambulab.com request fail with
         * mbedtls_ssl_handshake -0x2700 (CERT_VERIFY_FAILED) and surface to
         * the user as "CLOUD UNREACHABLE". */
        cfg.crt_bundle_attach = NULL;
        cfg.cert_pem = pinned_root_for(url);
        cfg.cert_len = strlen(cfg.cert_pem) + 1;
#endif
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGE(TAG, "esp_http_client_init failed for %s", url);
        return ESP_FAIL;
    }
#if BAMBU_SLICER_HEADERS
    /* PrintSphere's exact slicer identity (bambu_cloud_client.cpp). Content-
     * Type follows the reference client: only set when a body is present. */
    esp_http_client_set_header(c, "User-Agent", BAMBU_UA);
    esp_http_client_set_header(c, "X-BBL-Client-Name", "OrcaSlicer");
    esp_http_client_set_header(c, "X-BBL-Client-Type", "slicer");
    esp_http_client_set_header(c, "X-BBL-Client-Version", "01.09.05.51");
    esp_http_client_set_header(c, "X-BBL-Language", "en-US");
    esp_http_client_set_header(c, "X-BBL-OS-Type", "linux");
    esp_http_client_set_header(c, "X-BBL-OS-Version", "6.2.0");
    esp_http_client_set_header(c, "X-BBL-Agent-Version", "01.09.05.01");
    esp_http_client_set_header(c, "X-BBL-Executable-info", "{}");
    esp_http_client_set_header(c, "X-BBL-Agent-OS-Type", "linux");
    esp_http_client_set_header(c, "Accept", "application/json");
    if (json_body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
#else
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
#endif
    if (bearer) {
        static char bearer_buf[BAMBU_TOKEN_MAX + 16];
        snprintf(bearer_buf, sizeof(bearer_buf), "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", bearer_buf);
    }

    int blen = json_body ? (int)strlen(json_body) : 0;
    esp_err_t err = esp_http_client_open(c, blen);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s (heap_free=%u)",
                 url, esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
        esp_http_client_cleanup(c);
        return err;
    }
    if (blen > 0) {
        esp_http_client_write(c, json_body, blen);
    }
    int hdr_len = esp_http_client_fetch_headers(c);
    (void)hdr_len;

    int cap = 4096;
    out->body = malloc(cap);
    if (!out->body) {
        esp_http_client_cleanup(c);
        return ESP_ERR_NO_MEM;
    }
    int total = 0;
    while (1) {
        if (cap - total < 512) {
            if (cap >= 32 * 1024) { break; }  /* cap response at 32 KB */
            cap *= 2;
            char *nb = realloc(out->body, cap);
            if (!nb) { break; }
            out->body = nb;
        }
        int r = esp_http_client_read(c, out->body + total, cap - total - 1);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    out->body[total] = 0;
    out->len = total;
    out->status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    ESP_LOGD(TAG, "%s %s -> %d (%d bytes)", method, url, out->status, total);
    return ESP_OK;
}

static esp_err_t http_attempt(const char *url, const char *method,
                              const char *bearer, const char *json_body,
                              http_resp_t *out)
{
    return http_attempt_ex(url, method, bearer, json_body, false, out);
}

/* Retry wrapper: see bambu_api_tls_warmup() for why retries are needed.
 * Endpoints called only once (sendsmscode) cannot afford a single failed
 * handshake, so every request gets up to four extra attempts. */
#define HTTP_MAX_ATTEMPTS 5

/* NOTE on retrying a code login: do NOT use the generic retry wrapper for
 * bambu_login_with_code(). An SMS code is single-use and short-lived; if the
 * first request actually reached the cloud and was consumed, replaying it
 * turns "the network hiccuped" into "your code is invalid". The code-login
 * path therefore attempts exactly once and reports the real failure. */
static esp_err_t http_request_once(const char *url, const char *method,
                                   const char *bearer, const char *json_body,
                                   http_resp_t *out)
{
    return http_attempt(url, method, bearer, json_body, out);
}

static esp_err_t http_request(const char *url, const char *method,
                              const char *bearer, const char *json_body,
                              http_resp_t *out)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < HTTP_MAX_ATTEMPTS; i++) {
        err = http_attempt(url, method, bearer, json_body, out);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "attempt %d/%d for %s failed: %s",
                 i + 1, HTTP_MAX_ATTEMPTS, url, esp_err_to_name(err));
        free(out->body);
        memset(out, 0, sizeof(*out));
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    return err;
}

/* Warm up the TLS stack once after the clock is valid.
 *
 * Observed on this hardware: after SNTP the TLS handshakes through
 * esp_crt_bundle fail with
 *   esp-x509-crt-bundle: No matching trusted root certificate found
 *   mbedtls_ssl_handshake returned -0x3000
 * This loop retries a few times so a transient failure does not surface to
 * the caller. If it never converges the per-request retry wrapper handles it
 * and the /tlsprobe route can be used to re-test on demand. */
void bambu_api_tls_warmup(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    tls_diag_dump("warmup-start");

    for (int i = 0; i < 12; i++) {
        http_resp_t r;
        /* any HTTP status proves TLS came up; 404/405 is a success here */
        esp_err_t e = http_attempt(CN_API "/", "GET", NULL, NULL, &r);
        free(r.body);
        if (e == ESP_OK) {
            ESP_LOGI(TAG, "TLS warmup ok after %d attempt(s)", i + 1);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "TLS warmup did not converge - will retry per request");
}

/* DIAGNOSTIC: single verified handshake attempt with a printed outcome.
 * Exposed over HTTP (/tlsprobe) so a bench test can re-test immediately
 * after correcting the clock, without waiting for a reboot cycle. */
void bambu_tls_probe_once(void)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char s[40];
    strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &utc);

    http_resp_t r;
    esp_err_t e = http_attempt(CN_API "/", "GET", NULL, NULL, &r);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "[diag] /tlsprobe utc=%s => TLS OK, http %d (%d bytes) "
                      "*** certificate verified ***", s, r.status, r.len);
    } else {
        ESP_LOGE(TAG, "[diag] /tlsprobe utc=%s => TLS FAILED (%s)",
                 s, esp_err_to_name(e));
    }
    free(r.body);
}

/* ---------- DIAGNOSTIC: remote clock control over UDP ----------------
 *
 * The ESP32-C3 has no battery-backed RTC, so its wall clock exists only
 * after SNTP. If the clock ends up outside a server certificate's validity
 * window, every handshake fails with "No matching trusted root certificate".
 * This listener (UDP 8081) accepts `[epoch:uint32 big-endian][micros?]` and
 * warps the clock, which lets a bench test settle the question in seconds:
 *   - clock corrected -> handshakes succeed  => the SNTP clock was the bug
 *   - clock corrected -> still fails         => the fault is elsewhere
 * It is a debug aid and must be removed (or gated) for production. */

#include "lwip/sockets.h"
#include "esp_system.h"

#define TIMESVC_PORT 8081

static void time_sync_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "timesvc: socket() failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TIMESVC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "timesvc: bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "[diag] time-sync listener on UDP %d", TIMESVC_PORT);

    for (;;) {
        uint8_t buf[64];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 4) continue;

        uint32_t epoch = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                         ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
        struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
        struct timezone tz = { 0, 0 };
        settimeofday(&tv, &tz);

        time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        char s[40];
        strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &utc);
        ESP_LOGW(TAG, "[diag] clock set from %s -> utc=%s epoch=%lld",
                 inet_ntoa(from.sin_addr), s, (long long)now);

        char ack[64];
        int al = snprintf(ack, sizeof(ack), "ok epoch=%lld", (long long)now);
        sendto(sock, ack, al, 0, (struct sockaddr *)&from, fl);
    }
}

void bambu_time_sync_service_start(void)
{
    xTaskCreate(time_sync_task, "timesvc", 3072, NULL, 3, NULL);
}

/* ---------- JWT uid extraction ---------- */

static bool jwt_payload_json(const char *token, char *out, size_t outlen)
{
    const char *p1 = strchr(token, '.');
    if (!p1) return false;
    const char *p2 = strchr(p1 + 1, '.');
    if (!p2) return false;
    size_t n = p2 - (p1 + 1);
    if (n == 0 || n > 2048) return false;

    char b64[2100];
    memcpy(b64, p1 + 1, n);
    b64[n] = 0;
    /* base64url -> base64 + padding */
    for (size_t i = 0; i < n; i++) {
        if (b64[i] == '-') b64[i] = '+';
        else if (b64[i] == '_') b64[i] = '/';
    }
    while (n % 4) b64[n++] = '=';
    unsigned char bin[1600];
    size_t olen = 0;
    if (mbedtls_base64_decode(bin, sizeof(bin), &olen, (const unsigned char *)b64, n) != 0) {
        return false;
    }
    if (olen >= outlen) olen = outlen - 1;
    memcpy(out, bin, olen);
    out[olen] = 0;
    return true;
}

/* Recursively find the first non-empty string stored under any of `keys`.
 *
 * The account endpoints have been observed to nest the session token (for
 * instance one level down under a data object) and to name it differently
 * across revisions. A shallow cJSON_GetObjectItem(root, "accessToken") then
 * misses a perfectly good login and reports failure even though the cloud
 * accepted the code - which is exactly the kind of "wrong" answer that is
 * hard to tell apart from a rejected code. */
static const char *json_find_str(const cJSON *node, const char *const *keys)
{
    if (!node) return NULL;
    if (cJSON_IsObject(node)) {
        for (int i = 0; keys[i]; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(node, keys[i]);
            if (cJSON_IsString(v) && v->valuestring[0]) return v->valuestring;
        }
    }
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        const cJSON *ch;
        cJSON_ArrayForEach(ch, node) {
            const char *r = json_find_str(ch, keys);
            if (r) return r;
        }
    }
    return NULL;
}

/* Field names the cloud has used for the session token. */
static const char *const TOKEN_KEYS[] = {
    "accessToken", "access_token", "idToken", "token", NULL
};

/* Field names the cloud has used for the user id. "uidStr" observed in the
 * reference client's preference-endpoint parsing. */
static const char *const UID_KEYS[] = {
    "uid", "uidStr", "userId", "user_id", "id", NULL
};

/* Coerce a uid JSON node (string OR number) into the bare numeric id.
 *
 * The cloud is inconsistent about the type: the JWT payload carries "uid" as
 * a NUMBER and "username" as "u_<digits>", while profile endpoints have used
 * both string and numeric forms. A string-only reader therefore rejects a
 * perfectly good login with "UID PARSE FAIL" even though the token is fine -
 * exactly the failure observed on device 2026-10-09. The "u_" prefix, when
 * present, is stripped: callers prepend "u_" themselves (see bambu_mqtt.c),
 * so we store the bare id everywhere.
 *
 * Returns true when `uid` was filled with a non-empty all-digit id. */
static bool uid_node_to_str(const cJSON *f, char *uid, size_t uidlen)
{
    char buf[32];
    if (cJSON_IsString(f) && f->valuestring[0]) {
        strlcpy(buf, f->valuestring, sizeof(buf));
    } else if (cJSON_IsNumber(f) && f->valuedouble > 0) {
        snprintf(buf, sizeof(buf), "%lld", (long long)f->valuedouble);
    } else {
        return false;
    }
    char *v = buf;
    if (v[0] == 'u' && v[1] == '_' && v[2]) v += 2;
    if (!v[0]) return false;
    for (const char *p = v; *p; p++) {
        if (*p < '0' || *p > '9') return false;   /* ids are numeric */
    }
    strlcpy(uid, v, uidlen);
    return true;
}

static esp_err_t extract_uid(const char *token, char *uid, size_t uidlen)
{
    char payload[2048];
    if (!jwt_payload_json(token, payload, sizeof(payload))) {
        ESP_LOGW(TAG, "token is not a JWT");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(payload);
    if (!root) return ESP_FAIL;
    /* Claim priority as in the reference client: "username" (u_<id> form)
     * first, then the numeric/string uid fields. */
    static const char *const CLAIMS[] = { "username", "uid", "user_id", "sub", NULL };
    esp_err_t ret = ESP_FAIL;
    for (int i = 0; CLAIMS[i]; i++) {
        if (uid_node_to_str(cJSON_GetObjectItem(root, CLAIMS[i]), uid, uidlen)) {
            ret = ESP_OK;
            break;
        }
    }
    cJSON_Delete(root);
    if (ret == ESP_OK) ESP_LOGI(TAG, "uid=%s", uid);
    return ret;
}

/* Recursively find a uid under any of UID_KEYS, accepting the string and
 * numeric forms the cloud has used (see uid_node_to_str). */
static const char *json_find_uid(const cJSON *node, char *uid, size_t uidlen)
{
    if (!node) return NULL;
    if (cJSON_IsObject(node)) {
        for (int i = 0; UID_KEYS[i]; i++) {
            if (uid_node_to_str(cJSON_GetObjectItemCaseSensitive(node, UID_KEYS[i]),
                                uid, uidlen)) {
                return uid;
            }
        }
    }
    if (cJSON_IsObject(node) || cJSON_IsArray(node)) {
        const cJSON *ch;
        cJSON_ArrayForEach(ch, node) {
            const char *r = json_find_uid(ch, uid, uidlen);
            if (r) return r;
        }
    }
    return NULL;
}

/* Last-resort uid lookup over the profile endpoints.
 *
 * This is how the reference implementation resolves the MQTT username, and
 * it is the more dependable of the two routes: the uid must not depend on the
 * access token being a JWT with a `uid` claim, because the token is opaque to
 * us and its shape is the server's to change. A login that succeeds but
 * yields no uid leaves MQTT permanently unable to connect, so the fallback
 * matters. */
static bool fetch_uid_from_profile(const char *base, const char *token,
                                   char *uid, size_t uidlen)
{
    static const char *paths[] = {
        "/v1/user-service/my/profile",
        "/v1/user-service/user/info",
        "/v1/user-service/my/user",
        "/v1/design-user-service/my/preference",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char url[128];
        snprintf(url, sizeof(url), "%s%s", base, paths[i]);
        http_resp_t r;
        if (http_request(url, "GET", token, NULL, &r) != ESP_OK) {
            free(r.body);
            continue;
        }
        cJSON *root = cJSON_Parse(r.body);
        ESP_LOGD(TAG, "profile %s http %d", paths[i], r.status);
        free(r.body);
        if (!root) continue;
        const char *v = json_find_uid(root, uid, uidlen);
        bool ok = v && v[0];
        if (ok) {
            /* callers prepend "u_", so store the bare id */
            strlcpy(uid, v, uidlen);
            ESP_LOGI(TAG, "uid from %s: %s", paths[i], uid);
        }
        cJSON_Delete(root);
        if (ok) return true;
    }
    return false;
}

/* Resolve the MQTT username id: JWT claim first, profile endpoints second. */
static esp_err_t resolve_uid(const char *base, const char *token,
                             bambu_auth_t *auth)
{
    if (extract_uid(token, auth->uid, sizeof(auth->uid)) == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "no uid claim in token, asking profile endpoints");
    if (fetch_uid_from_profile(base, token, auth->uid, sizeof(auth->uid))) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ---------- login (password mode - the primary path, as in PrintSphere) ---------- */

/* Pull expiresIn (seconds) out of a login response. The field sits at the
 * root or under "data" depending on endpoint mood; the reference client
 * defaults to 3600 when absent. */
static int login_expires_in(const cJSON *root)
{
    const cJSON *e = cJSON_GetObjectItem(root, "expiresIn");
    if (!cJSON_IsNumber(e)) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            e = cJSON_GetObjectItem(data, "expiresIn");
        }
    }
    return cJSON_IsNumber(e) ? (int)e->valuedouble : 3600;
}

/* Best human-readable failure detail from a login/JSON error body. The cloud
 * has used "error", "msg" and "apiError" at the root and under "data". */
static void log_cloud_detail(const char *what, int status, const cJSON *root,
                             const char *raw_body)
{
    static const char *const DETAIL_KEYS[] = { "apiError", "error", "msg", "message", NULL };
    const char *d = root ? json_find_str(root, DETAIL_KEYS) : NULL;
    if (d && d[0]) {
        ESP_LOGW(TAG, "%s: http %d detail: %s", what, status, d);
    } else {
        ESP_LOGW(TAG, "%s: http %d body: %.200s", what, status, raw_body ? raw_body : "");
    }
}

static esp_err_t login_once(const char *base, const char *account,
                            const char *password, bambu_auth_t *auth,
                            char *err, size_t errlen)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "account", account);
    cJSON_AddStringToObject(b, "password", password);
    cJSON_AddStringToObject(b, "apiError", "");
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);

    char url[96];
    snprintf(url, sizeof(url), "%s/v1/user-service/user/login", base);
    http_resp_t resp;
    esp_err_t herr = http_request(url, "POST", NULL, body, &resp);
    free(body);
    if (herr != ESP_OK) {
        set_err(err, errlen, "CLOUD UNREACHABLE");
        return herr;
    }
    esp_err_t ret = ESP_FAIL;
    if (is_risk_blocked(resp.status, resp.body)) {
        set_http_err(resp.status, resp.body, "password login", err, errlen);
        free(resp.body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (!root) {
        set_err(err, errlen, "BAD LOGIN RESP");
        goto done;
    }
    /* Same acceptance rule as PrintSphere::authenticate_with_password(): a
     * 2xx answer carrying an accessToken is a session, regardless of any
     * "success" flag (the cloud omits that field on some endpoints). The
     * recursive token lookup covers both root and nested "data" copies. */
    const char *tok = json_find_str(root, TOKEN_KEYS);
    if (resp.status >= 200 && resp.status < 300 && tok) {
        strlcpy(auth->token, tok, sizeof(auth->token));
        strlcpy(auth->api_base, base, sizeof(auth->api_base));
        auth->token_expires_in = login_expires_in(root);
        ESP_LOGI(TAG, "password login OK http %d token_len=%u expires_in=%ds",
                 resp.status, (unsigned)strlen(auth->token), auth->token_expires_in);
        if (resolve_uid(base, auth->token, auth) != ESP_OK) {
            set_err(err, errlen, "UID PARSE FAIL");
            goto done;
        }
        ret = ESP_OK;
        goto done;
    }
    const cJSON *lt = cJSON_GetObjectItem(root, "loginType");
    if (cJSON_IsString(lt) && lt->valuestring[0]) {
        /* Server-driven mode switch, exactly as PrintSphere does: the cloud
         * tells us which credential this account will actually accept. */
        if (!strcmp(lt->valuestring, "verifyCode")) {
            /* This account only accepts verification codes: a password can
             * never yield a token. Send the user to the code-login page. */
            set_err(err, errlen, "USE SMS CODE");
        } else if (!strcmp(lt->valuestring, "tfa")) {
            set_err(err, errlen, "NEED 2FA CODE");
        } else {
            set_err(err, errlen, "LOGIN REJECTED");
        }
        goto done;
    }
    if (resp.status == 418) {
        set_err(err, errlen, "CLOUD BLOCKED");
        ESP_LOGW(TAG, "login http 418 (risk control): %.150s", resp.body);
        goto done;
    }
    if (resp.status == 401 || resp.status == 400) {
        set_err(err, errlen, "BAD ACCOUNT/PWD");
    } else {
        set_err(err, errlen, "LOGIN FAIL");
    }
    log_cloud_detail("password login", resp.status, root, resp.body);
done:
    if (root) cJSON_Delete(root);
    free(resp.body);
    return ret;
}

esp_err_t bambu_login(const app_cfg_t *cfg, bambu_auth_t *auth,
                      char *err, size_t errlen)
{
    const char *primary = (cfg->region == CFG_REGION_GLOBAL) ? GLOBAL_API : CN_API;
    const char *fallback = (cfg->region == CFG_REGION_GLOBAL) ? CN_API : GLOBAL_API;

    esp_err_t e = login_once(primary, cfg->account, cfg->password, auth, err, errlen);
    /* A code-login / 2FA / risk-control answer is a decision, not a
     * connectivity problem: retrying against the other region would only
     * burn time (and could trip rate limits). */
    bool definitive = err[0] && (strstr(err, "SMS CODE") || strstr(err, "2FA") ||
                                 strstr(err, "BLOCKED") || strstr(err, "BAD ACCOUNT") ||
                                 strstr(err, "REJECTED"));    if (e != ESP_OK && !definitive) {
        ESP_LOGW(TAG, "login via %s failed (%s), trying fallback", primary, err);
        vTaskDelay(pdMS_TO_TICKS(500));
        e = login_once(fallback, cfg->account, cfg->password, auth, err, errlen);
    }
    return e;
}

/* ---------- verification-code request (SMS for phones, e-mail otherwise) ---------- */

/* PrintSphere splits the code-request endpoint by account type
 * (request_sms_verification_code / request_email_verification_code):
 *   phone  -> POST /v1/user-service/user/sendsmscode   {"phone","type":"codeLogin"}
 *   e-mail -> POST /v1/user-service/user/sendemail/code {"email","type":"codeLogin"}
 * Both answer a plain 200 on success with a body that may be empty. */
esp_err_t bambu_send_sms_code(const char *account, uint8_t region,
                              char *err, size_t errlen)
{
    if (!account || !account[0]) {
        set_err(err, errlen, "NO ACCOUNT");
        return ESP_FAIL;
    }
    const char *base = (region == CFG_REGION_GLOBAL) ? GLOBAL_API : CN_API;
    const bool is_email = strchr(account, '@') != NULL;

    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, is_email ? "email" : "phone", account);
    cJSON_AddStringToObject(b, "type", "codeLogin");
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);

    char url[96];
    snprintf(url, sizeof(url), "%s/v1/user-service/user/%s",
             base, is_email ? "sendemail/code" : "sendsmscode");
    http_resp_t resp;
    esp_err_t herr = http_request(url, "POST", NULL, body, &resp);
    free(body);
    if (herr != ESP_OK) {
        set_err(err, errlen, "CLOUD UNREACHABLE");
        return herr;
    }
    /* success is signalled by HTTP 200; the body may be empty or JSON */
    esp_err_t ret = ESP_FAIL;
    if (resp.status >= 200 && resp.status < 300) {
        ret = ESP_OK;
        ESP_LOGI(TAG, "%s code requested for %s", is_email ? "email" : "sms", account);
    } else {
        set_http_err(resp.status, resp.body, "send code", err, errlen);
        if (!is_risk_blocked(resp.status, resp.body)) {
            set_err(err, errlen, "CODE SEND FAIL");
        }
    }
    free(resp.body);
    return ret;
}

/* One /login POST for a verification code. Fills `auth` on success. */
static esp_err_t login_code_try(const char *base, const char *body,
                                bambu_auth_t *auth, char *err, size_t errlen)
{
    char url[96];
    snprintf(url, sizeof(url), "%s/v1/user-service/user/login", base);
    http_resp_t resp;
    /* single attempt: see the note above http_request_once() */
    esp_err_t herr = http_request_once(url, "POST", NULL, body, &resp);
    if (herr != ESP_OK) {
        /* Distinguish "the cloud refused us" from "we never got an answer".
         * A risk-control mark often surfaces as a failed handshake rather
         * than a clean 418, so checking the captured body here matters. */
        if (is_risk_blocked(resp.status, resp.body)) {
            set_http_err(resp.status, resp.body, "code login", err, errlen);
            free(resp.body);
            return ESP_FAIL;
        }
        set_err(err, errlen, "CLOUD UNREACHABLE");
        free(resp.body);
        return herr;
    }
    esp_err_t ret = ESP_FAIL;
    /* classify a risk-control block before trying to parse the body: a 418
     * body is not JSON we want to interpret as "wrong code". */
    if (is_risk_blocked(resp.status, resp.body)) {
        set_http_err(resp.status, resp.body, "code login", err, errlen);
        free(resp.body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp.body);
    if (!root) {
        set_err(err, errlen, "BAD LOGIN RESP");
        goto done;
    }
    /* Same acceptance rule as the password path (and PrintSphere): a 2xx
     * answer carrying an accessToken is a session. The recursive lookup
     * covers both root and nested "data" copies. */
    const char *tok = json_find_str(root, TOKEN_KEYS);
    if (resp.status >= 200 && resp.status < 300 && tok) {
        strlcpy(auth->token, tok, sizeof(auth->token));
        strlcpy(auth->api_base, base, sizeof(auth->api_base));
        auth->token_expires_in = login_expires_in(root);
        /* Log the shape of the win, never the token itself. token_len matters:
         * a truncated token would fail later at MQTT connect with no obvious
         * cause. uid is logged only after resolve_uid() - printing it earlier
         * would show stale buffer contents, not the resolved id. */
        if (resolve_uid(base, auth->token, auth) != ESP_OK) {
            set_err(err, errlen, "UID PARSE FAIL");
            goto done;
        }
        ESP_LOGI(TAG, "code login OK http %d token_len=%u expires_in=%ds uid=%s",
                 resp.status, (unsigned)strlen(auth->token),
                 auth->token_expires_in, auth->uid);
        ret = ESP_OK;
        goto done;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "success"))) {
        set_err(err, errlen, "NO TOKEN");
        goto done;
    }

    /* Error path, mapped by the NUMERIC code the way PrintSphere does
     * (authenticate_with_email_code):
     *   HTTP 400 + code 1 -> code expired/unknown  -> "CODE EXPIRED"
     *   HTTP 400 + code 2 -> code incorrect        -> "CODE WRONG"
     * Verified contract with curl (2026-10-09):
     *   {"code":1,"error":"Code does not exist or has expired; please
     *    request a new verification code."}                     -> HTTP 400
     *   {"code":2,"error":"Incorrect code"}                     -> HTTP 400
     * The split matters for the user: an expired code just needs a fresh
     * request, a wrong code usually means a typo (or a full-width digit that
     * normalize_code did not catch). */
    const cJSON *cerr = cJSON_GetObjectItem(root, "code");
    const int cloud_code = cJSON_IsNumber(cerr) ? (int)cerr->valuedouble : -1;
    if (resp.status == 400 && cloud_code == 1) {
        set_err(err, errlen, "CODE EXPIRED");
    } else if (resp.status == 400 && cloud_code == 2) {
        set_err(err, errlen, "CODE WRONG");
    } else {
        /* fall back to the message text for shapes the numeric mapping does
         * not cover yet (observed: "does not exist or has expired",
         * "Incorrect code") */
        const cJSON *eobj = cJSON_GetObjectItem(root, "error");
        const char *emsg = cJSON_IsString(eobj) ? eobj->valuestring : NULL;
        if (emsg && (strstr(emsg, "does not exist or has expired") ||
                     strstr(emsg, "Incorrect code"))) {
            set_err(err, errlen, "CODE WRONG");
        } else if (resp.status == 401) {
            set_err(err, errlen, "BAD ACCOUNT/PWD");
        } else {
            set_err(err, errlen, "LOGIN FAIL");
        }
    }
    log_cloud_detail("code login", resp.status, root, resp.body);
done:
    if (root) cJSON_Delete(root);
    free(resp.body);
    return ret;
}

/* Normalise an SMS/email verification code in place.
 *
 * The code is typed by hand on a phone, so two things happen in practice:
 *   - a stray space or hyphen rides along ("123 456", "123-456")
 *   - a Chinese IME emits FULL-WIDTH digits (U+FF10..U+FF19, UTF-8 EF BC 90
 *     ..EF BC 99), which look identical on screen but are not the bytes the
 *     cloud stores. A code that "was entered exactly as received" then fails
 *     with "Code does not exist or has expired", which is very hard to explain
 *     from the logs alone.
 *
 * Returns a pointer to `out`, or `in` unchanged when the result would be
 * empty (never send a code we mangled into nothing). */
static const char *normalize_code(const char *in, char *out, size_t outlen)
{
    if (!in) return in;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < outlen; p++) {
        unsigned char c = *p;
        if (c >= '0' && c <= '9') {
            out[o++] = (char)c;
        } else if (c == 0xEF && p[1] == 0xBC && p[2] >= 0x90 && p[2] <= 0x99) {
            out[o++] = (char)('0' + (p[2] - 0x90));   /* full-width digit */
            p += 2;
        }
        /* every other byte (spaces, '-', newline, ...) is dropped */
    }
    out[o] = 0;
    /* only accept the cleaned form when it still looks like a code */
    return (o >= 4) ? out : in;
}

/* Mask an account for the log: keep enough to recognise it, never the whole
 * number. 13800138000 -> 138****8000 */
static void acct_masked(const char *acct, char *out, size_t outlen)
{
    size_t n = acct ? strlen(acct) : 0;
    if (n <= 5) {
        snprintf(out, outlen, "%.*s", (int)(n ? 1 : 0), acct ? acct : "");
        return;
    }
    snprintf(out, outlen, "%.3s****%s", acct, acct + n - 4);
}

esp_err_t bambu_login_with_code(const char *account, const char *code,
                                uint8_t region, bambu_auth_t *auth,
                                char *err, size_t errlen)
{
    const char *base = (region == CFG_REGION_GLOBAL) ? GLOBAL_API : CN_API;
    memset(auth, 0, sizeof(*auth));

    /* A hand-typed code may arrive with spaces, dashes or full-width digits;
     * see normalize_code(). Keep the original if cleaning would empty it. */
    char codebuf[24];
    code = normalize_code(code, codebuf, sizeof(codebuf));

    /* ONE request, ONE payload shape, ONE region - the PrintSphere contract
     * (authenticate_with_email_code). The five-shape fallback this replaces
     * was removed on purpose:
     *   - the documented {"account","code"} shape is what the server parses
     *     as a code-login attempt (proven 2026-10-09 with curl: every other
     *     shape returns a byte-identical verdict, so extra shapes change
     *     nothing);
     *   - each extra POST feeds the credential-endpoint rate guard, which is
     *     keyed to the outbound IP - the retries were exactly what once armed
     *     the HTTP 418 mark while /sendsmscode kept working;
     *   - a verification code is issued per region (the .cn account system
     *     does not know codes .com issued), so a cross-region retry can only
     *     burn the one-time code, never rescue it. */
    char masked[24];
    acct_masked(account, masked, sizeof(masked));
    ESP_LOGW(TAG, "code login -> %s acct=%s code_len=%u",
             base, masked, (unsigned)strlen(code));

    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "account", account);
    cJSON_AddStringToObject(b, "code", code);
    char *body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);

    esp_err_t e = login_code_try(base, body, auth, err, errlen);
    free(body);
    return e;
}

esp_err_t bambu_auth_from_token(const char *token, uint8_t region,
                                bambu_auth_t *auth)
{
    memset(auth, 0, sizeof(*auth));
    strlcpy(auth->token, token, sizeof(auth->token));
    strlcpy(auth->api_base, (region == CFG_REGION_GLOBAL) ? GLOBAL_API : CN_API,
            sizeof(auth->api_base));
    return resolve_uid(auth->api_base, token, auth);
}

/* Ask the cloud whether it actually accepts this token.
 *
 * Needed because bambu_auth_from_token() can succeed WITHOUT any network call:
 * resolve_uid() decodes the uid straight out of the JWT, so a hand-written
 * token decodes fine and looks valid. That is harmless on the login paths
 * (the token there came from the cloud a moment ago) but wrong on the pasted-
 * token route, where a fabricated or stale string would be reported as
 * "✓ 令牌有效" and only blow up later at the MQTT connect - the hardest place
 * to diagnose it from.
 *
 * /user/bind is used as the probe: it is the same call the monitor needs
 * anyway, and unlike /user-service/user/login it is a plain read endpoint, so
 * it is not what the cloud's credential rate-limit guards. Verified contract
 * (2026-10-09): a bogus token answers 401 {"code":4,"error":"请登录账号!"},
 * a valid one answers 200 with the devices array.
 *
 * Returns ESP_OK when the cloud accepted the token - including for an account
 * that simply has no printers bound. *http_status receives the HTTP code. */
esp_err_t bambu_verify_token(const bambu_auth_t *auth, int *http_status)
{
    if (http_status) *http_status = 0;
    char url[128];
    snprintf(url, sizeof(url), "%s/v1/iot-service/api/user/bind", auth->api_base);
    http_resp_t resp;
    esp_err_t herr = http_request(url, "GET", auth->token, NULL, &resp);
    if (herr != ESP_OK) {
        free(resp.body);
        return herr;
    }
    int st = resp.status;
    if (http_status) *http_status = st;
    ESP_LOGI(TAG, "token verify -> http %d body: %.120s", st,
             resp.body ? resp.body : "");
    free(resp.body);
    return (st >= 200 && st < 300) ? ESP_OK : ESP_FAIL;
}

/* ---------- device list ---------- */

static void parse_mqtt_url(const char *url, const char *def_host, int def_port,
                           char *host, size_t hlen, int *port)
{
    strlcpy(host, def_host, hlen);
    *port = def_port;
    if (!url || !url[0]) return;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    char tmp[96];
    strlcpy(tmp, h, sizeof(tmp));
    char *colon = strchr(tmp, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
        if (*port <= 0) *port = def_port;
    }
    char *slash = strchr(tmp, '/');
    if (slash) *slash = 0;
    if (tmp[0]) strlcpy(host, tmp, hlen);
}

esp_err_t bambu_get_devices(const bambu_auth_t *auth,
                            bambu_dev_t *devs, int max, int *n,
                            int *http_status)
{
    *n = 0;
    if (http_status) *http_status = 0;

    /* /user/bind is the "list the printers on this account" endpoint: it is
     * what the verified reference implementation calls, and it answers with a
     * `devices` array. /user/device is keyed to a single device id, so it was
     * never the right call here; kept only as a fallback. */
    static const char *paths[] = {
        "/v1/iot-service/api/user/bind",
        "/v1/iot-service/api/user/device?user_index=0",
    };

    cJSON *root = NULL;
    int status = 0;
    char last_body[200] = { 0 };

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char url[128];
        snprintf(url, sizeof(url), "%s%s", auth->api_base, paths[i]);
        http_resp_t resp;
        esp_err_t herr = http_request(url, "GET", auth->token, NULL, &resp);
        if (herr != ESP_OK) {
            free(resp.body);
            continue;
        }
        status = resp.status;
        root = cJSON_Parse(resp.body);
        if (!root) {
            /* copy before freeing: a JSON-less 4xx still has to be diagnosable */
            snprintf(last_body, sizeof(last_body), "%.180s", resp.body ? resp.body : "");
        }
        free(resp.body);
        if (root) {
            ESP_LOGI(TAG, "device list via %s -> http %d", paths[i], status);
            break;
        }
        ESP_LOGW(TAG, "device list via %s -> http %d, trying next", paths[i], status);
    }

    if (http_status) *http_status = status;
    if (!root) {
        ESP_LOGW(TAG, "no device list; last http %d body: %s", status, last_body);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    /* The devices array sits at the root of /user/bind today, but the cloud
     * has historically also nested it under "data" (PrintSphere checks both
     * in fetch_bindings). Prefer root, fall back to data. */
    const cJSON *arr = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(arr)) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            arr = cJSON_GetObjectItem(data, "devices");
        }
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "device list has no devices[] array");
        goto done;
    }

    const char *def_mqtt = strstr(auth->api_base, "bambulab.cn") ? CN_MQTT : GLOBAL_MQTT;
    int cnt = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cnt >= max) break;
        const cJSON *id = cJSON_GetObjectItem(it, "dev_id");
        if (!cJSON_IsString(id) || !id->valuestring[0]) continue;
        bambu_dev_t *d = &devs[cnt];
        memset(d, 0, sizeof(*d));
        strlcpy(d->dev_id, id->valuestring, sizeof(d->dev_id));
        const cJSON *nm = cJSON_GetObjectItem(it, "name");
        if (!cJSON_IsString(nm)) nm = cJSON_GetObjectItem(it, "dev_name");
        if (cJSON_IsString(nm)) strlcpy(d->name, nm->valuestring, sizeof(d->name));
        const cJSON *mdl = cJSON_GetObjectItem(it, "dev_product_name");
        if (!cJSON_IsString(mdl)) mdl = cJSON_GetObjectItem(it, "dev_model_name");
        if (cJSON_IsString(mdl)) strlcpy(d->model, mdl->valuestring, sizeof(d->model));
        d->online = cJSON_IsTrue(cJSON_GetObjectItem(it, "online"));
        const cJSON *mu = cJSON_GetObjectItem(it, "mqtt_url");
        parse_mqtt_url(cJSON_IsString(mu) ? mu->valuestring : NULL,
                       def_mqtt, 8883, d->mqtt_host, sizeof(d->mqtt_host), &d->mqtt_port);
        ESP_LOGI(TAG, "dev[%d] %s | %s | online=%d | mqtt=%s:%d",
                 cnt, d->dev_id, d->name, d->online, d->mqtt_host, d->mqtt_port);
        cnt++;
    }
    /* online devices first */
    for (int i = 0; i < cnt; i++) {
        for (int j = i + 1; j < cnt; j++) {
            if (!devs[i].online && devs[j].online) {
                bambu_dev_t t = devs[i]; devs[i] = devs[j]; devs[j] = t;
            }
        }
    }
    *n = cnt;
    ret = cnt > 0 ? ESP_OK : ESP_FAIL;
    if (cnt == 0) {
        ESP_LOGW(TAG, "account reports 0 printers (http %d)", status);
    }
done:
    cJSON_Delete(root);
    return ret;
}
