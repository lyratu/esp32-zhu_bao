"""Render the OLED screens offline so the layout can be checked without
squinting at a 0.96" panel.

It re-implements the same drawing calls ui.c makes (8x8 ASCII + 12x12 CJK)
against a byte buffer, then dumps each screen as ASCII art. Keep the screens
in sync with ui.c when the layout or wording changes.

Run:  python scripts/preview_ui.py
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_cjk_font import render as cjk_render, SIZE as CJK_SIZE  # noqa: E402

W, H = 128, 64   # 0.96" panel

# --- load the 8x8 ASCII table out of font8x8_basic.h -------------------------
_ascii = {}
_src = open(str(Path(__file__).resolve().parents[1] / "main/display/font8x8_basic.h"),
            encoding="utf-8").read()
for m in re.finditer(r"\{\s*((?:0x[0-9a-fA-F]{2}\s*,\s*){7}0x[0-9a-fA-F]{2})\s*\}", _src):
    idx = len(_ascii)
    _ascii[idx] = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))]

_cjk_cache = {}


def _cjk_rows(ch):
    if ch not in _cjk_cache:
        img = cjk_render(ch)
        px = img.load()
        rows = []
        for y in range(CJK_SIZE):
            v = 0
            for x in range(CJK_SIZE):
                if px[x, y]:
                    v |= 1 << x
            rows.append(v)
        _cjk_cache[ch] = rows
    return _cjk_cache[ch]


class Screen:
    def __init__(self):
        self.buf = [[0] * W for _ in range(H)]

    def fill_rect(self, x, y, w, h, on=1):
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.buf[yy][xx] = on

    def rect(self, x, y, w, h, on=1):
        self.fill_rect(x, y, w, 1, on)
        self.fill_rect(x, y + h - 1, w, 1, on)
        self.fill_rect(x, y, 1, h, on)
        self.fill_rect(x + w - 1, y, 1, h, on)

    def text_w(self, s, scale=1):
        w = 0
        for ch in s:
            w += (8 if ord(ch) < 0x80 else CJK_SIZE) * scale
        return w

    def text(self, x, y, s, scale=1, invert=False):
        cx = x
        for ch in s:
            cp = ord(ch)
            if cp < 0x80:
                g = _ascii.get(cp)
                if g:
                    for r in range(8):
                        for c in range(8):
                            on = (g[r] >> c) & 1
                            if invert:
                                on = not on
                            if on:
                                self.fill_rect(cx + c * scale, y + r * scale, scale, scale, 1)
                cx += 8 * scale
            else:
                for r, row in enumerate(_cjk_rows(ch)):
                    for c in range(CJK_SIZE):
                        on = (row >> c) & 1
                        if invert:
                            on = not on
                        if on:
                            self.fill_rect(cx + c * scale, y + r * scale, scale, scale, 1)
                cx += CJK_SIZE * scale

    def mixed(self, x, y, s, invert=False):
        """Mirror mixed_text(): ASCII gets a +2px offset inside a CJK row."""
        cx = x
        for ch in s:
            dy = 2 if ord(ch) < 0x80 else 0
            self.text(cx, y + dy, ch, 1, invert)
            cx += 8 if ord(ch) < 0x80 else CJK_SIZE

    def center(self, y, s, scale=1, invert=False):
        self.text(max(0, (W - self.text_w(s, scale)) // 2), y, s, scale, invert)

    def mixed_center(self, y, s, invert=False):
        self.mixed(max(0, (W - self.text_w(s)) // 2), y, s, invert)

    def mixed_right(self, xr, y, s, invert=False):
        self.mixed(xr - self.text_w(s), y, s, invert)

    def right(self, xr, y, s, scale=1, invert=False):
        self.text(xr - self.text_w(s, scale), y, s, scale, invert)

    def title_strip(self, s):
        """Mirror title_strip(): inverted 12px bar."""
        self.fill_rect(0, 0, W, 12, 1)
        self.mixed_center(0, s, True)

    def hline(self, y):
        self.fill_rect(0, y, W, 1, 1)

    def center_fit(self, y, s, maxw=W, max_scale=2, invert=False):
        sc = max_scale
        while sc > 1 and self.text_w(s, sc) > maxw:
            sc -= 1
        self.center(y, s, sc, invert)

    def wrap(self, x, y, maxw, s, scale=1, lh=12):
        """Mirror ui_text_wrap: break on spaces for Latin, per char for CJK."""
        line = 0
        i = 0
        while i < len(s):
            start = i
            w = 0
            last_space = None
            j = i
            while j < len(s):
                adv = (8 if ord(s[j]) < 0x80 else CJK_SIZE) * scale
                if w + adv > maxw:
                    break
                if s[j] == " ":
                    last_space = j
                w += adv
                j += 1
            if j == start:
                j = start + 1
            elif last_space is not None and j < len(s):
                j = last_space + 1
            self.text(x, y + line * lh, s[start:j].rstrip(), scale)
            line += 1
            i = j
            if i < len(s) and s[i] == " ":
                i += 1
        return line

    def art(self):
        lines = []
        for row in self.buf:
            lines.append("".join("#" if v else "." for v in row))
        return "\n".join(lines)


def show(title, fn):
    s = Screen()
    fn(s)
    print("=" * 60)
    print(title)
    print("=" * 60)
    print(s.art())
    print()


# --- the screens, mirroring ui.c ---------------------------------------------

def splash(s):
    s.mixed_center(4, "拓竹监视器")
    s.center(20, "BAMBU MONITOR")
    s.mixed_center(36, "正在启动")
    for i in range(3):
        s.fill_rect(64 + 24 + 6 + i * 6, 41, 3, 3, 1)


def draw_ip(s, y, ip, invert=False):
    """Mirror draw_ip() in ui.c: same scale-1 size as everything else."""
    s.center_fit(y, ip, W - 2, 1, invert)


def login_portal(s):
    s.title_strip("拓竹账号登录")
    s.mixed_center(14, "手机连同一WiFi")
    draw_ip(s, 28, "192.168.5.246")
    s.mixed_center(42, "浏览器打开上面地址")
    s.mixed_center(54, "登录后即可监控")


def portal(s):
    # no title strip (see screen_portal in ui.c): plain centered title
    s.mixed_center(0, "配网设置")
    s.mixed_center(13, "热点:A1MON-BFC4")
    s.center(26, "192.168.4.1")
    s.mixed_center(36, "手机连接上述热点")
    s.mixed_center(49, "浏览器打开地址")


def monitor(s):
    t = 0
    name = "雯雯的打印机"
    s.mixed(0, 0, name)
    s.fill_rect(s.text_w(name) + 3, 4, 5, 5, 1)
    s.mixed_right(W, 0, "打印中")
    s.hline(12)
    # temps row
    for x, label, val in ((0, "喷", 220), (62, "床", 45)):
        txt = "%s:%d" % (label, val)
        s.mixed(x, 13, txt)
        s.rect(x + s.text_w(txt) + 1, 16, 4, 4, 1)
    s.right(W, 15, "1:23")
    # big percent + layer
    s.text(0, 26, "47%", 2)
    s.mixed_right(W, 28, "层:12/30")
    # progress bar
    s.rect(0, 43, W, 8, 1)
    s.fill_rect(1, 45, int(126 * 47 / 100), 4, 1)
    # bottom row
    s.mixed(0, 52, "耗材:PLA")
    s.mixed_right(W, 52, "标准")


def monitor_idle(s):
    name = "雯雯的打印机"
    s.mixed(0, 0, name)
    s.mixed_right(W, 0, "空闲")
    s.hline(12)
    for x, label, val in ((0, "喷", 24), (62, "床", 23)):
        txt = "%s:%d" % (label, val)
        s.mixed(x, 13, txt)
        s.rect(x + s.text_w(txt) + 1, 16, 4, 4, 1)
    s.right(W, 15, "-56dB")
    s.text(0, 26, "--", 2)
    s.rect(0, 43, W, 8, 1)
    s.mixed(0, 52, "耗材:PLA")
    s.mixed_right(W, 52, "标准")


def monitor_error(s):
    name = "雯雯的打印机"
    s.mixed(0, 0, name)
    s.mixed_right(W, 0, "失败")
    s.hline(12)
    for x, label, val in ((0, "喷", 220), (62, "床", 45)):
        txt = "%s:%d" % (label, val)
        s.mixed(x, 13, txt)
    s.right(W, 15, "1:23")
    s.text(0, 26, "47%", 2)
    s.mixed_right(W, 28, "层:12/30")
    s.rect(0, 43, W, 8, 1)
    s.fill_rect(1, 45, int(126 * 47 / 100), 4, 1)
    line = "错误 080012C4"
    s.fill_rect(0, 52, s.text_w(line) + 6, 12, 1)
    s.mixed(3, 52, line, True)


def wifi_conn(s):
    s.title_strip("WiFi")
    s.mixed_center(20, "正在连接")


def portal_wait(s):
    s.title_strip("! 登录失败 !")
    s.mixed_center(14, "请打开配网页面")
    s.mixed_center(30, "正在重启")


def error_screen(s):
    s.title_strip("! 出错了 !")
    s.wrap(0, 14, W, "MQTT CONNECTION REFUSED BY BROKER", 1, 12)
    s.mixed_center(50, "正在重试")


if __name__ == "__main__":
    show("splash / 启动", splash)
    show("monitor printing / 监控-打印中", monitor)
    show("monitor idle / 监控-空闲", monitor_idle)
    show("monitor error / 监控-错误", monitor_error)
    show("login portal / 登录门户", login_portal)
    show("portal / 配网", portal)
    show("wifi connecting / 连接中", wifi_conn)
    show("portal wait / 配网等待", portal_wait)
    show("error / 出错", error_screen)
