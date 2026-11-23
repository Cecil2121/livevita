/* LiveVita: the player screen.
 *  - Theater mode: video on the left (70%), stream info below it, live chat on the right (30%) with an
 *    input bar. Full screen: the video alone, info shown on any button. Square switches.
 *  - Quality menu (START), quality down / up (L / R), chat (Triangle or tap the input bar). */
#include "playerui.h"
#include "app.h"
#include "chat.h"
#include "imgcache.h"
#include "tpemote.h"
#include "ui.h"
#include "player/tsplayer.h"
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>

#define TH_VIDEO_W 672                  /* theater: 70% of the screen, 16:9 */
#define TH_VIDEO_H 378
#define CHAT_X TH_VIDEO_W
#define CHAT_W (SCREEN_W - TH_VIDEO_W)
#define CHAT_HEAD 32
#define CHAT_INPUT 44
#define CHAT_SCALE 0.72f
#define CHAT_LH 19
#define EMOTE_PX 22

static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* ---------------------------------------------------------------- chat rendering */

static int g_space_w = 4;

typedef struct {
    int x0, w, x, base, draw;           /* left edge, width, pen x, baseline of the current line */
    int lines;
} Pen;

static void pen_newline(Pen *p) { p->x = p->x0; p->base += CHAT_LH; p->lines++; }

/* one word of text in a colour (wraps; a word longer than the width is cut) */
static void pen_word(Pen *p, const char *w, size_t n, unsigned color)
{
    char buf[CHAT_TEXT_MAX];
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, w, n);
    buf[n] = 0;
    int ww = text_w(CHAT_SCALE, buf);
    if (p->x + ww > p->x0 + p->w && p->x > p->x0) pen_newline(p);
    if (ww > p->w) { char cut[CHAT_TEXT_MAX]; fit(cut, sizeof cut, buf, CHAT_SCALE, p->w); snprintf(buf, sizeof buf, "%s", cut); ww = p->w; }
    if (p->draw) text(p->x, p->base, color, CHAT_SCALE, buf);
    p->x += ww;
}

static void pen_space(Pen *p) { p->x += g_space_w; }

/* draws an emote image from a URL, sized like a native emote (shared by Twitch and third-party emotes) */
static void pen_emote_url(Pen *p, const char *url, const char *fallback, size_t fn)
{
    char name[64];
    size_t k = fn < sizeof name - 1 ? fn : sizeof name - 1;
    memcpy(name, fallback, k);
    name[k] = 0;
    int tw = text_w(CHAT_SCALE, name), need = tw > EMOTE_PX ? tw : EMOTE_PX;
    if (p->x + need > p->x0 + p->w && p->x > p->x0) pen_newline(p);
    if (p->draw) {
        vita2d_texture *t = img_get(url);
        if (t) draw_picture(t, p->x + (need - EMOTE_PX) / 2, p->base - EMOTE_PX + 5, EMOTE_PX, EMOTE_PX);
        else text(p->x, p->base, C_DIM, CHAT_SCALE, name);    /* (its name until the picture is here) */
    }
    p->x += need;
}

/* text split into words; a word that is a third-party emote (BTTV / FFZ) is drawn as its image */
static void pen_text(Pen *p, const char *s, size_t n, unsigned color)
{
    size_t i = 0;
    while (i < n) {
        if (s[i] == ' ') { pen_space(p); i++; continue; }
        size_t j = i;
        while (j < n && s[j] != ' ') j++;
        size_t wl = j - i;
        char url[172];
        if (wl >= 2 && wl < 40) {                            /* could be an emote name */
            char word[40];
            memcpy(word, s + i, wl);
            word[wl] = 0;
            if (tpemote_lookup(word, url, sizeof url)) { pen_emote_url(p, url, word, wl); i = j; continue; }
        }
        pen_word(p, s + i, wl, color);
        i = j;
    }
}

static void pen_emote(Pen *p, const ChatEmote *e, const char *fallback, size_t fn)
{
    /* Twitch native emote: its static PNG from the v2 CDN */
    char url[160];
    snprintf(url, sizeof url, "https://static-cdn.jtvnw.net/emoticons/v2/%s/static/dark/1.0", e->id);
    pen_emote_url(p, url, fallback, fn);
}

static void pen_badges(Pen *p, uint8_t b)
{
    static const struct { uint8_t bit; char c; unsigned col; } B[] = {
        { CB_BROADCASTER, 'B', RGBA8(233, 25, 22, 255) }, { CB_STAFF, 'S', RGBA8(40, 40, 40, 255) },
        { CB_MOD, 'M', RGBA8(0, 173, 3, 255) },           { CB_VIP, 'V', RGBA8(224, 5, 185, 255) },
        { CB_PARTNER, 'P', RGBA8(145, 70, 255, 255) },    { CB_SUB, '*', RGBA8(100, 65, 165, 255) },
        { CB_TURBO, 'T', RGBA8(55, 104, 255, 255) },
    };
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        if (!(b & B[i].bit)) continue;
        if (p->x + 18 > p->x0 + p->w) pen_newline(p);
        if (p->draw) {
            rect(p->x, p->base - 14, 16, 16, B[i].col);
            char s[2] = { B[i].c, 0 };
            text_center_in(p->x, 16, p->base - 1, C_WHITE, 0.55f, s);
        }
        p->x += 19;
    }
}

/* lays a message out at (x, y) in width w; draws it if draw; returns its height */
static int msg_layout(const ChatMsg *m, int x, int y, int w, int draw)
{
    Pen p = { x, w, x, y + CHAT_LH - 5, draw, 1 };
    if (m->kind == CK_SYSTEM) {
        pen_text(&p, m->text, strlen(m->text), C_FAINT);
        return p.lines * CHAT_LH + 4;
    }
    pen_word(&p, m->time, strlen(m->time), C_FAINT);
    pen_space(&p);
    pen_badges(&p, m->badges);
    unsigned nc = readable_color(m->color, m->user);
    pen_word(&p, m->name, strlen(m->name), nc);
    if (m->kind != CK_ACTION) pen_word(&p, ":", 1, C_TEXT);
    pen_space(&p);
    if (m->deleted) { pen_text(&p, "<message deleted>", 17, C_FAINT); return p.lines * CHAT_LH + 4; }
    unsigned tc = m->kind == CK_ACTION ? nc : C_TEXT;
    size_t pos = 0, len = strlen(m->text);
    for (int e = 0; e < m->nemotes; e++) {
        const ChatEmote *em = &m->em[e];
        if (em->start < pos || em->end > len) continue;
        pen_text(&p, m->text + pos, em->start - pos, tc);
        pen_emote(&p, em, m->text + em->start, em->end - em->start);
        pos = em->end;
    }
    pen_text(&p, m->text + pos, len - pos, tc);
    return p.lines * CHAT_LH + 4;
}

static void draw_chat_panel(int x, int y, int w, int h)
{
    g_space_w = text_w(CHAT_SCALE, "a b") - text_w(CHAT_SCALE, "ab");
    if (g_space_w < 3) g_space_w = 4;
    rect(x, y, w, h, C_PANEL);
    rect(x, y, 2, h, C_PURPLE_D);
    /* header */
    rect(x, y, w, CHAT_HEAD, C_PURPLE_D);
    text(x + 12, y + 22, C_WHITE, 0.8f, "STREAM CHAT");
    const char *st = !app.chat_on ? "off" : chat_state() == CHAT_JOINED ? (chat_signed_in() ? "" : "reading only")
                   : chat_state() == CHAT_WAITING ? "reconnecting" : "connecting";
    if (st[0]) text_right(x + w - 10, y + 21, C_DIM, 0.65f, st);
    /* messages, newest at the bottom */
    int my = y + CHAT_HEAD + 4, mh = h - CHAT_HEAD - CHAT_INPUT - 8, mw = w - 20, mx = x + 10;
    if (!app.chat_on) {
        text(mx, my + 30, C_DIM, 0.75f, "Chat is turned off.");
        text(mx, my + 52, C_FAINT, 0.7f, "Turn it on in Settings.");
    } else {
        chat_lock();
        int n = chat_count();
        if (app.chat_scroll > n - 1) app.chat_scroll = n > 0 ? n - 1 : 0;
        int last = n - 1 - app.chat_scroll, first = last + 1, used = 0;
        static int hgt[CHAT_MAX_MSGS];
        while (first > 0 && used < mh) { first--; hgt[first % CHAT_MAX_MSGS] = msg_layout(chat_msg(first), mx, 0, mw, 0); used += hgt[first % CHAT_MAX_MSGS]; }
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(x, my, x + w, my + mh);
        int cy = my + mh - used;
        for (int i = first; i <= last; i++) { msg_layout(chat_msg(i), mx, cy, mw, 1); cy += hgt[i % CHAT_MAX_MSGS]; }
        vita2d_disable_clipping();
        chat_unlock();
        if (!n) text(mx, my + 30, C_FAINT, 0.72f, chat_state() == CHAT_JOINED ? "Waiting for messages..." : "Connecting to chat...");
        if (app.chat_scroll > 0) {
            rect(x + 8, my + mh - 24, w - 16, 24, C_PURPLE);
            text_center_in(x + 8, w - 16, my + mh - 7, C_WHITE, 0.65f, "Older messages - Down: back to live");
        }
    }
    /* input bar */
    int iy = y + h - CHAT_INPUT;
    rect(x + 8, iy + 6, w - 16, CHAT_INPUT - 12, C_BG);
    outline(x + 8, iy + 6, w - 16, CHAT_INPUT - 12, 1, C_PANEL_L);
    const char *ph = !app.chat_on ? "Chat is off" : !tw_can_chat() ? "Sign in again to chat (Settings)"
                   : chat_signed_in() ? "Send a message  (Triangle)" : "Connecting...";
    char f[100];
    fit(f, sizeof f, ph, 0.72f, w - 36);
    text(x + 18, iy + 28, C_FAINT, 0.72f, f);
}

/* ---------------------------------------------------------------- video and info */

static void draw_video_in(int x, int y, int w, int h)
{
    rect(x, y, w, h, RGBA8(0, 0, 0, 255));
    const TspStatus *st = tsp_status();
    int vw = 0, vh = 0;
    vita2d_texture *tx = tsp_frame(&vw, &vh);
    int audio_only = app.quality == TWQ_AUDIO || (st && st->audio_only);
    if (audio_only) {
        draw_icon(ICON_AUDIO, x + w / 2 - 40, y + h / 2 - 60, 80, C_SEL);
        text_center_in(x, w, y + h / 2 + 50, C_TEXT, 1.0f, "Audio only");
    } else if (tx && vw > 0 && vh > 0) {
        float s = (float)w / vw;
        if (vh * s > h) s = (float)h / vh;
        vita2d_draw_texture_part_scale(tx, x + (w - vw * s) / 2, y + (h - vh * s) / 2, 0, 0, vw, vh, s, s);
    }
    const char *note = NULL;
    char buf[160];
    if (!st || st->state == TSP_CONNECTING) note = "Connecting...";
    else if (!audio_only && (st->state == TSP_WAIT_KEY || (!tx && st->state == TSP_PLAYING))) note = "Loading video...";
    else if (st->state == TSP_ENDED) note = "The stream has ended.";
    else if (st->state == TSP_ERROR) { snprintf(buf, sizeof buf, "Playback error: %s", st->msg); note = buf; }
    else if (st->buffering) note = "Buffering...";
    if (note) {
        char f[160];
        fit(f, sizeof f, note, 0.9f, w - 60);
        int tw = text_w(0.9f, f);
        rect(x + (w - tw) / 2 - 18, y + h / 2 - 22, tw + 36, 38, C_SHADE);
        text_center_in(x, w, y + h / 2 + 4, st && st->state == TSP_ERROR ? C_WARN : C_TEXT, 0.9f, f);
    }
    if (st && st->ad_break && !audio_only) {                /* a stitched commercial break is playing */
        char ab[48];
        if (st->ad_left_ms > 0) snprintf(ab, sizeof ab, "Ad break   %ds", (st->ad_left_ms + 999) / 1000);
        else snprintf(ab, sizeof ab, "Ad break");
        int tw = text_w(0.75f, ab);
        int bx = x + 18, by = y + 18;
        rect(bx, by, tw + 30, 32, C_SHADE);
        rect(bx, by, 5, 32, C_PURPLE);                      /* purple accent bar */
        text(bx + 16, by + 22, C_TEXT, 0.75f, ab);
    }
}

static void info_lines(char *l1, size_t c1, char *l2, size_t c2)
{
    char v[24] = "", up[24] = "";
    fmt_viewers(v, sizeof v, app.playing.viewers);
    fmt_uptime(up, sizeof up, app.playing.started_at);
    snprintf(l1, c1, "%s%s%s", v[0] ? v : "", v[0] ? " viewers" : "", "");
    snprintf(l2, c2, "%s%s%s", up[0] ? "Live for " : "", up, "");
}

static void draw_info_panel(int x, int y, int w, int h)
{
    rect(x, y, w, h, C_BG);
    char line[220], l1[48], l2[48];
    int px = x + 16;
    rect(px, y + 14, 10, 10, C_LIVE);
    fit(line, sizeof line, app.playing.name, 1.05f, w - 220);
    text(px + 18, y + 26, C_TEXT, 1.05f, line);
    info_lines(l1, sizeof l1, l2, sizeof l2);
    char right[100];
    snprintf(right, sizeof right, "%s%s%s", l1, l1[0] && l2[0] ? "   " : "", l2);
    text_right(x + w - 16, y + 25, C_DIM, 0.75f, right);
    /* title: up to two lines */
    char t1[220], t2[220] = "";
    snprintf(t1, sizeof t1, "%s", app.playing.title);
    if (text_w(0.8f, t1) > w - 32) {                         /* break at a space that fits */
        size_t cut = strlen(t1);
        while (cut > 0) {
            while (cut > 0 && t1[cut] != ' ') cut--;
            char tmp[220];
            memcpy(tmp, t1, cut);
            tmp[cut] = 0;
            if (text_w(0.8f, tmp) <= w - 32 || cut == 0) break;
            cut--;
        }
        if (cut > 0) { fit(t2, sizeof t2, t1 + cut + 1, 0.8f, w - 32); t1[cut] = 0; }
        else fit(t1, sizeof t1, app.playing.title, 0.8f, w - 32);
    }
    text(px, y + 56, C_TEXT, 0.8f, t1);
    if (t2[0]) text(px, y + 78, C_TEXT, 0.8f, t2);
    char g[140];
    snprintf(line, sizeof line, "%s%s%s", app.playing.game[0] ? app.playing.game : "", app.playing.game[0] ? "   |   " : "", app.quality_label);
    fit(g, sizeof g, line, 0.75f, w - 32);
    text(px, y + (t2[0] ? 104 : 84), C_SEL, 0.75f, g);
    text(px, y + h - 10, C_FAINT, 0.68f, "O back   Square full screen   Triangle chat   L/R quality   START menu");
}

static void draw_quality_menu(void)
{
    int w = 360, h = 58 + TWQ_COUNT * 46, x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2;
    rect(0, 0, SCREEN_W, SCREEN_H, C_SHADE);
    rect(x, y, w, h, C_PANEL);
    rect(x, y, w, 44, C_PURPLE);
    text(x + 18, y + 30, C_WHITE, 0.95f, "Video quality");
    for (int i = 0; i < TWQ_COUNT; i++) {
        int ry = y + 52 + i * 46;
        if (i == app.qmenu_sel) rect(x + 8, ry, w - 16, 40, C_PANEL_L);
        text(x + 24, ry + 27, C_TEXT, 0.9f, TWQ_NAME[i]);
        if (i == app.quality) text_right(x + w - 24, ry + 27, C_SEL, 0.8f, "current");
    }
}

static void draw_stats(int x, int y)
{
    const TspStatus *st = tsp_status();
    if (!st) return;
    char l[200];
    snprintf(l, sizeof l, "%dx%d  %d.%d fps  net %u KB/s  shown %u  dropped %u  %s", st->width, st->height, st->fps10 / 10,
             st->fps10 % 10, st->net_kbs, (unsigned)st->shown, (unsigned)st->dropped, app.quality_label);
    rect(x, y, text_w(0.7f, l) + 16, 24, C_SHADE);
    text(x + 8, y + 17, C_TEXT, 0.7f, l);
}


/* ---------------------------------------------------------------- dialogs (shared with the pages) */

void dialog_draw(void)
{
    if (app.dialog == DLG_NONE) return;
    rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 110));
    int x = 222, y = 146, w = 492, h = 242;
    rect(x, y, w, h, RGBA8(28, 26, 30, 245));
    const char *nm = app.user_loaded ? app.user.name : app.playing.name;
    char msg[400];
    int buttons = 1;
    if (app.dialog == DLG_SIGNIN) {
        snprintf(msg, sizeof msg, "Log in with a free Twitch account to chat and follow.");
        buttons = 2;
    } else if (app.dialog == DLG_FOLLOW) {
        if (app.user_loaded && app.user.following == 1)
            snprintf(msg, sizeof msg, "You follow %s on your Twitch account.", nm);
        else if (app_chan_saved()) {
            snprintf(msg, sizeof msg, "You follow %s on this Vita. Unfollow?", nm);
            buttons = 3;
        } else {
            snprintf(msg, sizeof msg, "Follow %s on this Vita? They'll show in your Following list here. (Twitch doesn't let apps add follows to your account.)", nm);
            buttons = 3;
        }
    } else {
        snprintf(msg, sizeof msg, "Follow %s on twitch.tv or in the Twitch app: Twitch no longer lets other apps follow categories.", app.game_name);
    }
    draw_wrapped(x + 38, y + 50, w - 76, 0.95f, C_TEXT, msg, 4, 26);
    if (buttons >= 2) {                                       /* 2: Ok / Later, 3: Follow or Unfollow / Cancel */
        int bx = x + 40, by = y + 160, bw = 180, bh = 62;
        const char *a = buttons == 2 ? "Ok" : app_chan_saved() ? "Unfollow" : "Follow";
        rect(bx, by, bw, bh, C_PURPLE);
        text_center_in(bx, bw, by + 40, C_WHITE, 1.0f, a);
        rect(bx + bw, by, bw, bh, RGBA8(70, 68, 74, 255));
        text_center_in(bx + bw, bw, by + 40, C_WHITE, 1.0f, buttons == 2 ? "Later" : "Cancel");
        outline(app.dlg_sel == 0 ? bx : bx + bw, by, bw, bh, 2, C_WHITE);
    } else {
        int bx = x + w / 2 - 90, by = y + 160, bw = 180, bh = 62;
        rect(bx, by, bw, bh, C_PURPLE);
        text_center_in(bx, bw, by + 40, C_WHITE, 1.0f, "Ok");
        outline(bx, by, bw, bh, 2, C_WHITE);
    }
}

/* 1 if the dialog took the input */
int dialog_input(const Input *in)
{
    if (app.dialog == DLG_NONE) return 0;
    uint32_t p = in->press;
    int vita_follow = app.dialog == DLG_FOLLOW && !(app.user_loaded && app.user.following == 1);
    int two = app.dialog == DLG_SIGNIN || vita_follow;
    if (two && (p & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) app.dlg_sel = !app.dlg_sel;
    int ok = 0, later = 0;
    if (p & SCE_CTRL_CROSS) { if (two && app.dlg_sel == 1) later = 1; else ok = 1; }
    if (p & SCE_CTRL_CIRCLE) later = 1;
    if (in->tap) {                                           /* the buttons */
        int y = 146 + 160;
        if (in->tap_y >= y && in->tap_y < y + 62) {
            if (two) { if (in->tap_x >= 262 && in->tap_x < 442) ok = 1; else if (in->tap_x >= 442 && in->tap_x < 622) later = 1; }
            else if (in->tap_x >= 378 && in->tap_x < 558) ok = 1;
        }
    }
    if (ok) {
        int was = app.dialog;
        app.dialog = DLG_NONE;
        if (was == DLG_SIGNIN) { app_back_from_player(); app_login(); }
        else if (was == DLG_FOLLOW && vita_follow) app_toggle_chan_saved();
    } else if (later) app.dialog = DLG_NONE;
    return 1;
}

/* ---------------------------------------------------------------- handheld full screen: overlay and info panel */

static void draw_hh_overlay(void)
{
    /* a dark band behind the title at the top */
    for (int i = 0; i < 6; i++) rect(0, i * 16, SCREEN_W, 16, RGBA8(0, 0, 0, 150 - i * 22));
    char line[240], f[240];
    fit(f, sizeof f, app.playing.title[0] ? app.playing.title : app.playing.name, 1.15f, 700);
    text_center_in(0, SCREEN_W, 50, C_WHITE, 1.15f, f);
    if (app.playing_vod) {
        char d[24], ago[32];
        fmt_duration(d, sizeof d, app.vod.duration);
        fmt_ago(ago, sizeof ago, app.vod.created_at);
        snprintf(line, sizeof line, "%s  |  %s  |  %s", app.playing.name, ago, d);
        text_center_in(0, SCREEN_W, 84, C_DIM, 0.9f, line);
    } else {                                                 /* "playing <Game> for <N> viewers" */
        char v[24] = "";
        fmt_viewers(v, sizeof v, app.playing.viewers);
        const char *g = app.playing.game[0] ? app.playing.game : "";
        int w1 = text_w(0.9f, "playing "), w2 = text_w(0.9f, g), w3 = text_w(0.9f, " for "), w4 = text_w(0.9f, v), w5 = text_w(0.9f, " viewers");
        int x = (SCREEN_W - (w1 + w2 + w3 + w4 + w5)) / 2;
        text(x, 84, C_DIM, 0.9f, "playing "); x += w1;
        text(x, 84, C_WHITE, 0.9f, g); text(x + 1, 84, C_WHITE, 0.9f, g); x += w2;
        if (v[0]) {
            text(x, 84, C_DIM, 0.9f, " for "); x += w3;
            text(x, 84, C_WHITE, 0.9f, v); text(x + 1, 84, C_WHITE, 0.9f, v); x += w4;
            text(x, 84, C_DIM, 0.9f, " viewers");
        }
    }
    /* film (past broadcasts), heart (follow), i (channel info) */
    if (!app.playing_vod) { draw_icon(ICON_FILM, 34, 44, 46, C_WHITE); text(34, 108, C_FAINT, 0.6f, "Left"); }
    int followed = (app.user_loaded && app.user.following == 1) || app_chan_saved();
    draw_icon(ICON_HEART, 852, 40, 50, followed ? RGBA8(255, 90, 140, 255) : C_WHITE);
    text(856, 108, C_FAINT, 0.6f, "Right");
    draw_icon(ICON_INFO, 862, 466, 30, C_WHITE);
    text(858, 514, C_FAINT, 0.6f, "Down");
    rect(0, SCREEN_H - 26, 520, 26, RGBA8(0, 0, 0, 120));
    snprintf(line, sizeof line, "O back   Square video + chat   Triangle chat   START quality (%s)", app.quality_label);
    text(10, SCREEN_H - 8, C_FAINT, 0.62f, line);
}

static void draw_info_panel_hh(void)
{
    rect(0, 0, 312, SCREEN_H, RGBA8(20, 12, 34, 215));
    draw_icon(ICON_CLOSE, 26, 24, 30, C_SEL);
    draw_icon(ICON_GEAR, 252, 20, 42, C_WHITE);
    const TwUser *u = &app.user;
    int ax = 106, ay = 120;
    rect(ax - 4, ay - 4, 108, 108, C_WHITE);
    if (app.user_loaded && u->avatar[0]) draw_picture(img_get(u->avatar), ax, ay, 100, 100);
    else rect(ax, ay, 100, 100, C_PANEL_L);
    const char *desc = app.user_loaded && u->description[0] ? u->description : app.playing.name;
    if (!app.user_loaded) desc = app.signed_in ? "Loading the channel..." : "Log in to see the channel's info.";
    draw_wrapped(14, 270, 284, 0.82f, C_TEXT, desc, 6, 22);
    char fol[24] = "-", vw[24] = "-";
    if (app.user_loaded && u->followers >= 0) snprintf(fol, sizeof fol, "%ld", u->followers);
    if (!app.playing_vod && app.playing.viewers >= 0) snprintf(vw, sizeof vw, "%d", app.playing.viewers);
    text(40, 470, C_TEXT, 0.85f, "Followers");
    text(40, 496, C_TEXT, 1.0f, fol);
    rect(158, 458, 2, 44, C_DIM);
    text(176, 470, C_TEXT, 0.85f, "Viewers");
    text(176, 496, C_TEXT, 1.0f, vw);
}

static int hit_box(const Input *in, int x, int y, int w, int h)
{
    return in->tap && in->tap_x >= x && in->tap_x < x + w && in->tap_y >= y && in->tap_y < y + h;
}

static void heart(void)
{
    app.dialog = app.signed_in ? DLG_FOLLOW : DLG_SIGNIN;
    app.dlg_sel = 0;
}

/* ---- TV channel panel: description, followers, viewers + Follow / Clips (SELECT) ---- */

static const char *const TVP_BTN[] = { "Follow", "Clips", "Stats", "Close" };
#define TVP_N 4

static void draw_tv_panel(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 120));
    int w = 640, h = SCREEN_H;
    rect(0, 0, w, h, RGBA8(20, 18, 26, 240));
    rect(w, 0, 3, h, C_PURPLE_D);
    const TwUser *u = &app.user;
    if (app.user_loaded && u->avatar[0]) draw_picture(img_get(u->avatar), 40, 56, 120, 120);
    else rect(40, 56, 120, 120, C_PANEL_L);
    char f[300];
    fit(f, sizeof f, app.user_loaded ? u->name : app.playing.name, 1.4f, w - 200);
    text(180, 96, C_TEXT, 1.4f, f);
    int followed = (app.user_loaded && u->following == 1) || app_chan_saved();
    if (followed) text(180, 134, RGBA8(255, 120, 160, 255), 0.8f, (app.user_loaded && u->following == 1) ? "Followed on Twitch" : "Followed on this Vita");
    /* description */
    const char *desc = !app.user_loaded ? (app.signed_in ? "Loading channel info..." : "Log in to see channel info.")
                     : u->description[0] ? u->description : "No bio yet.";
    draw_wrapped(40, 210, w - 80, 0.85f, C_TEXT, desc, 4, 26);
    /* stats line: "1.2M followers      5,310 viewers" */
    char line[200], up[24] = "", vw[32] = "", fo[32] = "";
    if (app.user_loaded && u->followers >= 0) fmt_count(fo, sizeof fo, u->followers);
    if (app.playing.viewers >= 0) fmt_count(vw, sizeof vw, app.playing.viewers);
    fmt_uptime(up, sizeof up, app.playing.started_at);
    snprintf(line, sizeof line, "%s%s%s%s", fo[0] ? fo : "", fo[0] ? " followers" : "",
             (fo[0] && vw[0]) ? "      " : "", vw[0] ? vw : "");
    if (vw[0]) { int n = (int)strlen(line); snprintf(line + n, sizeof line - n, " viewers"); }
    if (line[0]) text(40, 340, C_SEL, 0.85f, line);
    if (up[0]) { snprintf(line, sizeof line, "Live for %s", up); text(40, 368, C_DIM, 0.8f, line); }
    if (app.playing.game[0]) { fit(f, sizeof f, app.playing.game, 0.8f, w - 80); text(40, 396, C_DIM, 0.8f, f); }
    /* buttons */
    for (int i = 0; i < TVP_N; i++) {
        int bx = 40 + i * 150, by = SCREEN_H - 90, bw = 136, bh = 52;
        int sel = i == app.tv_panel_sel;
        const char *lbl = (i == 0 && followed) ? (app.user_loaded && u->following == 1 ? "Followed" : "Unfollow") : TVP_BTN[i];
        rect(bx, by, bw, bh, sel ? C_SEL : C_PANEL);
        text_center_in(bx, bw, by + 34, sel ? C_BG : C_TEXT, 0.9f, lbl);
    }
    text(40, SCREEN_H - 20, C_FAINT, 0.7f, "Left/Right: choose   X: select   O / SELECT: close");
}

static void tv_panel_input(const Input *in)
{
    uint32_t p = in->press;
    if (p & SCE_CTRL_LEFT && app.tv_panel_sel > 0) app.tv_panel_sel--;
    if (p & SCE_CTRL_RIGHT && app.tv_panel_sel < TVP_N - 1) app.tv_panel_sel++;
    if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_SELECT)) { app.tv_panel = 0; return; }
    if (!(p & SCE_CTRL_CROSS)) return;
    switch (app.tv_panel_sel) {
    case 0: heart(); break;                                  /* Follow (the shared dialog) */
    case 1: app_open_clips(); break;
    case 2: app.show_stats = !app.show_stats; break;
    case 3: app.tv_panel = 0; break;
    }
}

void player_screen_draw(void)
{
    if (app.theater) {
        draw_video_in(0, 0, TH_VIDEO_W, TH_VIDEO_H);
        draw_info_panel(0, TH_VIDEO_H, TH_VIDEO_W, SCREEN_H - TH_VIDEO_H);
        draw_chat_panel(CHAT_X, 0, CHAT_W, SCREEN_H);
        if (app.show_stats) draw_stats(6, 6);
    } else if (app.mode != UI_MODE_TV) {                     /* handheld: the classic overlay */
        draw_video_in(0, 0, SCREEN_W, SCREEN_H);
        const TspStatus *st = tsp_status();
        if (app.info_open) draw_info_panel_hh();
        else if (now_ms() < app.osd_until || (st && st->state >= TSP_ENDED) || app.dialog) draw_hh_overlay();
        if (app.show_stats) draw_stats(6, 120);
    } else {
        draw_video_in(0, 0, SCREEN_W, SCREEN_H);
        const TspStatus *st = tsp_status();
        if (now_ms() < app.osd_until || (st && st->state >= TSP_ENDED)) {
            rect(0, 0, SCREEN_W, 78, C_SHADE);
            char line[220], l1[48], l2[48];
            fit(line, sizeof line, app.playing.name, 1.1f, 420);
            text(20, 32, C_TEXT, 1.1f, line);
            info_lines(l1, sizeof l1, l2, sizeof l2);
            char right[140];
            snprintf(right, sizeof right, "%s%s%s%s%s", app.playing.game, app.playing.game[0] ? "   " : "", l1, l1[0] ? "   " : "", l2);
            fit(line, sizeof line, right, 0.8f, 480);
            text_right(SCREEN_W - 20, 31, C_SEL, 0.8f, line);
            fit(line, sizeof line, app.playing.title, 0.85f, SCREEN_W - 40);
            text(20, 62, C_DIM, 0.85f, line);
            rect(0, SCREEN_H - 32, SCREEN_W, 32, C_SHADE);
            snprintf(line, sizeof line, "O back   Square theater + chat   Triangle chat   L/R quality (%s)   START menu   SELECT channel", app.quality_label);
            text(14, SCREEN_H - 10, C_DIM, 0.72f, line);
        }
        if (app.show_stats) draw_stats(6, 84);
    }
    if (app.mode == UI_MODE_TV && app.tv_panel) draw_tv_panel();
    if (app.qmenu_open) draw_quality_menu();
    dialog_draw();
}

void player_screen_input(const Input *in)
{
    uint32_t p = in->press;
    if (dialog_input(in)) return;
    if (p || in->tap) app.osd_until = now_ms() + 4000;
    if (app.mode == UI_MODE_TV && app.tv_panel) { tv_panel_input(in); return; }
    int hh_full = app.mode != UI_MODE_TV && !app.theater;
    if (hh_full && app.info_open && !app.qmenu_open) {       /* the channel info panel */
        if ((p & (SCE_CTRL_CIRCLE | SCE_CTRL_UP)) || hit_box(in, 10, 10, 60, 60)) { app.info_open = 0; return; }
        if ((p & SCE_CTRL_START) || hit_box(in, 240, 10, 70, 60)) { app.qmenu_open = 1; app.qmenu_sel = app.quality; return; }
        if (in->tap && in->tap_x >= 312) app.info_open = 0;
        return;
    }
    if (hh_full && !app.qmenu_open) {                        /* the overlay's icons */
        int osd = now_ms() < app.osd_until;
        if ((p & SCE_CTRL_LEFT) || (osd && hit_box(in, 20, 30, 80, 80))) { if (!app.playing_vod) app_open_vods(); return; }
        if ((p & SCE_CTRL_RIGHT) || (osd && hit_box(in, 840, 30, 80, 80))) { heart(); return; }
        if ((p & SCE_CTRL_DOWN) || (osd && hit_box(in, 840, 450, 80, 80))) {
            app.info_open = 1;
            return;
        }
    }
    if (app.qmenu_open) {
        if (p & SCE_CTRL_UP && app.qmenu_sel > 0) app.qmenu_sel--;
        if (p & SCE_CTRL_DOWN && app.qmenu_sel < TWQ_COUNT - 1) app.qmenu_sel++;
        if (p & SCE_CTRL_CROSS) { app.qmenu_open = 0; if (app.qmenu_sel != app.quality) app_change_quality(app.qmenu_sel); }
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) app.qmenu_open = 0;
        return;
    }
    if (p & SCE_CTRL_CIRCLE) { app_back_from_player(); return; }
    if (p & SCE_CTRL_SQUARE) { app.theater = !app.theater; app_save_settings(); }
    if (p & SCE_CTRL_TRIANGLE) app_chat_prompt();
    if (p & SCE_CTRL_SELECT) {
        if (app.mode == UI_MODE_TV) { app.tv_panel = 1; app.tv_panel_sel = 0; }   /* TV: channel panel */
        else app.show_stats = !app.show_stats;                                     /* handheld/theater: stats */
    }
    if (p & SCE_CTRL_START) { app.qmenu_open = 1; app.qmenu_sel = app.quality; }
    if (p & SCE_CTRL_UP) app.chat_scroll++;
    if (p & SCE_CTRL_DOWN && app.chat_scroll > 0) app.chat_scroll--;
    if (p & SCE_CTRL_LTRIGGER && app.quality < TWQ_COUNT - 1) app_change_quality(app.quality + 1);   /* lower */
    if (p & SCE_CTRL_RTRIGGER && app.quality > 0) app_change_quality(app.quality - 1);               /* higher */
    if (in->tap && app.theater && in->tap_x >= CHAT_X && in->tap_y >= SCREEN_H - CHAT_INPUT) app_chat_prompt();
    if (in->tap && !app.theater) app.osd_until = now_ms() + 4000;
    if (app.theater && in->touching && in->touch_x >= CHAT_X && in->drag_dy) {   /* drag the chat */
        static int acc;
        acc += in->drag_dy;
        while (acc >= CHAT_LH * 2) { acc -= CHAT_LH * 2; app.chat_scroll++; }
        while (acc <= -CHAT_LH * 2) { acc += CHAT_LH * 2; if (app.chat_scroll > 0) app.chat_scroll--; }
    }
}
