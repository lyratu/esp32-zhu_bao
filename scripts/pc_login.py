"""Log in to the Bambu cloud from THIS PC, then hand the token to the device.

Why this exists
---------------
The cloud's risk-control guard lives on the credential endpoint
(``/v1/user-service/user/login``) and is keyed to the outbound IP. When it is
armed, ``/sendsmscode`` still returns 200, so the device looks "half online"
and only the login step keeps failing - which reads like a code bug but is not.

The reference implementation sidesteps exactly this by never logging in on the
microcontroller. Its changelog (printsphere-lite, ``backend-v0.4.42``) puts it
plainly: "Bambu 云登录和 token 获取仍由电脑端后端完成，避免把云登录流程塞进
ESP8266". This script applies the same split:

    1. log in from the PC                  (password, or SMS code)
    2. the cloud hands back an accessToken
    3. POST the token to the device route ``/token``

Step 3 uses the route in ``main/net/portal.c``. It never calls the login
endpoint on the device, so the device's own rate limit and risk-control state
are irrelevant to it.

Password first
--------------
Password login is preferred: a wrong password consumes nothing (unlike an SMS
code), it needs no waiting for a message, and the response is unambiguous.
Only fall back to the code flow for accounts the cloud marks
``loginType=verifyCode`` - those can *never* be satisfied with a password, and
this script says so explicitly instead of leaving you guessing.

Usage
-----
    # preferred: password login
    python pc_login.py --ip 192.168.5.246 --account 15629122270

    # the password can also come from the environment / a prompt
    python pc_login.py --ip 192.168.5.246 --account 15629122270 --password 'xxx'

    # account marked verifyCode: request the code, then type it in
    python pc_login.py --ip 192.168.5.246 --account 15629122270 --sms

    # you already received a code (do NOT run this twice - codes are single use)
    python pc_login.py --ip 192.168.5.246 --account 15629122270 --code 123456

    # you already have a token (e.g. copied out of another client's config)
    python pc_login.py --ip 192.168.5.246 --token eyJhbGciOi...

    # international account
    python pc_login.py --ip 192.168.5.246 --account me@example.com --region global

Notes
-----
* Both login flavours hit the SAME cloud endpoint, so a risk-control block
  (HTTP 418) stops either one: the mark is on the outbound IP, not on the
  method. Wait, or move to another network - do not retry in a loop.
* Keep code login to ONE attempt per code.
"""

import argparse
import getpass
import json
import ssl
import sys
import urllib.error
import urllib.request

REGIONS = {
    "cn": ("https://api.bambulab.cn", "https://bambulab.cn", 0),
    "global": ("https://api.bambulab.com", "https://bambulab.com", 1),
}

# Same slicer fingerprint as the firmware (adapted from PrintSphere's
# perform_json_request): bambu_network_agent UA + the X-BBL-* identity set.
# Measured 2026-10-09: the 418 mark is keyed to the outbound IP, not headers;
# this set just makes the client look like a coherent slicer.
UA = "bambu_network_agent/01.09.05.01"

CTX = ssl.create_default_context()


def _headers(region):
    api, site, _ = REGIONS[region]
    return {
        "Accept": "application/json",
        "Content-Type": "application/json",
        "User-Agent": UA,
        "X-BBL-Client-Name": "OrcaSlicer",
        "X-BBL-Client-Type": "slicer",
        "X-BBL-Client-Version": "01.09.05.51",
        "X-BBL-Language": "en-US",
        "X-BBL-OS-Type": "linux",
        "X-BBL-OS-Version": "6.2.0",
        "X-BBL-Agent-Version": "01.09.05.01",
        "X-BBL-Executable-info": "{}",
        "X-BBL-Agent-OS-Type": "linux",
    }


def post(url, payload, region, timeout=20):
    """Return (status, parsed_json_or_None, raw_text)."""
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=_headers(region),
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=CTX) as r:
            text = r.read().decode("utf-8", "replace")
            status = r.status
    except urllib.error.HTTPError as e:
        text = e.read().decode("utf-8", "replace")
        status = e.code
    except Exception as e:                                   # noqa: BLE001
        return 0, None, "%s: %s" % (type(e).__name__, e)
    try:
        return status, json.loads(text), text
    except ValueError:
        return status, None, text


def is_risk_block(status, text):
    return status == 418 or "not a robot" in text or "captchaId" in text


def find_token(obj):
    """The token is a JWT and is not always a top-level field."""
    keys = ("accessToken", "access_token", "idToken", "token")
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k in keys and isinstance(v, str) and v:
                return v
        for v in obj.values():
            t = find_token(v)
            if t:
                return t
    elif isinstance(obj, list):
        for v in obj:
            t = find_token(v)
            if t:
                return t
    return None


def login_password(region, account, password):
    """POST {account,password} to the same endpoint the SMS route uses.

    Preferred over the code flow: a wrong password consumes nothing and the
    answer is unambiguous. Returns None (after explaining) for the two account
    states a password can never satisfy."""
    api, _, _ = REGIONS[region]
    url = api + "/v1/user-service/user/login"
    status, obj, text = post(url, {"account": account, "password": password,
                                   "apiError": ""}, region)
    print("  login(password) -> HTTP %s  %s" % (status, text[:200]))
    if is_risk_block(status, text):
        sys.exit("云端风控拦截了登录请求（HTTP 418）。这是出口 IP 级别的限制，"
                 "和用密码还是验证码无关：\n"
                 "  等几分钟再试，或改用手机热点等其它网络。重复重试只会延长封禁。")
    tok = find_token(obj) if obj else None
    if tok:
        return tok
    if isinstance(obj, dict):
        lt = obj.get("loginType")
        if lt == "verifyCode":
            sys.exit("这个账号在拓竹侧被设为「只能用短信验证码登录」"
                     "(loginType=verifyCode)，密码永远换不到 token。\n"
                     "  请在拓竹 App 里把登录方式改成「密码」；或改走验证码流程：\n"
                     "      python pc_login.py --ip <设备IP> --account %s --sms" % account)
        if lt == "tfa":
            sys.exit("该账号开启了两步验证 (loginType=tfa)，需要额外的一次性口令，"
                     "命令行无法完成。")
    return None


def send_code(region, account):
    """PrintSphere splits the endpoint by account type: phones go to
    /sendsmscode with {"phone",...}, e-mail addresses to /sendemail/code
    with {"email",...}."""
    api, _, _ = REGIONS[region]
    is_mail = "@" in account
    url = api + ("/v1/user-service/user/sendemail/code" if is_mail
                 else "/v1/user-service/user/sendsmscode")
    body = {"email" if is_mail else "phone": account, "type": "codeLogin"}
    status, obj, text = post(url, body, region)
    print("  %s -> HTTP %s  %s" % ("sendemail/code" if is_mail else "sendsmscode",
                                   status, text[:200]))
    if is_risk_block(status, text):
        sys.exit("云端风控拦截了发码请求（出口 IP 已被标记）。请等待数分钟或换网络。")
    return 200 <= status < 300


def login(region, account, code):
    """ONE canonical payload, ONE request - the PrintSphere contract. The
    documented {"account","code"} shape is what the server parses as a
    code-login attempt; every other shape returns a byte-identical verdict,
    so extra shapes only feed the credential-endpoint rate guard."""
    api, _, _ = REGIONS[region]
    url = api + "/v1/user-service/user/login"
    status, obj, text = post(url, {"account": account, "code": code}, region)
    print("  login(code) -> HTTP %s  %s" % (status, text[:200]))
    if is_risk_block(status, text):
        sys.exit("云端风控拦截了登录请求（HTTP 418）。这是出口 IP 级别的限制：\n"
                 "  等几分钟再试，或改用手机热点等其它网络。重复重试只会延长封禁。")
    if isinstance(obj, dict):
        c = obj.get("code")
        if status == 400 and c == 1:
            print("  -> 验证码已过期或不存在（云端 code 1）：请重新获取后立即重试")
        elif status == 400 and c == 2:
            print("  -> 验证码不正确（云端 code 2）：请核对后重试")
    return find_token(obj) if obj else None


def push_to_device(ip, token, account, region_id):
    url = "http://%s/token" % ip
    payload = {"token": token, "account": account, "region": region_id}
    req = urllib.request.Request(
        url, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=25) as r:
            body = r.read().decode("utf-8", "replace")
        print("  device /token -> HTTP %s  %s" % (r.status, body))
        try:
            return json.loads(body).get("ok", False)
        except ValueError:
            return False
    except Exception as e:                                   # noqa: BLE001
        print("  device /token -> 失败: %s: %s" % (type(e).__name__, e))
        return False


def main():
    ap = argparse.ArgumentParser(description="PC-side Bambu cloud login -> device token")
    ap.add_argument("--ip", required=True, help="device IP shown on the OLED")
    ap.add_argument("--account", help="phone number or e-mail")
    ap.add_argument("--password", help="Bambu account password (password login)")
    ap.add_argument("--sms", action="store_true",
                    help="use the SMS-code flow instead of password login")
    ap.add_argument("--code", help="SMS code (skip the interactive prompt)")
    ap.add_argument("--token", help="an accessToken you already have")
    ap.add_argument("--region", choices=sorted(REGIONS), default="cn")
    args = ap.parse_args()

    api, site, region_id = REGIONS[args.region]
    print("区域: %s (%s)\n设备: %s" % (args.region, api, args.ip))

    token = args.token

    # --- password login (default) -------------------------------------------
    if not token and not args.sms:
        if not args.account:
            sys.exit("需要 --account（或直接给 --token）")
        password = args.password
        if not password:
            password = getpass.getpass("拓竹账号密码（不会回显）: ")
        if not password:
            sys.exit("没有密码，退出")
        token = login_password(args.region, args.account, password)
        if not token:
            sys.exit("密码登录未拿到 accessToken（详见上面的 HTTP 状态与云端原文）")
        print("  ✓ 已从电脑端拿到 accessToken（%d 字符）" % len(token))

    # --- SMS-code login -----------------------------------------------------
    if not token:
        if not args.account:
            sys.exit("需要 --account（或直接给 --token）")
        code = args.code
        if not code:
            if not send_code(args.region, args.account):
                sys.exit("验证码发送失败，未继续（不消耗任何验证码）")
            code = getpass.getpass("请输入收到的验证码: ").strip()
        if not code:
            sys.exit("没有验证码，退出")
        token = login(args.region, args.account, code)
        if not token:
            sys.exit("登录未拿到 accessToken（详见上面的 HTTP 状态与云端原文）")
        print("  ✓ 已从电脑端拿到 accessToken（%d 字符）" % len(token))

    if not args.account:
        args.account = input("账号（仅作为记录写入设备，可留空直接回车）: ").strip()

    if push_to_device(args.ip, token, args.account, region_id):
        print("\n✓ 设备已接受该令牌。回到设备页面点「保存并重启设备」即可进入监控。")
    else:
        sys.exit("\n✗ 设备拒绝了该令牌，请检查上面的返回内容。")


if __name__ == "__main__":
    main()
