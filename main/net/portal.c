#include "portal.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "bambu_api.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "portal";

/* Two separate pages, one per phase:
 *   PAGE_PROVISION - AP mode, first boot / BOOT long-press: WiFi only.
 *   PAGE_LOGIN     - STA mode, reachable on the device IP from the LAN:
 *                    Bambu SMS-code login.
 * The shared look-and-feel lives in CSS duplicated in both pages; keeping
 * them separate avoids shipping a lot of dead markup to a phone. */
#define PAGE_CSS \
"*{box-sizing:border-box;margin:0;padding:0}" \
"body{font-family:system-ui,-apple-system,\"PingFang SC\",\"Microsoft YaHei\",sans-serif;" \
"background:#0f1115;color:#e8eaf0;display:flex;justify-content:center;padding:28px 16px}" \
".wrap{width:100%;max-width:430px}h1{font-size:20px}" \
".sub{color:#8a93a6;font-size:13px;margin:4px 0 20px}" \
".card{background:#171a21;border:1px solid #262b36;border-radius:14px;padding:18px;margin-bottom:14px}" \
"h2{font-size:13px;color:#00d4a0;letter-spacing:.08em;margin-bottom:10px}" \
"h3{font-size:12px;color:#8a93a6;margin:16px 0 8px;font-weight:600}" \
"label{display:block;font-size:12px;color:#8a93a6;margin:10px 0 4px}" \
"input,select,textarea{width:100%;padding:10px 12px;background:#0c0e12;border:1px solid #262b36;" \
"border-radius:10px;color:#e8eaf0;font-size:15px;outline:none}" \
"input:focus,select:focus,textarea:focus{border-color:#00d4a0}" \
"textarea{font-size:11px;font-family:ui-monospace,Menlo,Consolas,monospace;resize:vertical}" \
".row{display:flex;gap:8px;align-items:center}.row select{flex:1}" \
".mini{flex:0 0 auto;padding:10px 12px;background:#1d2330;border:1px solid #262b36;" \
"border-radius:10px;color:#e8eaf0;font-size:13px}" \
".btn{width:100%;padding:13px;border:0;border-radius:12px;background:#00d4a0;" \
"color:#052a21;font-size:16px;font-weight:700;margin-top:6px}" \
".btn[disabled],.mini[disabled]{opacity:.5}" \
"#msg{margin-top:14px;font-size:14px;min-height:20px}" \
".ok{color:#00d4a0}.bad{color:#ff6b6b}.warn{color:#ffcc55}" \
".hint{font-size:11px;color:#8a93a6;margin-top:4px}" \
".tips{color:#8a93a6;font-size:12px;line-height:1.7;margin-top:14px}" \
".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;vertical-align:1px}"

static const char PAGE_PROVISION[] = R"rawliteral(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>A1 mini Monitor 配网</title>
<style>)rawliteral" PAGE_CSS R"rawliteral(</style></head><body><div class="wrap">
<h1>A1 mini 监控器</h1>
<div class="sub">第 1 步：只需连接 WiFi —— 保存后设备将重启并自动联网</div>
<div class="card"><h2>WIFI 网络</h2>
<label>扫描到的网络</label>
<div class="row"><select id="ssid_sel"><option value="">扫描中...</option></select>
<button class="mini" onclick="scan()">刷新</button></div>
<label>或手动输入 SSID（优先使用）</label>
<input id="ssid" placeholder="WiFi 名称">
<label>WiFi 密码</label><input id="pass" type="password" placeholder="无密码可留空">
</div>
<button class="btn" id="save" onclick="save()">保存并重启设备</button>
<div id="msg"></div>
<div class="tips">· 本页只需填 WiFi，保存后设备重启并自动联网<br>
· 联网后屏幕会显示一个 IP 地址，用手机浏览器打开它完成拓竹账号登录<br>
· 长按开发板 BOOT 键 3 秒可随时回到本页重新配网</div>
<script>
const $=id=>document.getElementById(id);
async function scan(){
 const sel=$('ssid_sel');sel.innerHTML='<option value="">扫描中...</option>';
 try{const r=await fetch('/scan');const j=await r.json();
  sel.innerHTML=j.length?j.map(x=>`<option value="${x.s}">${x.s} (${x.r}dBm)</option>`).join('')
   :'<option value="">未发现网络</option>';
 }catch(e){sel.innerHTML='<option value="">扫描失败</option>';}
}
async function loadCfg(){
 try{const r=await fetch('/cfg');const j=await r.json();
  /* re-read /cfg (not just at boot) so a phone connected to the config AP
   * sees the SSID it just saved instead of an empty field */
  if(j.ssid)$('ssid').value=j.ssid;
 }catch(e){}
}
async function save(){
 const ssid=($('ssid').value.trim()||$('ssid_sel').value);
 const b={ssid:ssid,pass:$('pass').value};
 const m=$('msg');
 if(!ssid){m.className='bad';m.textContent='请选择或输入 WiFi 名称';return;}
 const btn=$('save');btn.disabled=true;
 m.className='';m.textContent='保存中...';
 try{
  const r=await fetch('/save',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const j=await r.json();
  if(j.ok){m.className='ok';m.textContent='✓ 已保存，设备重启中，请留意屏幕上的 IP...';}
  else{m.className='bad';m.textContent=j.msg||'保存失败';btn.disabled=false;}
 }catch(e){m.className='bad';m.textContent='请求失败: '+e;btn.disabled=false;}
}
scan();loadCfg();
</script></div></body></html>)rawliteral";

static const char PAGE_LOGIN[] = R"rawliteral(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>A1 mini Monitor 登录</title>
<style>)rawliteral" PAGE_CSS R"rawliteral(</style></head><body><div class="wrap">
<h1>拓竹账号登录</h1>
<div class="sub">第 2 步：短信验证码登录 —— 成功后设备重启进入监控</div>
<div class="card"><h2>网络状态</h2>
<div style="font-size:14px"><span id="cdot" class="dot" style="background:#8a93a6"></span>
<span id="ctext">检查中...</span></div>
<div class="hint" id="chint"></div>
</div>
<div class="card"><h2>拓竹账号</h2>
<label>账号（手机号或邮箱）</label>
<input id="acct" autocomplete="username" placeholder="拓竹 App 注册的手机号或邮箱">
<label>账号区域</label>
<select id="region"><option value="0" selected>中国大陆（api.bambulab.cn）</option>
<option value="1">国际（api.bambulab.com）</option></select>
<div class="row" style="margin-top:14px">
  <button class="mini" id="codebtn" onclick="getCode()">获取验证码</button>
  <input id="code" placeholder="验证码" style="flex:1">
</div>
<div class="row" style="margin-top:10px">
  <button class="mini" id="loginbtn" onclick="login()">用验证码登录</button>
</div>
<div class="hint" id="tokinfo"></div>
</div>

<button class="btn" id="save" onclick="save()">保存并重启设备</button>
<div id="msg"></div>
<div class="tips">· 只有登录成功（拿到 token）后才能保存<br>
· 凭据只保存在设备本机 NVS 中</div>
<script>
const $=id=>document.getElementById(id);
let tokenReady=false;
async function netstat(){
 try{const r=await fetch('/netstatus');const j=await r.json();
  const d=$('cdot');
  if(j.online){d.style.background='#00d4a0';$('ctext').textContent='已联网';
   $('chint').textContent='设备 IP '+j.ip;}
  else{d.style.background='#ff6b6b';$('ctext').textContent='未联网';
   $('chint').textContent='设备无法访问拓竹云，请先完成配网';}
  return j.online;
 }catch(e){$('ctext').textContent='状态未知';return false;}
}
async function loadCfg(){
 try{const r=await fetch('/cfg');const j=await r.json();
  /* pre-fill the phone number if it was already entered somewhere */
  if(j.acct)$('acct').value=j.acct;
  if(typeof j.region==='number')$('region').value=String(j.region);
 }catch(e){}
}
function markToken(t){
 tokenReady=true;
 $('tokinfo').className='hint ok';
 $('tokinfo').textContent='✓ 已登录（token '+t+'）';
}
function errText(msg){
 /* map the short device errors to something the user can act on */
 if(!msg) return '操作失败';
 if(msg.indexOf('还需等待')>=0)
   return '云端风控拦截（不是验证码的问题）：设备已暂停云请求，'
        + msg.replace(/^CLOUD BLOCKED:\s*/,'') + '。期间不要连续重试，重试只会延长封禁';
 if(msg.indexOf('BLOCKED')>=0)
   return '云端风控拦截：请等待数分钟，或改用手机热点等其他网络再试'
        + '（同一路由器下所有设备共用出口 IP，都会受影响）';
 if(msg.indexOf('UNREACHABLE')>=0)
   return '无法连接拓竹云：请确认设备已联网（看屏幕上的 IP 是否能上网）';
 if(msg.indexOf('REJECTED')>=0||msg.indexOf('LOGIN FAIL')>=0)
   return '登录被云端拒绝：请检查「账号区域」是否选对（中国大陆 / 国际）';
 if(msg.indexOf('NO PHONE')>=0) return '请先填写账号（手机号或邮箱）';
 if(msg.indexOf('BAD LOGIN RESP')>=0) return '云端返回了无法解析的内容，请重试';
 /* verification-code login; the numeric cloud codes map to distinct device
  * errors now: 1 = expired/unknown, 2 = incorrect */
 if(msg.indexOf('SEND FAIL')>=0) return '验证码发送失败，请稍后重试';
 if(msg.indexOf('CODE USED')>=0)
   return '该验证码已被使用过了：验证码是一次性的，请重新获取';
 if(msg.indexOf('CODE EXPIRED')>=0)
   return '验证码已过期或不存在：验证码有效期很短，请重新获取后立即填入';
 if(msg.indexOf('CODE WRONG')>=0)
   return '验证码不正确：请核对短信/邮件里的数字后重试（注意输入法不要用全角数字）';
 if(msg.indexOf('NO DEVICE')>=0) return '该账号下没有找到打印机设备';
 return msg;
}
async function getCode(){
 const acct=$('acct').value.trim();const m=$('msg');
 if(!acct){m.className='bad';m.textContent='请先填写账号（手机号或邮箱）';return;}
 const isMail=acct.indexOf('@')>=0;
 const b=$('codebtn');b.disabled=true;
 m.className='';m.textContent='正在发送验证码...';
 try{
  const r=await fetch('/smscode',{method:'POST',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({account:acct,region:parseInt($('region').value)})});
  const j=await r.json();
  if(j.ok){m.className='ok';m.textContent=isMail?'✓ 验证码已发送，请查收邮箱':'✓ 验证码已发送，请查收短信';}
  else{m.className='bad';m.innerHTML=errText(j.msg);}
 }catch(e){m.className='bad';m.textContent='请求失败: '+e;}
 b.disabled=false;
}
async function login(){
 const acct=$('acct').value.trim(),code=$('code').value.trim();const m=$('msg');
 if(!acct||!code){m.className='bad';m.textContent='请填写账号和验证码';return;}
 const b=$('loginbtn');b.disabled=true;
 m.className='';m.textContent='登录中...';
 try{
  const r=await fetch('/login',{method:'POST',
   headers:{'Content-Type':'application/json'},
   body:JSON.stringify({account:acct,code:code,region:parseInt($('region').value)})});
  const j=await r.json();
  if(j.ok){m.className='ok';m.textContent='✓ 登录成功，请点下方保存配置';markToken(j.token);}
  else{m.className='bad';m.innerHTML=errText(j.msg);}
 }catch(e){m.className='bad';m.textContent='请求失败: '+e;}
 b.disabled=false;
}
async function save(){
 const b={acct:$('acct').value.trim(),region:parseInt($('region').value)};
 const m=$('msg');
 if(!b.acct){m.className='bad';m.textContent='请填写拓竹账号';return;}
 if(!tokenReady){m.className='bad';m.textContent='请先获取验证码并登录成功';return;}
 const btn=$('save');btn.disabled=true;
 m.className='';m.textContent='保存中...';
 try{
  const r=await fetch('/save',{method:'POST',
   headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  const j=await r.json();
  if(j.ok){m.className='ok';m.textContent='✓ 已保存，设备重启中，屏幕进入监控即完成...';}
  else{m.className='bad';m.textContent=j.msg||'保存失败';btn.disabled=false;}
 }catch(e){m.className='bad';m.textContent='请求失败: '+e;btn.disabled=false;}
}
netstat();loadCfg();
</script></div></body></html>)rawliteral";

/* ---------------- DNS: whitelist captive-portal hijack ----------------
 *
 * Only the OS/connectivity-probe hostnames are answered with 192.168.4.1 so
 * the browser pops up the config page. Everything else - crucially
 * api.bambulab.cn - is relayed to the upstream DNS server handed out by the
 * router over DHCP. The ESP32 must therefore keep its STA link up (APSTA):
 * the AP alone has no route to the internet. */

static const char *k_hijack[] = {
    "connectivitycheck.gstatic.com",
    "connectivitycheck.android.com",
    "clients3.google.com",
    "www.google.com",
    "captive.apple.com",
    "www.apple.com",
    "gnu.org",
    "www.msftconnecttest.com",
    "msftconnecttest.com",
    "www.msftncsi.com",
    "connectivity-check.ubuntu.com",
    "networkcheck.kde.org",
    "detectportal.firefox.com",
};

static bool host_is_hijacked(const char *host)
{
    for (size_t i = 0; i < sizeof(k_hijack) / sizeof(k_hijack[0]); i++) {
        const char *h = k_hijack[i];
        size_t hl = strlen(h), nl = strlen(host);
        if (nl == hl && !strcasecmp(host, h)) return true;
        /* also match subdomains, e.g. "x.captive.apple.com" */
        if (nl > hl && host[nl - hl - 1] == '.' && !strcasecmp(host + nl - hl, h)) {
            return true;
        }
    }
    return false;
}

/* decode the first QNAME in a DNS packet into dotted form ("" on failure) */
static void dns_qname(const uint8_t *buf, int n, char *out, size_t outlen)
{
    out[0] = 0;
    int i = 12, o = 0;
    while (i < n && buf[i]) {
        int len = buf[i++];
        if (len > 63 || i + len > n) { out[0] = 0; return; }
        if (o && o < (int)outlen - 1) out[o++] = '.';
        for (int k = 0; k < len && o < (int)outlen - 1; k++) {
            out[o++] = (char)buf[i + k];
        }
        i += len;
    }
    out[o] = 0;
}

/* build a single-A answer pointing at 192.168.4.1 */
static int dns_answer_hijack(const uint8_t *q, int n, uint8_t *resp, int resplen)
{
    uint16_t flags = (q[2] << 8) | q[3];
    flags |= 0x8000 | 0x0180;                     /* response + rd + ra */
    int qd = (q[4] << 8) | q[5];
    if (qd < 1) return -1;
    int i = 12;
    for (int k = 0; k < qd; k++) {
        while (i < n && q[i]) i += q[i] + 1;
        i += 5;                                   /* zero + qtype + qclass */
    }
    if (i > n) return -1;
    int p = 0;
    memcpy(resp, q, i);                           /* echo header + question */
    p = i;
    resp[2] = flags >> 8;
    resp[3] = flags & 0xFF;
    resp[6] = 0; resp[7] = 1;                     /* ancount = 1 */
    resp[8] = resp[9] = resp[10] = resp[11] = 0;  /* no ns/ar records */
    resp[p++] = 0xC0; resp[p++] = 12;             /* name pointer */
    resp[p++] = 0; resp[p++] = 1;                 /* type A  */
    resp[p++] = 0; resp[p++] = 1;                 /* class IN */
    resp[p++] = 0; resp[p++] = 0; resp[p++] = 0; resp[p++] = 60; /* ttl */
    resp[p++] = 0; resp[p++] = 4;                 /* rdlength */
    resp[p++] = 192; resp[p++] = 168; resp[p++] = 4; resp[p++] = 1;
    (void)resplen;
    return p;
}

/* Relay a query to the upstream DNS server (STA side). Returns reply len. */
static int dns_upstream(const uint8_t *q, int n, uint8_t *resp, int resplen)
{
    int up = wifi_dns_server();                   /* 0 when unknown */
    if (!up) return -1;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = up;

    int r = -1;
    if (sendto(s, q, n, 0, (struct sockaddr *)&dst, sizeof(dst)) == n) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int got = recvfrom(s, resp, resplen, 0, (struct sockaddr *)&from, &flen);
        if (got > 0) r = got;
    }
    close(s);
    return r;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[600];
    uint8_t resp[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        if (buf[2] & 0x80) continue;              /* ignore responses */

        char host[128];
        dns_qname(buf, n, host, sizeof(host));

        int p;
        if (host_is_hijacked(host)) {
            p = dns_answer_hijack(buf, n, resp, sizeof(resp));
        } else {
            p = dns_upstream(buf, n, resp, sizeof(resp));
        }
        if (p > 0) {
            sendto(sock, resp, p, 0, (struct sockaddr *)&from, flen);
        } else if (host[0]) {
            /* upstream unreachable: still answer probes so the phone shows
             * the portal instead of hanging on DNS. Real domains get a
             * NXDOMAIN-free empty answer only if they are probes; for others
             * we drop the packet so the client can retry via another server. */
            if (host_is_hijacked(host)) {
                p = dns_answer_hijack(buf, n, resp, sizeof(resp));
                if (p > 0) sendto(sock, resp, p, 0, (struct sockaddr *)&from, flen);
            }
        }
    }
}

/* ---------------- http handlers ---------------- */

/* Which page to serve depends on where the request came in:
 *   - AP interface (192.168.4.x)  -> provisioning page
 *   - STA interface (device IP)   -> Bambu login page */
static bool s_on_sta = false;   /* set by portal_start() */

static esp_err_t h_index(httpd_req_t *req)
{
    /* When the station is up the portal runs in "login" mode: the user
     * reaches the device over the LAN, so serve the Bambu login page.
     * Otherwise the request can only have come from the config AP. */
    const char *page = (s_on_sta && wifi_sta_online()) ? PAGE_LOGIN : PAGE_PROVISION;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

/* The Bambu logo on the config AP should not trigger the DNS hijack page
 * loop on Android/iOS probes; those are answered by the DNS task. */
static esp_err_t h_scan(httpd_req_t *req)
{
    char ssids[20][33];
    int8_t rssi[20];
    int n = wifi_scan(ssids, rssi, 20);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "s", ssids[i]);
        cJSON_AddNumberToObject(o, "r", rssi[i]);
        cJSON_AddItemToArray(arr, o);
    }
    char *j = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, j, HTTPD_RESP_USE_STRLEN);
    free(j);
    return e;
}

/* read a small JSON POST body into buf (NUL terminated). returns len or -1 */
static int read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= buflen) {
        return -1;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;
    return got;
}

/* POST {ssid,pass} -> apply the WiFi now so cloud calls can go out.
 * Keeps the AP alive; the page polls /netstatus for the result. */
static esp_err_t h_wificonn(httpd_req_t *req)
{
    char buf[256];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = cJSON_Parse(buf);
    char ssid[33] = { 0 }, pass[65] = { 0 };
    if (root) {
        const cJSON *s = cJSON_GetObjectItem(root, "ssid");
        if (cJSON_IsString(s)) strlcpy(ssid, s->valuestring, sizeof(ssid));
        const cJSON *p = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(p)) strlcpy(pass, p->valuestring, sizeof(pass));
        cJSON_Delete(root);
    }
    if (!ssid[0]) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"no ssid\"}", HTTPD_RESP_USE_STRLEN);
    }
    /* fall back to the stored password when the user only picks a network.
     * Use app_cfg_raw_load(): app_cfg_load() would report "invalid" (and so
     * lose the password) whenever the Bambu account is not filled in yet. */
    static app_cfg_t c;
    app_cfg_raw_load(&c);
    if (!pass[0] && !strcmp(c.ssid, ssid)) {
        strlcpy(pass, c.pass, sizeof(pass));
    }
    wifi_sta_reconnect_async(ssid, pass);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
}

/* GET -> station connectivity, used to gate the SMS login buttons. */
static esp_err_t h_netstatus(httpd_req_t *req)
{
    char ip[16];
    wifi_get_ip(ip, sizeof(ip));
    uint32_t dns = wifi_dns_server();
    char out[128];
    snprintf(out, sizeof(out),
             "{\"online\":%s,\"ip\":\"%.15s\",\"dns\":\"%u.%u.%u.%u\"}",
             wifi_sta_online() ? "true" : "false", ip,
             (unsigned)(dns & 0xff), (unsigned)((dns >> 8) & 0xff),
             (unsigned)((dns >> 16) & 0xff), (unsigned)((dns >> 24) & 0xff));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* ---------- request throttling ---------------------------------------
 *
 * The Bambu cloud risk control answers HTTP 418 once an outbound IP starts
 * looking abusive, and the mark is sticky - it takes the whole household
 * (every device behind the router) offline for the cloud APIs. An impatient
 * user tapping "get code" / "login" repeatedly, or a page that retries on
 * its own, is exactly what trips it. So enforce a per-endpoint cooldown on
 * the device itself, before anything leaves the LAN.
 *
 * These are deliberately generous: the goal is to stop double-taps and
 * stuck-in-a-loop pages, not to shape normal use. */
#define SMS_COOLDOWN_S   60   /* one SMS request per minute */
#define LOGIN_COOLDOWN_S 10   /* one login attempt per 10 s */

/* Shared by every code-login POST on this page.
 *
 * The route POSTs to the cloud login endpoint (/v1/user-service/user/login),
 * which is where the risk-control counter lives. A short shared timer keeps
 * double-taps from looking like a brute-force attempt to the cloud. */
static uint32_t s_last_login = 0;

/* Once the cloud answers with a risk-control block, stop knocking for a while.
 *
 * The mark is keyed to the outbound IP, it counts requests, and it only clears
 * if we actually stop: every extra request refreshes it. A 10 s login cooldown
 * is useless against that - the user taps again, the mark sticks, and the SMS
 * endpoint keeps working so it looks like "login is broken". Two minutes is
 * long enough to let the counter decay and short enough that the SMS code is
 * usually still usable when we come back. */
#define BLOCK_COOLDOWN_S 120
static uint32_t s_blocked_until = 0;

static bool block_cooldown_active(int *remain)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s_blocked_until != 0 && (int32_t)(s_blocked_until - now) > 0) {
        if (remain) {
            *remain = (int)(s_blocked_until - now);
        }
        return true;
    }
    return false;
}

static void note_cloud_error(const char *err)
{
    if (err && strstr(err, "BLOCKED")) {
        s_blocked_until = (uint32_t)(esp_timer_get_time() / 1000000) + BLOCK_COOLDOWN_S;
        ESP_LOGW(TAG, "cloud risk control: pausing all cloud calls for %d s",
                 BLOCK_COOLDOWN_S);
    }
}

static bool throttle_blocked(uint32_t *last, uint32_t cooldown_s, int *remain)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    if (*last != 0 && (now - *last) < cooldown_s) {
        if (remain) {
            *remain = (int)(cooldown_s - (now - *last));
        }
        return true;
    }
    *last = now;
    return false;
}

static esp_err_t h_smscode(httpd_req_t *req)
{
    static uint32_t last_sms = 0;
    int remain = 0;
    if (block_cooldown_active(&remain)) {
        char out[128];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"msg\":\"CLOUD BLOCKED: 还需等待 %d 秒（云端风控，继续点击只会延长封禁）\"}",
                 remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }
    if (throttle_blocked(&last_sms, SMS_COOLDOWN_S, &remain)) {
        char out[128];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"msg\":\"请等待 %d 秒后再获取验证码（避免被云端风控）\"}",
                 remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }
    char buf[512];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = cJSON_Parse(buf);
    char account[64] = { 0 };
    uint8_t region = CFG_REGION_CN;
    if (root) {
        /* "account" is the canonical key; "phone" kept for older pages that
         * may still be open in a browser tab. An address containing '@' is
         * routed to the e-mail code endpoint by bambu_send_sms_code(). */
        const cJSON *p = cJSON_GetObjectItem(root, "account");
        if (!cJSON_IsString(p)) p = cJSON_GetObjectItem(root, "phone");
        if (cJSON_IsString(p)) strlcpy(account, p->valuestring, sizeof(account));
        const cJSON *r = cJSON_GetObjectItem(root, "region");
        if (cJSON_IsNumber(r)) region = (uint8_t)r->valuedouble;
        cJSON_Delete(root);
    }
    char err[32] = { 0 };
    esp_err_t e = bambu_send_sms_code(account, region, err, sizeof(err));
    note_cloud_error(err);
    char out[96];
    if (e == ESP_OK) {
        snprintf(out, sizeof(out), "{\"ok\":true}");
    } else {
        snprintf(out, sizeof(out), "{\"ok\":false,\"msg\":\"%s\"}", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_login(httpd_req_t *req)
{
    int remain = 0;
    if (block_cooldown_active(&remain)) {
        char out[128];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"msg\":\"CLOUD BLOCKED: 还需等待 %d 秒（云端风控，继续点击只会延长封禁）\"}",
                 remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }
    if (throttle_blocked(&s_last_login, LOGIN_COOLDOWN_S, &remain)) {
        char out[128];
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"msg\":\"请等待 %d 秒后再试\"}", remain);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    }
    char buf[512];
    if (read_body(req, buf, sizeof(buf)) < 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }
    cJSON *root = cJSON_Parse(buf);
    char account[64] = { 0 }, code[16] = { 0 };
    uint8_t region = CFG_REGION_CN;
    if (root) {
        const cJSON *a = cJSON_GetObjectItem(root, "account");
        if (cJSON_IsString(a)) strlcpy(account, a->valuestring, sizeof(account));
        const cJSON *c = cJSON_GetObjectItem(root, "code");
        if (cJSON_IsString(c)) strlcpy(code, c->valuestring, sizeof(code));
        const cJSON *r = cJSON_GetObjectItem(root, "region");
        if (cJSON_IsNumber(r)) region = (uint8_t)r->valuedouble;
        cJSON_Delete(root);
    }

    static bambu_auth_t auth;   /* large: keep off the httpd stack */
    char err[32] = { 0 };
    esp_err_t e = bambu_login_with_code(account, code, region, &auth, err, sizeof(err));
    note_cloud_error(err);
    char out[128];
    if (e == ESP_OK) {
        /* cache the token immediately so a plain "save" keeps it */
        app_cfg_set_token(auth.token);
        /* preview: last 6 chars only */
        const char *t = auth.token;
        size_t n = strlen(t);
        const char *tp = n > 6 ? t + n - 6 : t;
        snprintf(out, sizeof(out),
                 "{\"ok\":true,\"token\":\"...%.6s\"}", tp);
        ESP_LOGI(TAG, "portal code-login OK uid=%s", auth.uid);
    } else {
        snprintf(out, sizeof(out), "{\"ok\":false,\"msg\":\"%s\"}", err);
        ESP_LOGW(TAG, "portal code-login failed: %s", err);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_cfg(httpd_req_t *req)
{
    /* app_cfg_raw_load(), NOT app_cfg_load(): the latter returns false for a
     * half-filled config (WiFi saved, Bambu account still missing), which
     * would make the config page come up blank right after provisioning. */
    static app_cfg_t c;
    app_cfg_raw_load(&c);
    char out[400];
    /* never send the WiFi/Bambu passwords back to the browser; the token is
     * truncated to a harmless tail so the user can tell one is cached. */
    const char *t = c.token;
    size_t n = strlen(t);
    char tokprev[16] = "";
    if (n > 6) snprintf(tokprev, sizeof(tokprev), "...%.6s", t + n - 6);
    snprintf(out, sizeof(out),
             "{\"ssid\":\"%.32s\",\"acct\":\"%.63s\",\"region\":%u,\"token\":\"%.12s\"}",
             c.ssid, c.account, c.region, tokprev);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_save(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"bad length\"}", HTTPD_RESP_USE_STRLEN);
    }
    char buf[1025];
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;

    bool provisioning = !(s_on_sta && wifi_sta_online());

    cJSON *root = cJSON_Parse(buf);
    static app_cfg_t old, c;     /* large structs: keep off the stack */
    /* raw load: a half-filled config must still contribute its WiFi fields */
    app_cfg_raw_load(&old);
    memcpy(&c, &old, sizeof(c)); /* start from current config */
    bool ok = false;
    const char *why = "missing fields";
    if (root) {
        const cJSON *f;
        f = cJSON_GetObjectItem(root, "ssid");
        if (cJSON_IsString(f) && f->valuestring[0]) strlcpy(c.ssid, f->valuestring, sizeof(c.ssid));
        f = cJSON_GetObjectItem(root, "pass");
        if (cJSON_IsString(f) && f->valuestring[0]) strlcpy(c.pass, f->valuestring, sizeof(c.pass));
        f = cJSON_GetObjectItem(root, "acct");
        if (cJSON_IsString(f) && f->valuestring[0]) strlcpy(c.account, f->valuestring, sizeof(c.account));
        f = cJSON_GetObjectItem(root, "pwd");
        if (cJSON_IsString(f) && f->valuestring[0]) strlcpy(c.password, f->valuestring, sizeof(c.password));
        f = cJSON_GetObjectItem(root, "region");
        c.region = cJSON_IsNumber(f) ? (uint8_t)f->valuedouble : CFG_REGION_CN;
        cJSON_Delete(root);
    }

    if (provisioning) {
        /* phase 1 (AP): only the WiFi is required; the Bambu account is
         * collected later on the login page once we are online. */
        ok = c.ssid[0] != 0;
        why = "ssid required";
    } else {
        /* phase 2 (STA): WiFi is already known - require an account plus a
         * token (SMS login) or a password. */
        ok = c.ssid[0] && c.account[0] && (c.password[0] || c.token[0]);
        why = "login first";
    }

    const char *resp = ok
        ? "{\"ok\":true,\"msg\":\"saved\"}"
        : "{\"ok\":false,\"msg\":\"missing fields\"}";
    if (!ok) {
        static char fail[64];
        snprintf(fail, sizeof(fail), "{\"ok\":false,\"msg\":\"%s\"}", why);
        resp = fail;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    if (ok) {
        app_cfg_save(&c);
        ESP_LOGI(TAG, "config saved (%s), rebooting...", provisioning ? "wifi" : "account");
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
    }
    return ESP_OK;
}

/* DIAGNOSTIC: fire one verified TLS handshake on demand and report it.
 * Useful after correcting the clock: no reboot needed to see if it worked. */
static esp_err_t h_tlsprobe(httpd_req_t *req)
{
    bambu_tls_probe_once();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"probe done, see log\"}",
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_redirect(httpd_req_t *req){
    /* send the phone to the right page: config AP or the device IP */
    char loc[40];
    if (s_on_sta && wifi_sta_online()) {
        char ip[16];
        wifi_get_ip(ip, sizeof(ip));
        snprintf(loc, sizeof(loc), "http://%s/", ip);
    } else {
        strlcpy(loc, "http://192.168.4.1/", sizeof(loc));
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t portal_start(bool on_sta)
{
    s_on_sta = on_sta;
    /* the DNS hijack is only meaningful while the config AP is up; on the
     * LAN it would blackhole the user's real lookups. */
    if (!on_sta) {
        xTaskCreate(dns_task, "dns", 3584, NULL, 5, NULL);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* The login handler runs the whole cloud round trip on this task, and a
     * *successful* code login also resolves the uid: jwt_payload_json() holds
     * ~2.1 KB for the base64 payload plus ~1.6 KB of decoded bytes, and
     * extract_uid() another 2 KB of JSON - all live at once, on top of the
     * ~1 KB of frames the handler chain itself uses. The old 10 KB left almost
     * no headroom, and this path only ever runs for real on the first
     * successful login, i.e. at the one moment a stack overflow would look
     * like "the device rebooted for no reason". */
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 16;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    const httpd_uri_t u_index = { .uri = "/", .method = HTTP_GET, .handler = h_index };
    const httpd_uri_t u_scan  = { .uri = "/scan", .method = HTTP_GET, .handler = h_scan };
    const httpd_uri_t u_cfg   = { .uri = "/cfg", .method = HTTP_GET, .handler = h_cfg };
    const httpd_uri_t u_net   = { .uri = "/netstatus", .method = HTTP_GET, .handler = h_netstatus };
    const httpd_uri_t u_wconn = { .uri = "/wificonn", .method = HTTP_POST, .handler = h_wificonn };
    const httpd_uri_t u_sms   = { .uri = "/smscode", .method = HTTP_POST, .handler = h_smscode };
    const httpd_uri_t u_login = { .uri = "/login", .method = HTTP_POST, .handler = h_login };
    const httpd_uri_t u_save  = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    const httpd_uri_t u_probe = { .uri = "/tlsprobe", .method = HTTP_GET, .handler = h_tlsprobe };
    const httpd_uri_t u_any   = { .uri = "/*", .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(server, &u_index);
    httpd_register_uri_handler(server, &u_scan);
    httpd_register_uri_handler(server, &u_cfg);
    if (!on_sta) {
        httpd_register_uri_handler(server, &u_wconn);
    }
    httpd_register_uri_handler(server, &u_net);
    httpd_register_uri_handler(server, &u_sms);
    httpd_register_uri_handler(server, &u_login);
    httpd_register_uri_handler(server, &u_save);
    if (on_sta) {
        httpd_register_uri_handler(server, &u_probe);
    }
    httpd_register_uri_handler(server, &u_any);
    ESP_LOGI(TAG, "portal httpd on :80 (%s)", on_sta ? "STA/login" : "AP/config");
    return ESP_OK;
}
