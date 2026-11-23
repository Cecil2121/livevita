#ifndef UI_H
#define UI_H
/* LiveVita: drawing helpers, colours, input (buttons, external pads, touch) and the on-screen keyboard. */
#include <stddef.h>
#include <stdint.h>
#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* classic purple palette */
#define C_PURPLE   RGBA8(100, 65, 165, 255)   /* #6441A5 headers */
#define C_PURPLE_D RGBA8(75, 54, 124, 255)
#define C_PANEL    RGBA8(44, 37, 65, 255)     /* #2C2541 panels */
#define C_PANEL_L  RGBA8(62, 53, 92, 255)
#define C_BG       RGBA8(25, 23, 28, 255)     /* #19171C background */
#define C_TEXT     RGBA8(240, 238, 245, 255)
#define C_DIM      RGBA8(165, 160, 180, 255)
#define C_FAINT    RGBA8(110, 104, 128, 255)
#define C_LIVE     RGBA8(233, 25, 22, 255)
#define C_WARN     RGBA8(255, 190, 80, 255)
#define C_SHADE    RGBA8(0, 0, 0, 175)
#define C_SEL      RGBA8(185, 163, 227, 255)  /* selection outline */
#define C_WHITE    RGBA8(255, 255, 255, 255)

/* ---- input ---- */
typedef struct {
    uint32_t held, press;               /* buttons (SCE_CTRL_*; L1/R1 of a DualShock count as L/R) */
    int tap, tap_x, tap_y;              /* a front-touch tap ended this frame (screen pixels) */
    int touching, touch_x, touch_y;
    int drag_dy;                        /* vertical drag this frame (pixels, + = finger moved down) */
} Input;

void input_init(void);
void input_read(Input *in, int keyboard_open);
/* the console: 1 if a PS TV; *ext_pad = a DualShock 3 / 4 is connected */
int  detect_pstv(int *ext_pad);

/* ---- drawing ---- */
void ui_set_font(vita2d_pgf *f);
void text(int x, int y, unsigned color, float scale, const char *s);       /* y = baseline */
int  text_w(float scale, const char *s);
void text_right(int x_right, int y, unsigned color, float scale, const char *s);
void text_center_in(int x, int w, int y, unsigned color, float scale, const char *s);
/* copies s cut at a character boundary, with "...", to fit max_w pixels */
void fit(char *out, size_t cap, const char *s, float scale, int max_w);
void rect(int x, int y, int w, int h, unsigned color);
void outline(int x, int y, int w, int h, int t, unsigned color);
void spinner(int cx, int cy, unsigned color);
void fmt_viewers(char *out, size_t cap, int v);
/* "2h 14m" since an ISO time ("2026-11-22T18:00:00Z"); "" if unknown */
void fmt_uptime(char *out, size_t cap, const char *started_at);
/* word-wrapped text from (x, y baseline), at most max_lines lines of line_h; returns the lines drawn */
int  draw_wrapped(int x, int y, int w, float scale, unsigned color, const char *s, int max_lines, int line_h);
/* "Today", "One day ago", "3 days ago", "2 weeks ago" from an ISO time */
void fmt_ago(char *out, size_t cap, const char *iso);
/* "Joined March 2016" from an ISO time ("" if unknown) */
void fmt_joined(char *out, size_t cap, const char *iso);
/* 711825 -> "711,825" */
void fmt_count(char *out, size_t cap, long v);
/* Twitch's "3h10m6s" -> "03:10:06" */
void fmt_duration(char *out, size_t cap, const char *tw);
/* the app's own wordmark */
void draw_wordmark(int x, int y, float scale);
/* simple drawn icons, about size x size */
enum { ICON_SEARCH, ICON_HEART, ICON_GRID, ICON_GEAR, ICON_STAR, ICON_AUDIO, ICON_FILM, ICON_INFO, ICON_CLOSE };
void draw_icon(int icon, int x, int y, int size, unsigned color);
/* a picture scaled to fill w x h (or a plain panel while it loads) */
void draw_picture(vita2d_texture *t, int x, int y, int w, int h);
/* 0xRRGGBB -> a colour readable on the dark background (name colours) */
unsigned readable_color(uint32_t rgb, const char *name_for_default);

/* ---- on-screen keyboard ---- */
enum { IME_NONE, IME_SEARCH, IME_CHAT };
int  ime_open(int purpose, const char *title, const char *initial, int max_len);   /* 0 = opened */
int  ime_active(void);
/* when the keyboard closed with Enter: its purpose (text in out), else IME_NONE */
int  ime_poll(char *out, size_t cap);

#endif
