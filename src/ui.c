#include "ui.h"
#include "applog.h"
#include <psp2/ctrl.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vita2d_pgf *font;
void ui_set_font(vita2d_pgf *f) { font = f; }

static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* ---------------------------------------------------------------- input */

static int g_pstv, g_ext;
static uint32_t g_prev;
static unsigned g_rep_at;
static int g_was_touching, g_t0x, g_t0y, g_lastx, g_lasty, g_moved;
static unsigned g_t0;

int detect_pstv(int *ext_pad)
{
    int tv = sceKernelIsPSVitaTV() == 1;
    SceCtrlPortInfo pi;
    int ext = 0;
    memset(&pi, 0, sizeof pi);
    if (sceCtrlGetControllerPortInfo(&pi) >= 0)
        for (int p = 1; p < 5; p++) if (pi.port[p] == SCE_CTRL_TYPE_DS3 || pi.port[p] == SCE_CTRL_TYPE_DS4) ext = 1;
    if (!tv && pi.port[0] == SCE_CTRL_TYPE_VIRT) tv = 1;    /* (a PS TV's port 0 is virtual) */
    g_pstv = tv;
    g_ext = ext;
    if (ext_pad) *ext_pad = ext;
    plog("hardware: %s, ports %d %d %d %d %d", tv ? "PS TV" : "PS Vita", pi.port[0], pi.port[1], pi.port[2], pi.port[3], pi.port[4]);
    return tv;
}

void input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

void input_read(Input *in, int keyboard_open)
{
    memset(in, 0, sizeof *in);
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t held = pad.buttons;
    int ly = pad.ly, lx = pad.lx;
    if (g_pstv || g_ext) {                                   /* DualShocks on ports 1-4 */
        for (int p = 1; p < 5; p++) {
            SceCtrlData e;
            memset(&e, 0, sizeof e);
            if (sceCtrlPeekBufferPositiveExt2(p, &e, 1) < 0) continue;
            uint32_t b = e.buttons;
            if (b & SCE_CTRL_L1) b |= SCE_CTRL_LTRIGGER;
            if (b & SCE_CTRL_R1) b |= SCE_CTRL_RTRIGGER;
            held |= b & ~(SCE_CTRL_L1 | SCE_CTRL_R1);
            if (e.ly < 40 || e.ly > 215) ly = e.ly;
            if (e.lx < 40 || e.lx > 215) lx = e.lx;
        }
    }
    if (ly < 40) held |= SCE_CTRL_UP;                        /* left stick as the d-pad */
    if (ly > 215) held |= SCE_CTRL_DOWN;
    if (lx < 40) held |= SCE_CTRL_LEFT;
    if (lx > 215) held |= SCE_CTRL_RIGHT;
    held &= ~SCE_CTRL_INTERCEPTED;
    if (keyboard_open) { g_prev = held; return; }
    in->held = held;
    in->press = held & ~g_prev;
    g_prev = held;
    const uint32_t DIRS = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
    if (held & DIRS) {                                       /* holding a direction repeats */
        if (in->press & DIRS) g_rep_at = now_ms() + 380;
        else if (now_ms() >= g_rep_at) { in->press |= held & DIRS; g_rep_at = now_ms() + 85; }
    }
    if (g_pstv) return;
    SceTouchData td;
    memset(&td, 0, sizeof td);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 0) td.reportNum = 0;
    if (td.reportNum > 0) {
        int x = td.report[0].x / 2, y = td.report[0].y / 2;  /* (the panel is 1920 x 1088) */
        if (!g_was_touching) { g_t0x = g_lastx = x; g_t0y = g_lasty = y; g_t0 = now_ms(); g_moved = 0; }
        if (abs(x - g_t0x) > 18 || abs(y - g_t0y) > 18) g_moved = 1;
        in->drag_dy = y - g_lasty;
        g_lastx = x;
        g_lasty = y;
        in->touching = 1;
        in->touch_x = x;
        in->touch_y = y;
        g_was_touching = 1;
    } else if (g_was_touching) {
        g_was_touching = 0;
        if (!g_moved && now_ms() - g_t0 < 700) { in->tap = 1; in->tap_x = g_lastx; in->tap_y = g_lasty; }
    }
}

/* ---------------------------------------------------------------- text */

void text(int x, int y, unsigned color, float scale, const char *s) { vita2d_pgf_draw_text(font, x, y, color, scale, s); }
int text_w(float scale, const char *s) { return vita2d_pgf_text_width(font, scale, s); }
void text_right(int xr, int y, unsigned color, float scale, const char *s) { text(xr - text_w(scale, s), y, color, scale, s); }
void text_center_in(int x, int w, int y, unsigned color, float scale, const char *s) { text(x + (w - text_w(scale, s)) / 2, y, color, scale, s); }

void fit(char *out, size_t cap, const char *s, float scale, int max_w)
{
    snprintf(out, cap, "%s", s);
    if (max_w <= 0) { out[0] = 0; return; }
    if (text_w(scale, out) <= max_w) return;
    size_t n = strlen(out);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
        if (n + 4 > cap) continue;
        memcpy(out + n, "...", 4);
        if (text_w(scale, out) <= max_w) return;
    }
}

void rect(int x, int y, int w, int h, unsigned color) { if (w > 0 && h > 0) vita2d_draw_rectangle(x, y, w, h, color); }

void outline(int x, int y, int w, int h, int t, unsigned color)
{
    rect(x - t, y - t, w + 2 * t, t, color);
    rect(x - t, y + h, w + 2 * t, t, color);
    rect(x - t, y, t, h, color);
    rect(x + w, y, t, h, color);
}

void spinner(int cx, int cy, unsigned color)
{
    float a0 = (float)(now_ms() % 1000) / 1000.0f * 6.2831853f;
    for (int i = 0; i < 8; i++) {
        float a = a0 + (float)i * 0.785398f;
        unsigned al = (unsigned)(60 + i * 24);
        vita2d_draw_fill_circle(cx + cosf(a) * 14, cy + sinf(a) * 14, 3, (color & 0x00FFFFFF) | (al << 24));
    }
}

void fmt_viewers(char *out, size_t cap, int v)
{
    if (v < 0) snprintf(out, cap, "%s", "");
    else if (v >= 1000000) snprintf(out, cap, "%d.%dM", v / 1000000, (v / 100000) % 10);
    else if (v >= 10000) snprintf(out, cap, "%dK", v / 1000);
    else if (v >= 1000) snprintf(out, cap, "%d.%dK", v / 1000, (v / 100) % 10);
    else snprintf(out, cap, "%d", v);
}

static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

void fmt_uptime(char *out, size_t cap, const char *iso)
{
    out[0] = 0;
    int Y, M, D, h, m, s;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return;
    SceDateTime now;
    if (sceRtcGetCurrentClockUtc(&now) < 0) return;
    long long t0 = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + s;
    long long t1 = days_from_civil(now.year, now.month, now.day) * 86400 + now.hour * 3600 + now.minute * 60 + now.second;
    long long d = t1 - t0;
    if (d < 0 || d > 30LL * 86400) return;
    if (d >= 3600) snprintf(out, cap, "%lldh %02lldm", d / 3600, (d / 60) % 60);
    else snprintf(out, cap, "%lldm", d / 60);
}

int draw_wrapped(int x, int y, int w, float scale, unsigned color, const char *s, int max_lines, int line_h)
{
    char line[400];
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        while (*p == ' ' || *p == '\n') p++;
        if (!*p) break;
        /* as many words as fit */
        size_t best = 0, i = 0;
        for (;;) {
            size_t j = i;
            while (p[j] && p[j] != ' ' && p[j] != '\n') j++;
            if (j >= sizeof line) break;
            memcpy(line, p, j);
            line[j] = 0;
            if (text_w(scale, line) > w && best) break;
            best = j;
            if (!p[j] || p[j] == '\n') break;
            i = j + 1;
        }
        if (!best) best = strcspn(p, " \n");
        if (best >= sizeof line) best = sizeof line - 1;
        memcpy(line, p, best);
        line[best] = 0;
        if (lines == max_lines - 1 && p[best]) {             /* the last line: "..." when more follows */
            char f[400];
            snprintf(f, sizeof f, "%s ...", line);
            fit(line, sizeof line, f, scale, w);
        } else if (text_w(scale, line) > w) { char f[400]; fit(f, sizeof f, line, scale, w); snprintf(line, sizeof line, "%s", f); }
        text(x, y + lines * line_h, color, scale, line);
        lines++;
        p += best;
    }
    return lines;
}

void fmt_ago(char *out, size_t cap, const char *iso)
{
    out[0] = 0;
    int Y, M, D, h, m, s;
    if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return;
    SceDateTime now;
    if (sceRtcGetCurrentClockUtc(&now) < 0) return;
    long long t0 = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 + h * 3600 + m * 60 + s;
    long long t1 = days_from_civil(now.year, now.month, now.day) * 86400 + now.hour * 3600 + now.minute * 60 + now.second;
    long long d = (t1 - t0) / 86400;
    if (d <= 0) snprintf(out, cap, "Today");
    else if (d == 1) snprintf(out, cap, "One day ago");
    else if (d < 14) snprintf(out, cap, "%lld days ago", d);
    else if (d < 60) snprintf(out, cap, "%lld weeks ago", d / 7);
    else snprintf(out, cap, "%lld months ago", d / 30);
}

void fmt_joined(char *out, size_t cap, const char *iso)
{
    static const char *const MON[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                         "September", "October", "November", "December" };
    int Y, M;
    out[0] = 0;
    if (iso && sscanf(iso, "%d-%d", &Y, &M) == 2 && M >= 1 && M <= 12) snprintf(out, cap, "Joined %s %d", MON[M - 1], Y);
}

void fmt_count(char *out, size_t cap, long v)
{
    char d[32];
    snprintf(d, sizeof d, "%ld", v < 0 ? 0 : v);
    size_t n = strlen(d), o = 0;
    for (size_t i = 0; i < n && o + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

void fmt_duration(char *out, size_t cap, const char *tw)
{
    int h = 0, m = 0, s = 0, v = 0;
    for (const char *p = tw; p && *p; p++) {
        if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0');
        else { if (*p == 'h') h = v; else if (*p == 'm') m = v; else if (*p == 's') s = v; v = 0; }
    }
    snprintf(out, cap, "%02d:%02d:%02d", h, m, s);
}

/* ---------------------------------------------------------------- pictures */

void draw_wordmark(int x, int y, float scale)
{
    /* a small screen-with-play-button mark, then the name (the app's own design) */
    int s = (int)(22 * scale), w = (int)(30 * scale);
    rect(x, y - s, w, s, C_WHITE);
    rect(x + 2, y - s + 2, w - 4, s - 4, C_PURPLE_D);
    vita2d_draw_line(x + w * 0.38f, y - s * 0.75f, x + w * 0.38f, y - s * 0.25f, C_WHITE);
    for (int i = 0; i < (int)(s * 0.5f); i++)
        vita2d_draw_line(x + w * 0.38f, y - s * 0.75f + i * 0.5f, x + w * 0.38f + i * 0.9f, y - s * 0.5f, C_WHITE),
        vita2d_draw_line(x + w * 0.38f, y - s * 0.25f - i * 0.5f, x + w * 0.38f + i * 0.9f, y - s * 0.5f, C_WHITE);
    text(x + w + (int)(8 * scale), y - 1, C_WHITE, 1.25f * scale, "LiveVita");
}

void draw_icon(int icon, int x, int y, int sz, unsigned c)
{
    float s = (float)sz;
    switch (icon) {
    case ICON_SEARCH:
        for (int r = (int)(s * 0.32f); r > (int)(s * 0.32f) - 3; r--) {
            for (int k = 0; k < 24; k++) {
                float a = (float)k * 0.2618f, b = a + 0.2618f;
                vita2d_draw_line(x + s * 0.4f + cosf(a) * r, y + s * 0.4f + sinf(a) * r, x + s * 0.4f + cosf(b) * r, y + s * 0.4f + sinf(b) * r, c);
            }
        }
        for (int t = -1; t <= 1; t++) vita2d_draw_line(x + s * 0.62f + t, y + s * 0.62f, x + s * 0.92f + t, y + s * 0.92f, c);
        break;
    case ICON_HEART:
        vita2d_draw_fill_circle(x + s * 0.3f, y + s * 0.35f, s * 0.22f, c);
        vita2d_draw_fill_circle(x + s * 0.7f, y + s * 0.35f, s * 0.22f, c);
        for (int i = 0; i < (int)(s * 0.55f); i++) {
            float yy = y + s * 0.4f + i, half = s * 0.46f * (1.0f - (float)i / (s * 0.55f));
            vita2d_draw_line(x + s * 0.5f - half, yy, x + s * 0.5f + half, yy, c);
        }
        break;
    case ICON_GRID:
        for (int r = 0; r < 2; r++) for (int k = 0; k < 2; k++) rect(x + (int)(s * (0.08f + 0.48f * k)), y + (int)(s * (0.08f + 0.48f * r)), (int)(s * 0.36f), (int)(s * 0.36f), c);
        break;
    case ICON_GEAR:
        vita2d_draw_fill_circle(x + s * 0.5f, y + s * 0.5f, s * 0.3f, c);
        for (int k = 0; k < 8; k++) {
            float a = (float)k * 0.785398f;
            vita2d_draw_fill_circle(x + s * 0.5f + cosf(a) * s * 0.36f, y + s * 0.5f + sinf(a) * s * 0.36f, s * 0.09f, c);
        }
        vita2d_draw_fill_circle(x + s * 0.5f, y + s * 0.5f, s * 0.12f, C_BG);
        break;
    case ICON_STAR:
        for (int k = 0; k < 5; k++) {
            float a = -1.5708f + (float)k * 1.2566f;
            for (int t = 0; t < 3; t++)
                vita2d_draw_line(x + s * 0.5f, y + s * 0.55f, x + s * 0.5f + cosf(a) * s * 0.48f + t, y + s * 0.55f + sinf(a) * s * 0.48f, c);
        }
        vita2d_draw_fill_circle(x + s * 0.5f, y + s * 0.55f, s * 0.2f, c);
        break;
    case ICON_FILM:                                          /* a strip of film */
        rect(x, y + (int)(s * 0.15f), sz, (int)(s * 0.7f), c);
        for (int k = 0; k < 5; k++) {
            int hx = x + (int)(s * (0.06f + k * 0.19f));
            rect(hx, y + (int)(s * 0.2f), (int)(s * 0.1f), (int)(s * 0.1f), C_BG);
            rect(hx, y + (int)(s * 0.7f), (int)(s * 0.1f), (int)(s * 0.1f), C_BG);
        }
        rect(x + (int)(s * 0.08f), y + (int)(s * 0.36f), (int)(s * 0.38f), (int)(s * 0.28f), C_BG);
        rect(x + (int)(s * 0.54f), y + (int)(s * 0.36f), (int)(s * 0.38f), (int)(s * 0.28f), C_BG);
        break;
    case ICON_INFO:
        rect(x + (int)(s * 0.38f), y, (int)(s * 0.24f), (int)(s * 0.22f), c);
        rect(x + (int)(s * 0.38f), y + (int)(s * 0.32f), (int)(s * 0.24f), (int)(s * 0.6f), c);
        rect(x + (int)(s * 0.26f), y + (int)(s * 0.32f), (int)(s * 0.14f), (int)(s * 0.1f), c);
        rect(x + (int)(s * 0.26f), y + (int)(s * 0.86f), (int)(s * 0.48f), (int)(s * 0.14f), c);
        break;
    case ICON_CLOSE:
        for (int t = -2; t <= 2; t++) {
            vita2d_draw_line(x + t, y, x + sz + t, y + sz, c);
            vita2d_draw_line(x + sz + t, y, x + t, y + sz, c);
        }
        break;
    case ICON_AUDIO:
        for (int k = 0; k < 5; k++) {
            int h = (int)(s * (0.3f + 0.6f * fabsf(sinf((float)(now_ms() % 2000) / 318.0f + k))));
            rect(x + (int)(s * 0.1f) + k * (int)(s * 0.17f), y + sz - h, (int)(s * 0.11f), h, c);
        }
        break;
    }
}

void draw_picture(vita2d_texture *t, int x, int y, int w, int h)
{
    if (!t) { rect(x, y, w, h, C_PANEL_L); return; }
    float tw = (float)vita2d_texture_get_width(t), th = (float)vita2d_texture_get_height(t);
    if (tw <= 0 || th <= 0) return;
    vita2d_draw_texture_scale(t, x, y, w / tw, h / th);
}

unsigned readable_color(uint32_t rgb, const char *name)
{
    static const uint32_t DEF[] = { 0xFF4A4A, 0x5A7FFF, 0x3CD070, 0xB22222, 0xFF7F50, 0x9ACD32, 0xFF6347, 0x2E8B57,
                                    0xDAA520, 0xD2691E, 0x5F9EA0, 0x1E90FF, 0xFF69B4, 0x8A2BE2, 0x00FF7F };
    if (!rgb) {                                              /* no colour chosen: one from the name, as chat clients do */
        unsigned h = 0;
        for (const char *p = name; p && *p; p++) h = h * 31 + (unsigned char)*p;
        rgb = DEF[h % (sizeof DEF / sizeof DEF[0])];
    }
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int lum = (r * 299 + g * 587 + b * 114) / 1000;
    if (lum < 110) {                                         /* too dark on the dark panel: lighten */
        r = r + (255 - r) * 45 / 100; g = g + (255 - g) * 45 / 100; b = b + (255 - b) * 45 / 100;
    }
    return RGBA8(r, g, b, 255);
}

/* ---------------------------------------------------------------- keyboard */

static SceWChar16 ime_title[64], ime_init[260], ime_buf[260];
static int g_ime_purpose = IME_NONE;

static void utf8_to_utf16(const char *s, uint16_t *out, size_t cap)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o + 1 < cap) {
        unsigned cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = (unsigned)(*p & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = (unsigned)(*p & 0x0F) << 12 | (unsigned)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3] && o + 2 < cap) {
            cp = (unsigned)(*p & 0x07) << 18 | (unsigned)(p[1] & 0x3F) << 12 | (unsigned)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            continue;
        } else { p++; continue; }
        out[o++] = (uint16_t)cp;
    }
    out[o] = 0;
}

static void utf16_to_utf8(const uint16_t *s, char *out, size_t cap)
{
    size_t o = 0;
    for (; *s; s++) {
        unsigned c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) {   /* emoji etc. */
            c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
            s++;
            if (o + 4 >= cap) break;
            out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | ((c >> 12) & 63));
            out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
        } else if (c < 0x80) { if (o + 1 >= cap) break; out[o++] = (char)c; }
        else if (c < 0x800) { if (o + 2 >= cap) break; out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else { if (o + 3 >= cap) break; out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

int ime_open(int purpose, const char *title, const char *initial, int max_len)
{
    if (g_ime_purpose != IME_NONE) return -1;
    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    utf8_to_utf16(title, (uint16_t *)ime_title, 64);
    utf8_to_utf16(initial ? initial : "", (uint16_t *)ime_init, 260);
    memset(ime_buf, 0, sizeof ime_buf);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;         /* (others can be switched to on the keyboard) */
    p.languagesForced = SCE_FALSE;
    p.type = SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = ime_title;
    p.maxTextLength = (SceUInt32)(max_len < 250 ? max_len : 250);
    p.initialText = ime_init;
    p.inputTextBuffer = ime_buf;
    int r = sceImeDialogInit(&p);
    plog("ime: init -> 0x%08X", (unsigned)r);
    if (r < 0) return -1;
    g_ime_purpose = purpose;
    return 0;
}

int ime_active(void) { return g_ime_purpose != IME_NONE; }

int ime_poll(char *out, size_t cap)
{
    if (g_ime_purpose == IME_NONE || sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return IME_NONE;
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    int purpose = g_ime_purpose;
    g_ime_purpose = IME_NONE;
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return IME_NONE;
    utf16_to_utf8((const uint16_t *)ime_buf, out, cap);
    char *b = out, *e;
    while (*b == ' ') b++;
    e = b + strlen(b);
    while (e > b && e[-1] == ' ') *--e = 0;
    memmove(out, b, strlen(b) + 1);
    return out[0] ? purpose : IME_NONE;
}
