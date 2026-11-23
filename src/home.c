/* LiveVita: the home screens in two layouts over the same lists.
 *  - Handheld (PS Vita): a slim purple tab bar (L/R or touch) and a 3-column grid of stream cards.
 *  - TV (PS TV / DualShock): a left icon sidebar that opens when focused, and a 2 x 2 grid of big cards. */
#include "home.h"
#include "app.h"
#include "imgcache.h"
#include "ui.h"
#include "twitch.h"
#include <psp2/ctrl.h>
#include <stdio.h>
#include <string.h>

static const char *const VIEW_NAME[VIEW_COUNT] = { "Following", "Featured", "Games", "Search", "Settings" };

/* TV sidebar entries -> views */
static const int TV_SIDE_VIEW[] = { VIEW_SEARCH, VIEW_FOLLOWING, VIEW_GAMES, VIEW_SETTINGS };
static const char *const TV_SIDE_NAME[] = { "Search", "Following", "Browse", "Settings" };
static const int TV_SIDE_ICON[] = { ICON_SEARCH, ICON_HEART, ICON_GRID, ICON_GEAR };
#define TV_SIDE_N 4
#define TV_SIDE_W_CLOSED 76
#define TV_SIDE_W_OPEN 236

/* ---------------------------------------------------------------- geometry */

typedef struct {
    int x0, y0;                         /* first card */
    int cols, rows;                     /* rows fully in view */
    int w, th, h;                       /* card width, picture height, card height (with text) */
    int gx, gy;                         /* gaps */
    int overlay;                        /* text drawn over the picture (TV) */
    float ts;                           /* text scale */
} Grid;

static Grid stream_grid(int tv, int top_y)
{
    Grid g;
    memset(&g, 0, sizeof g);
    if (tv) {
        g.cols = 2; g.w = 368; g.th = 207; g.h = 207; g.gx = 24; g.gy = 18; g.overlay = 1; g.ts = 0.95f;   /* two full rows */
        g.x0 = TV_SIDE_W_CLOSED + (SCREEN_W - TV_SIDE_W_CLOSED - (2 * 368 + 24)) / 2;
    } else {
        g.cols = 3; g.w = 300; g.th = 169; g.h = 213; g.gx = 15; g.gy = 12; g.ts = 0.8f;
        g.x0 = 15;
    }
    g.y0 = top_y;
    g.rows = (SCREEN_H - 34 - top_y + g.gy) / (g.h + g.gy);
    if (g.rows < 1) g.rows = 1;
    return g;
}

static Grid game_grid(int tv, int top_y)
{
    Grid g;
    memset(&g, 0, sizeof g);
    if (tv) {                                                /* two full rows of box art */
        g.cols = 5; g.w = 136; g.th = 181; g.h = 207; g.gx = 22; g.gy = 14; g.ts = 0.85f;
        g.x0 = TV_SIDE_W_CLOSED + (SCREEN_W - TV_SIDE_W_CLOSED - (5 * 136 + 4 * 22)) / 2;
    }
    else    { g.cols = 6; g.w = 140; g.th = 187; g.h = 210; g.gx = 14; g.gy = 12; g.ts = 0.75f; g.x0 = 15; }
    g.y0 = top_y;
    g.rows = (SCREEN_H - 34 - top_y + g.gy) / (g.h + g.gy);
    if (g.rows < 1) g.rows = 1;
    return g;
}

static int content_top(int tv)
{
    if (tv) return app.view == VIEW_SEARCH ? 112 : 74;
    return app.view == VIEW_SEARCH ? 116 : 64;
}

/* keeps the selection's row in view */
static void keep_visible(int sel, int *top, const Grid *g)
{
    int row = sel / g->cols;
    if (row < *top) *top = row;
    if (row >= *top + g->rows) *top = row - g->rows + 1;
    if (*top < 0) *top = 0;
}

/* d-pad in a grid; returns 1 if it moved, -1 if Left was pressed in the first column (TV: to the sidebar) */
static int grid_move(int *sel, int n, int cols, uint32_t press)
{
    if (n <= 0) return (press & SCE_CTRL_LEFT) ? -1 : 0;
    int s = *sel, moved = 0;
    if (press & SCE_CTRL_LEFT) { if (s % cols == 0) return -1; s--; }
    if (press & SCE_CTRL_RIGHT) { if (s % cols < cols - 1 && s + 1 < n) s++; }
    if (press & SCE_CTRL_UP) { if (s - cols >= 0) s -= cols; }
    if (press & SCE_CTRL_DOWN) { if (s + cols < n) s += cols; else if (s / cols < (n - 1) / cols) s = n - 1; }
    if (s != *sel) { *sel = s; moved = 1; }
    return moved;
}

/* index of the card under a tap, or -1 */
static int grid_hit(const Grid *g, int top, int n, int x, int y)
{
    for (int r = 0; r <= g->rows; r++)
        for (int c = 0; c < g->cols; c++) {
            int i = (top + r) * g->cols + c;
            if (i >= n) return -1;
            int cx = g->x0 + c * (g->w + g->gx), cy = g->y0 + r * (g->h + g->gy);
            if (x >= cx && x < cx + g->w && y >= cy && y < cy + g->h) return i;
        }
    return -1;
}

/* ---------------------------------------------------------------- cards */

static void draw_stream_card(const TwStream *s, int x, int y, const Grid *g, int selected)
{
    char line[220], v[24];
    if (s->live && s->thumb[0]) draw_picture(img_get(s->thumb), x, y, g->w, g->th);
    else {
        rect(x, y, g->w, g->th, C_PANEL);
        char ini[8] = { s->name[0] ? s->name[0] : '?', 0 };
        text_center_in(x, g->w, y + g->th / 2 + 18, C_PANEL_L, 3.0f, ini);
    }
    if (s->live) {                                           /* LIVE + viewers */
        rect(x + 8, y + 8, 46, 22, C_LIVE);
        text_center_in(x + 8, 46, y + 25, C_WHITE, 0.75f, "LIVE");
        if (s->viewers >= 0) {
            fmt_viewers(v, sizeof v, s->viewers);
            char vv[40];
            snprintf(vv, sizeof vv, "%s viewers", v);
            int w = text_w(0.75f, vv) + 14;
            int vy = g->overlay ? y + 8 : y + g->th - 30;
            rect(g->overlay ? x + 60 : x + 8, vy, w, 22, C_SHADE);
            text((g->overlay ? x + 60 : x + 8) + 7, vy + 17, C_WHITE, 0.75f, vv);
        }
    } else {
        rect(x + 8, y + 8, 70, 22, C_SHADE);
        text_center_in(x + 8, 70, y + 25, C_DIM, 0.75f, "OFFLINE");
    }
    int tx = x + (g->overlay ? 12 : 2), tw = g->w - (g->overlay ? 24 : 4);
    int ty = g->overlay ? y + g->th - 52 : y + g->th + 20;
    if (g->overlay) {                                        /* dark band behind the text */
        rect(x, y + g->th - 72, g->w, 72, RGBA8(10, 8, 16, 150));
        rect(x, y + g->th - 76, g->w, 4, RGBA8(10, 8, 16, 70));
    }
    fit(line, sizeof line, s->title[0] ? s->title : s->name, g->ts, tw);
    text(tx, ty, C_TEXT, g->ts, line);
    if (s->game[0]) snprintf(line, sizeof line, "%s  -  %s", s->name, s->game);
    else snprintf(line, sizeof line, "%s", s->name);
    char l2[220];
    fit(l2, sizeof l2, line, g->ts * 0.9f, tw);
    text(tx, ty + (int)(24 * g->ts), C_DIM, g->ts * 0.9f, l2);
    if (selected) outline(x, y, g->w, g->h, 4, C_SEL);
}

static void draw_game_card(const TwGame *gm, int pseudo_top, int x, int y, const Grid *g, int selected)
{
    char line[120];
    if (pseudo_top) {                                        /* TV Browse: "Top live" */
        rect(x, y, g->w, g->th, C_PURPLE);
        draw_icon(ICON_STAR, x + g->w / 2 - 30, y + g->th / 2 - 40, 60, C_WHITE);
        text_center_in(x, g->w, y + g->th / 2 + 50, C_WHITE, 0.9f, "Top live");
        fit(line, sizeof line, "All categories", g->ts, g->w - 4);
    } else {
        draw_picture(img_get(gm->box_art), x, y, g->w, g->th);
        fit(line, sizeof line, gm->name, g->ts, g->w - 4);
    }
    text(x + 2, y + g->th + 19, C_TEXT, g->ts, line);
    if (selected) outline(x, y, g->w, g->h, 4, C_SEL);
}

static void draw_message(int x, int w, int y, const char *msg, int loading, int is_err)
{
    char line[220];
    fit(line, sizeof line, msg, 1.0f, w - 40);
    text_center_in(x, w, y, is_err ? C_WARN : C_DIM, 1.0f, line);
    if (loading) spinner(x + w / 2, y + 40, C_SEL);
}

/* ---------------------------------------------------------------- views */

static void draw_streams(StreamList *L, int list, int tv)
{
    Grid g = stream_grid(tv, content_top(tv));
    int area_x = tv ? TV_SIDE_W_CLOSED : 0, area_w = SCREEN_W - area_x;
    int loading = app_list_loading(list);
    if (!L->n) {
        const char *msg;
        if (loading) msg = list == LIST_SEARCH ? "Searching..." : "Loading...";
        else if (L->err[0]) msg = L->err;
        else if (list == LIST_SEARCH) msg = app.search_query[0] ? "No channels found." : "Search for a channel: press X or Square, or tap the box above.";
        else if (list == LIST_FOLLOWING) msg = L->loaded ? "None of the channels you follow are live right now." : "";
        else msg = L->loaded ? "Nothing here right now." : "";
        draw_message(area_x, area_w, g.y0 + 150, msg, loading, L->err[0] && !loading);
        return;
    }
    keep_visible(L->sel, &L->top, &g);
    for (int r = 0; r <= g.rows; r++)
        for (int c = 0; c < g.cols; c++) {
            int i = (L->top + r) * g.cols + c;
            if (i >= L->n) break;
            int x = g.x0 + c * (g.w + g.gx), y = g.y0 + r * (g.h + g.gy);
            if (y > SCREEN_H - 34) break;
            draw_stream_card(&L->items[i], x, y, &g, i == L->sel && (!tv || !app.side_focus));
        }
    if (loading) spinner(SCREEN_W - 30, g.y0 - 20, C_SEL);
}

static int games_count(int tv) { return app.games.n + (tv ? 1 : 0); }

static void draw_games(int tv)
{
    Grid g = game_grid(tv, content_top(tv));
    int area_x = tv ? TV_SIDE_W_CLOSED : 0, area_w = SCREEN_W - area_x;
    int n = games_count(tv), loading = app_games_loading();
    if (!app.games.n && (loading || app.games.err[0])) {
        draw_message(area_x, area_w, g.y0 + 150, loading ? "Loading categories..." : app.games.err, loading, !loading);
        if (!tv) return;
    }
    keep_visible(app.games.sel, &app.games.top, &g);
    for (int r = 0; r <= g.rows; r++)
        for (int c = 0; c < g.cols; c++) {
            int i = (app.games.top + r) * g.cols + c;
            if (i >= n) break;
            int x = g.x0 + c * (g.w + g.gx), y = g.y0 + r * (g.h + g.gy);
            if (y > SCREEN_H - 34) break;
            int pseudo = tv && i == 0;
            draw_game_card(pseudo ? NULL : &app.games.items[i - (tv ? 1 : 0)], pseudo, x, y, &g,
                           i == app.games.sel && (!tv || !app.side_focus));
        }
}

/* settings rows */
enum { SET_QUALITY, SET_LAYOUT, SET_CHAT, SET_ACCOUNT, SET_N };

static void setting_value(int i, char *out, size_t cap)
{
    static const char *const LAYOUT[] = { "Auto", "Handheld", "TV" };
    switch (i) {
    case SET_QUALITY: snprintf(out, cap, "< %s >", TWQ_NAME[app.quality]); break;
    case SET_LAYOUT:
        if (app.layout == LAYOUT_AUTO)
            snprintf(out, cap, "< Auto (%s: %s) >", app.is_pstv ? "PS TV" : app.ext_pad ? "Vita + DualShock" : "PS Vita",
                     app.detected == UI_MODE_TV ? "TV" : "Handheld");
        else snprintf(out, cap, "< %s >", LAYOUT[app.layout]);
        break;
    case SET_CHAT: snprintf(out, cap, "< %s >", app.chat_on ? "On" : "Off"); break;
    case SET_ACCOUNT: snprintf(out, cap, "%s", app.signed_in ? "Sign out" : "Log in"); break;
    }
}

void home_settings_draw(int tv, int top)
{
    static const char *const NAME[SET_N] = { "Video quality", "Layout", "Chat", "Account" };
    /* TV: the dark grey look of the TV layout, full width; handheld: the purple panels */
    int x = tv ? 30 : 60, w = tv ? SCREEN_W - 60 : SCREEN_W - 120;
    int y = top + 10, rh = tv ? 64 : 56;
    float s = tv ? 1.05f : 0.95f;
    unsigned pan = tv ? RGBA8(31, 31, 35, 255) : C_PANEL, pan_sel = tv ? RGBA8(53, 53, 60, 255) : C_PANEL_L;
    unsigned bar = tv ? RGBA8(145, 71, 255, 255) : C_PURPLE;
    for (int i = 0; i < SET_N; i++) {
        int ry = y + i * (rh + 8);
        int sel = i == app.settings_sel && (!tv || !app.side_focus);
        rect(x, ry, w, rh, sel ? pan_sel : pan);
        if (sel) rect(x, ry, 5, rh, bar);
        char label[120], val[80];
        if (i == SET_ACCOUNT) snprintf(label, sizeof label, "Account: %s", tw_user()[0] ? tw_user() : "not signed in");
        else snprintf(label, sizeof label, "%s", NAME[i]);
        text(x + 22, ry + rh / 2 + 8, C_TEXT, s, label);
        setting_value(i, val, sizeof val);
        text_right(x + w - 22, ry + rh / 2 + 8, sel ? C_SEL : C_DIM, s * 0.9f, val);
    }
    char about[160];
    snprintf(about, sizeof about, "LiveVita %s  -  an unofficial Twitch viewer. Not affiliated with Twitch.", APP_VERSION);
    text(x, y + SET_N * (rh + 8) + 34, C_FAINT, 0.8f, about);
    if (!tw_can_chat() && tw_user()[0])
        text(x, y + SET_N * (rh + 8) + 60, C_FAINT, 0.8f, "To send chat messages, sign out and sign in again (adds the chat permission).");
}

static void draw_search_box(int tv)
{
    int x = tv ? TV_SIDE_W_CLOSED + 30 : 15, w = SCREEN_W - x - (tv ? 30 : 15);
    int y = tv ? 70 : 62, h = 40;
    rect(x, y, w, h, C_PANEL);
    outline(x, y, w, h, 1, C_PANEL_L);
    draw_icon(ICON_SEARCH, x + 10, y + 8, 24, C_DIM);
    char line[200];
    fit(line, sizeof line, app.search_query[0] ? app.search_query : "Search channels  (Square / tap)", 0.95f, w - 60);
    text(x + 44, y + 28, app.search_query[0] ? C_TEXT : C_FAINT, 0.95f, line);
}

/* ---------------------------------------------------------------- handheld chrome */

static int hh_tab_x[VIEW_COUNT], hh_tab_w[VIEW_COUNT];

static void draw_handheld_bar(void)
{
    rect(0, 0, SCREEN_W, 52, C_PURPLE);
    draw_wordmark(14, 37, 0.95f);
    int x = 210;
    rect(x - 34, 15, 22, 22, C_PURPLE_D);
    text_center_in(x - 34, 22, 32, C_DIM, 0.7f, "L");
    for (int v = 0; v < VIEW_COUNT; v++) {
        int w = text_w(0.95f, VIEW_NAME[v]);
        hh_tab_x[v] = x - 12;
        hh_tab_w[v] = w + 24;
        if (v == app.view) { rect(x - 12, 8, w + 24, 36, C_PURPLE_D); rect(x - 12, 46, w + 24, 4, C_WHITE); }
        text(x, 34, v == app.view ? C_WHITE : RGBA8(215, 200, 245, 255), 0.95f, VIEW_NAME[v]);
        x += w + 30;
    }
    rect(x - 8, 15, 22, 22, C_PURPLE_D);
    text_center_in(x - 8, 22, 32, C_DIM, 0.7f, "R");
}

/* ---------------------------------------------------------------- TV chrome */

static void draw_tv_sidebar(void)
{
    int w = app.side_focus ? TV_SIDE_W_OPEN : TV_SIDE_W_CLOSED;
    if (app.side_focus) rect(w, 0, SCREEN_W - w, SCREEN_H, RGBA8(0, 0, 0, 120));   /* dim the grid behind */
    rect(0, 0, w, SCREEN_H, C_PANEL);
    rect(w - 2, 0, 2, SCREEN_H, C_PURPLE_D);
    if (app.side_focus) draw_wordmark(18, 48, 0.9f);
    else { rect(18, 26, 40, 30, C_PURPLE); text_center_in(18, 40, 48, C_WHITE, 0.9f, "LV"); }
    for (int i = 0; i < TV_SIDE_N; i++) {
        int y = 110 + i * 70;
        int active = TV_SIDE_VIEW[i] == app.view || (TV_SIDE_VIEW[i] == VIEW_GAMES && app.view == VIEW_FEATURED);
        int hi = app.side_focus && i == app.side_sel;
        if (hi) rect(8, y - 8, w - 18, 56, C_PURPLE);
        else if (active) rect(0, y - 8, 5, 56, C_PURPLE);
        draw_icon(TV_SIDE_ICON[i], 22, y + 2, 32, hi || active ? C_WHITE : C_DIM);
        if (app.side_focus) text(74, y + 28, hi ? C_WHITE : C_TEXT, 1.05f, TV_SIDE_NAME[i]);
    }
}

static void draw_tv_header(void)
{
    char t[160];
    if (app.view == VIEW_GAMES && app.game_open) snprintf(t, sizeof t, "%s", app.game_name);
    else if (app.view == VIEW_FEATURED) snprintf(t, sizeof t, "Top live");
    else if (app.view == VIEW_GAMES) snprintf(t, sizeof t, "Browse");
    else snprintf(t, sizeof t, "%s", VIEW_NAME[app.view]);
    char f[160];
    fit(f, sizeof f, t, 1.4f, 560);
    text(TV_SIDE_W_CLOSED + 30, 50, C_TEXT, 1.4f, f);
    if (tw_user()[0]) text_right(SCREEN_W - 30, 46, C_DIM, 0.85f, tw_user());
}

/* ---------------------------------------------------------------- draw / input entry points */

static void footer(const char *hint)
{
    rect(0, SCREEN_H - 30, SCREEN_W, 30, C_PANEL);
    text(14, SCREEN_H - 9, C_DIM, 0.8f, hint);
}

void home_draw(void)
{
    if (app.mode != UI_MODE_TV) { hh_draw(); return; }
    tv_draw();
    return;
    int tv = app.mode == UI_MODE_TV;
    if (!tv) {
        draw_handheld_bar();
        if (app.view == VIEW_GAMES && app.game_open) {       /* category title under the bar */
            char t[160], f[160];
            snprintf(t, sizeof t, "%s", app.game_name);
            fit(f, sizeof f, t, 0.85f, 700);
            text(16, 72, C_SEL, 0.85f, f);
        }
    } else draw_tv_header();

    int list = -1;
    switch (app.view) {
    case VIEW_FOLLOWING: list = LIST_FOLLOWING; break;
    case VIEW_FEATURED: list = LIST_FEATURED; break;
    case VIEW_SEARCH: draw_search_box(tv); list = LIST_SEARCH; break;
    case VIEW_GAMES: if (app.game_open) list = LIST_GAME; else draw_games(tv); break;
    case VIEW_SETTINGS: home_settings_draw(tv, content_top(tv)); break;
    }
    if (list >= 0) {
        if (!tv && list == LIST_GAME) {
            /* (the grid starts a little lower for the category title) */
            StreamList *L = &app.lists[list];
            Grid g = stream_grid(0, 82);
            if (!L->n) draw_streams(L, list, 0);
            else {
                keep_visible(L->sel, &L->top, &g);
                for (int r = 0; r <= g.rows; r++)
                    for (int c = 0; c < g.cols; c++) {
                        int i = (L->top + r) * g.cols + c;
                        if (i >= L->n) break;
                        int x = g.x0 + c * (g.w + g.gx), y = g.y0 + r * (g.h + g.gy);
                        if (y > SCREEN_H - 34) break;
                        draw_stream_card(&L->items[i], x, y, &g, i == L->sel);
                    }
            }
        } else draw_streams(&app.lists[list], list, tv);
    }
    if (tv) draw_tv_sidebar();

    const char *hint;
    if (app.view == VIEW_SETTINGS) hint = tv ? "Up/Down: choose   Left/Right/X: change   Left on the edge: menu" : "Up/Down: choose   Left/Right/X: change   L/R: tabs";
    else if (tv && app.side_focus) hint = "Up/Down: choose   X/Right: open   O: back to the grid";
    else if (app.view == VIEW_GAMES && app.game_open) hint = "X: watch   O: back to categories   Triangle: refresh";
    else if (app.view == VIEW_GAMES) hint = "X: open category   Triangle: refresh";
    else if (app.view == VIEW_SEARCH) hint = "X: watch   Square: search   Triangle: refresh";
    else hint = tv ? "X: watch   Left: menu   Triangle: refresh   L/R: sections" : "X: watch   L/R: tabs   Triangle: refresh   Touch: tap a card";
    footer(hint);
}

static void set_view(int v)
{
    if (v == app.view) return;
    app.view = v;
    if (v == VIEW_FOLLOWING && !app.lists[LIST_FOLLOWING].loaded) app_load_list(LIST_FOLLOWING);
    if (v == VIEW_FEATURED && !app.lists[LIST_FEATURED].loaded) app_load_list(LIST_FEATURED);
    if (v == VIEW_GAMES && !app.games.loaded) app_load_games();
}

void home_settings_input(const Input *in, int top)
{
    if (in->press & SCE_CTRL_UP && app.settings_sel > 0) app.settings_sel--;
    if (in->press & SCE_CTRL_DOWN && app.settings_sel < SET_N - 1) app.settings_sel++;
    int dir = (in->press & SCE_CTRL_LEFT) ? -1 : (in->press & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) ? 1 : 0;
    if (in->tap) {                                           /* tap a row: select it and change it */
        int tv = app.mode == UI_MODE_TV, rh = tv ? 64 : 56, y = top + 10;
        int i = (in->tap_y - y) / (rh + 8);
        if (in->tap_y >= y && i >= 0 && i < SET_N) { app.settings_sel = i; dir = 1; }
    }
    if (!dir) return;
    switch (app.settings_sel) {
    case SET_QUALITY: app.quality = (app.quality + dir + TWQ_COUNT) % TWQ_COUNT; app_save_settings(); break;
    case SET_LAYOUT: app_set_layout((app.layout + dir + LAYOUT_COUNT) % LAYOUT_COUNT); break;
    case SET_CHAT: app_toggle_chat(); break;
    case SET_ACCOUNT: if (in->press & SCE_CTRL_CROSS || in->tap) { if (app.signed_in) app_logout(); else app_login(); } break;
    }
}

/* X / tap on an item of the current view */
static void activate(int idx)
{
    int tv = app.mode == UI_MODE_TV;
    if (app.view == VIEW_GAMES && !app.game_open) {
        if (tv && idx == 0) { app.view = VIEW_FEATURED; if (!app.lists[LIST_FEATURED].loaded) app_load_list(LIST_FEATURED); return; }
        int gi = idx - (tv ? 1 : 0);
        if (gi >= 0 && gi < app.games.n) app_open_game(app.games.items[gi].id, app.games.items[gi].name);
        return;
    }
    StreamList *L = app_current_list();
    if (!L || idx < 0 || idx >= L->n) return;
    if (L->items[idx].live) app_open_stream(&L->items[idx]);
    else app_set_status("That channel is offline right now.");
}

void home_input(const Input *in)
{
    if (app.mode != UI_MODE_TV) { hh_input(in); return; }
    tv_input(in);
    return;
    int tv = app.mode == UI_MODE_TV;
    uint32_t p = in->press;

    /* ---- TV sidebar ---- */
    if (tv && app.side_focus) {
        if (p & SCE_CTRL_UP && app.side_sel > 0) { app.side_sel--; set_view(TV_SIDE_VIEW[app.side_sel]); app.game_open = 0; }
        if (p & SCE_CTRL_DOWN && app.side_sel < TV_SIDE_N - 1) { app.side_sel++; set_view(TV_SIDE_VIEW[app.side_sel]); app.game_open = 0; }
        if (p & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) app.side_focus = 0;
        if (p & SCE_CTRL_CROSS && app.view == VIEW_SEARCH) app_search_prompt();
        return;
    }
    /* ---- switching views ---- */
    if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        int d = (p & SCE_CTRL_LTRIGGER) ? -1 : 1;
        if (tv) {
            int i = 0;
            for (int k = 0; k < TV_SIDE_N; k++) if (TV_SIDE_VIEW[k] == app.view) i = k;
            i = (i + d + TV_SIDE_N) % TV_SIDE_N;
            app.side_sel = i;
            app.game_open = 0;
            set_view(TV_SIDE_VIEW[i]);
        } else set_view((app.view + d + VIEW_COUNT) % VIEW_COUNT);
        return;
    }
    if (p & SCE_CTRL_START) { set_view(VIEW_SETTINGS); if (tv) { for (int k = 0; k < TV_SIDE_N; k++) if (TV_SIDE_VIEW[k] == VIEW_SETTINGS) app.side_sel = k; } return; }
    if (!tv && in->tap && in->tap_y < 52) {                  /* a tab */
        for (int v = 0; v < VIEW_COUNT; v++)
            if (in->tap_x >= hh_tab_x[v] && in->tap_x < hh_tab_x[v] + hh_tab_w[v]) { set_view(v); app.game_open = 0; }
        return;
    }
    if (app.view == VIEW_SETTINGS) {
        if (tv && (p & SCE_CTRL_CIRCLE)) { app.side_focus = 1; return; }   /* (Left changes the value here) */
        home_settings_input(in, content_top(tv));
        return;
    }
    /* ---- the grid ---- */
    int is_games = app.view == VIEW_GAMES && !app.game_open;
    int n, *sel, *top;
    Grid g;
    if (is_games) { n = games_count(tv); sel = &app.games.sel; top = &app.games.top; g = game_grid(tv, content_top(tv)); }
    else {
        StreamList *L = app_current_list();
        if (!L) return;
        n = L->n; sel = &L->sel; top = &L->top;
        g = stream_grid(tv, !tv && app.view == VIEW_GAMES ? 82 : content_top(tv));
    }
    int m = grid_move(sel, n, g.cols, p);
    if (m < 0 && tv) { app.side_focus = 1; for (int k = 0; k < TV_SIDE_N; k++) if (TV_SIDE_VIEW[k] == app.view || (app.view == VIEW_FEATURED && TV_SIDE_VIEW[k] == VIEW_GAMES)) app.side_sel = k; return; }
    if (p & SCE_CTRL_CROSS) {
        if (app.view == VIEW_SEARCH && n == 0) app_search_prompt();
        else activate(*sel);
    }
    if (p & SCE_CTRL_SQUARE && app.view == VIEW_SEARCH) app_search_prompt();
    if (p & SCE_CTRL_CIRCLE) {
        if (app.view == VIEW_GAMES && app.game_open) app_close_game();
        else if (tv && app.view == VIEW_FEATURED) { app.view = VIEW_GAMES; }
        else if (tv) app.side_focus = 1;
    }
    if (p & SCE_CTRL_TRIANGLE) {
        if (is_games) app_load_games();
        else if (app.view == VIEW_FOLLOWING) app_load_list(LIST_FOLLOWING);
        else if (app.view == VIEW_FEATURED) app_load_list(LIST_FEATURED);
        else if (app.view == VIEW_SEARCH) app_load_list(LIST_SEARCH);
        else if (app.game_open) app_load_list(LIST_GAME);
    }
    /* ---- touch (handheld) ---- */
    if (!tv) {
        if (in->tap && app.view == VIEW_SEARCH && in->tap_y >= 62 && in->tap_y < 102) { app_search_prompt(); return; }
        if (in->tap) {
            int i = grid_hit(&g, *top, n, in->tap_x, in->tap_y);
            if (i >= 0) { *sel = i; activate(i); }
        }
        static int drag_acc;
        if (in->touching) {
            drag_acc += in->drag_dy;
            int step = (g.h + g.gy) / 2;
            while (drag_acc <= -step) { drag_acc += step; if (*top + g.rows < (n + g.cols - 1) / g.cols) { (*top)++; if (*sel < *top * g.cols) *sel = *top * g.cols; } }
            while (drag_acc >= step) { drag_acc -= step; if (*top > 0) { (*top)--; int last = (*top + g.rows) * g.cols - 1; if (*sel > last) *sel = last; } }
        } else drag_acc = 0;
    }
}
