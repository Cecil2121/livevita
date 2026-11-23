/* LiveVita: the PS TV layout, in the style of today's Twitch TV apps (Android TV / Fire TV):
 *  - a dark, subdued look; a few text tabs across the top (Home, Browse, Search, Profile) instead of a side menu;
 *  - Home: the top third previews whatever is highlighted (a large still of the stream, with its name, title, game
 *    and viewers), and below it rows that scroll sideways: live channels you follow, top live channels, top
 *    categories; Up / Down moves between rows;
 *  - Browse: categories in a grid, then a category's channels under the same preview;
 *  - Search: a search box and the results; Profile: the account and the settings.
 * The tabs use the shared views: Home = VIEW_FOLLOWING, Browse = VIEW_GAMES, Search = VIEW_SEARCH,
 * Profile = VIEW_SETTINGS. */
#include "home.h"
#include "app.h"
#include "imgcache.h"
#include "playerui.h"
#include "favcat.h"
#include "favchan.h"
#include "ui.h"
#include "twitch.h"
#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_BG      RGBA8(14, 14, 16, 255)
#define T_PANEL   RGBA8(31, 31, 35, 255)
#define T_TEXT    RGBA8(239, 239, 241, 255)
#define T_DIM     RGBA8(173, 173, 184, 255)
#define T_ACCENT  RGBA8(145, 71, 255, 255)
#define T_FOCUS   RGBA8(255, 255, 255, 255)

#define NAV_H 44
#define HERO_Y 50
#define HERO_H 210
#define ROWS_Y 272
#define CARD_W 210
#define CARD_H 118
#define CARD_G 14
#define GCARD_W 100
#define GCARD_H 133
#define GCARD_G 12
#define ROW_X 24

typedef struct { int x, y, w, h; } TBox;

static const char *const TAB_NAME[] = { "Home", "Browse", "Search", "Profile" };
static const int TAB_VIEW[] = { VIEW_FOLLOWING, VIEW_GAMES, VIEW_SEARCH, VIEW_PROFILE };
#define NTABS 4

static int g_nav = 1;                   /* the tabs have the focus */
static int g_row;                       /* Home: the row with the focus */
static int g_col[4];                    /* ... the card selected in each row (by row kind) */
static int g_hscroll[4];                /* ... and how far each row has scrolled */
static int g_search_box;                /* Search: the box has the focus */

static int tab_of_view(void)
{
    for (int i = 0; i < NTABS; i++) if (TAB_VIEW[i] == app.view) return i;
    if (app.view == VIEW_FEATURED) return 0;
    if (app.view == VIEW_SETTINGS) return 3;                 /* (Options belongs to Profile) */
    return 0;
}

/* ---------------------------------------------------------------- Home rows */

enum { ROW_FOLLOW, ROW_TOP, ROW_GAMES, ROW_FAVCAT, ROW_KINDS };

/* the rows shown (the followed row only when someone you follow is live) */
static int home_rows(int *kinds)
{
    int n = 0;
    if (!app.signed_in) return 0;
    if (app.lists[LIST_FOLLOWING].n) kinds[n++] = ROW_FOLLOW;
    if (favcat_count()) kinds[n++] = ROW_FAVCAT;
    kinds[n++] = ROW_TOP;
    kinds[n++] = ROW_GAMES;
    return n;
}

static int row_count(int kind)
{
    return kind == ROW_FOLLOW ? app.lists[LIST_FOLLOWING].n : kind == ROW_TOP ? app.lists[LIST_FEATURED].n
         : kind == ROW_FAVCAT ? favcat_count() : app.games.n;
}

static const char *row_title(int kind)
{
    return kind == ROW_FOLLOW ? "Live Channels You Follow" : kind == ROW_TOP ? "Top Live Channels"
         : kind == ROW_FAVCAT ? "Categories You Follow" : "Top Categories";
}

static int is_cat_row(int kind) { return kind == ROW_GAMES || kind == ROW_FAVCAT; }
static const TwGame *row_game(int kind, int i) { return kind == ROW_FAVCAT ? favcat_get(i) : &app.games.items[i]; }
static int row_height(int kind) { return is_cat_row(kind) ? 28 + GCARD_H + 30 : 28 + CARD_H + 46; }

/* ---------------------------------------------------------------- drawing pieces */

static void tv_stream_card(const TwStream *s, int x, int y, int focused)
{
    if (s->thumb[0]) draw_picture(img_get(s->thumb), x, y, CARD_W, CARD_H);
    else rect(x, y, CARD_W, CARD_H, T_PANEL);
    if (s->live) {
        rect(x + 6, y + 6, 36, 16, C_LIVE);
        text_center_in(x + 6, 36, y + 19, C_WHITE, 0.58f, "LIVE");
        if (s->viewers >= 0) {
            char v[24], vv[40];
            fmt_viewers(v, sizeof v, s->viewers);
            snprintf(vv, sizeof vv, "%s viewers", v);
            int w = text_w(0.58f, vv) + 10;
            rect(x + 6, y + CARD_H - 22, w, 16, RGBA8(0, 0, 0, 170));
            text(x + 11, y + CARD_H - 9, C_WHITE, 0.58f, vv);
        }
    }
    if (s->saved) {
        int w = text_w(0.55f, "Saved") + 10;
        rect(x + CARD_W - w - 6, y + 6, w, 16, T_ACCENT);
        text(x + CARD_W - w - 1, y + 19, C_WHITE, 0.55f, "Saved");
    }
    char f[200];
    fit(f, sizeof f, s->name, 0.8f, CARD_W - 4);
    text(x + 2, y + CARD_H + 18, focused ? T_TEXT : T_DIM, 0.8f, f);
    fit(f, sizeof f, s->game, 0.68f, CARD_W - 4);
    text(x + 2, y + CARD_H + 36, T_DIM, 0.68f, f);
    if (focused) outline(x, y, CARD_W, CARD_H, 3, T_FOCUS);
}

static void tv_game_card(const TwGame *g, int x, int y, int w, int h, int focused)
{
    draw_picture(img_get(g->box_art), x, y, w, h);
    char f[120];
    fit(f, sizeof f, g->name, 0.7f, w);
    text(x, y + h + 18, focused ? T_TEXT : T_DIM, 0.7f, f);
    if (g->followed || favcat_has(g->id)) {
        rect(x + 4, y + 4, text_w(0.55f, "Following") + 10, 16, T_ACCENT);
        text(x + 9, y + 17, C_WHITE, 0.55f, "Following");
    }
    if (focused) outline(x, y, w, h, 3, T_FOCUS);
}

/* the preview at the top: the highlighted stream (or category) */
static void tv_hero_stream(const TwStream *s)
{
    int px = SCREEN_W - 374 - 24;
    if (s->thumb_big[0] || s->thumb[0]) draw_picture(img_get(s->thumb_big[0] ? s->thumb_big : s->thumb), px, HERO_Y, 374, HERO_H);
    else rect(px, HERO_Y, 374, HERO_H, T_PANEL);
    for (int i = 0; i < 8; i++) rect(px + i * 6, HERO_Y, 6, HERO_H, RGBA8(14, 14, 16, 220 - i * 27));   /* fade into the page */
    char f[240];
    int x = 30, w = px - 50;
    if (s->live) { rect(x, HERO_Y + 14, 44, 20, C_LIVE); text_center_in(x, 44, HERO_Y + 30, C_WHITE, 0.7f, "LIVE"); }
    fit(f, sizeof f, s->name, 1.45f, w);
    text(x, HERO_Y + 72, T_TEXT, 1.45f, f);
    draw_wrapped(x, HERO_Y + 104, w, 0.85f, T_TEXT, s->title, 2, 24);
    char v[24] = "", up[24] = "", line[200];
    fmt_viewers(v, sizeof v, s->viewers);
    fmt_uptime(up, sizeof up, s->started_at);
    snprintf(line, sizeof line, "%s%s%s%s%s%s", s->game, s->game[0] && v[0] ? "   |   " : "", v, v[0] ? " viewers" : "",
             up[0] ? "   |   live for " : "", up);
    fit(f, sizeof f, line, 0.75f, w);
    text(x, HERO_Y + 168, T_ACCENT, 0.75f, f);
    text(x, HERO_Y + 196, T_DIM, 0.68f, "X  Watch");
}

static void tv_hero_game(const TwGame *g)
{
    int px = SCREEN_W - 158 - 60;
    draw_picture(img_get(g->box_art_big[0] ? g->box_art_big : g->box_art), px, HERO_Y, 158, HERO_H);
    char f[160];
    text(30, HERO_Y + 28, T_DIM, 0.72f, "CATEGORY");
    fit(f, sizeof f, g->name, 1.3f, px - 60);
    text(30, HERO_Y + 66, T_TEXT, 1.3f, f);
    int match = !strcmp(app.hero_cat_id, g->id);
    int blank = match && app.hero_cat_desc_loaded && !app.hero_cat_desc[0];
    const char *d = match && app.hero_cat_desc_loaded
                  ? (app.hero_cat_desc[0] ? app.hero_cat_desc : "No description for this category.")
                  : "Loading category info...";
    draw_wrapped(30, HERO_Y + 96, px - 60, 0.72f, blank ? T_DIM : T_TEXT, d, 4, 23);
    text(30, HERO_Y + 196, T_DIM, 0.68f, "X  See live channels");
}

/* box art for a category id, looked up in the loaded categories / followed ones */
static const char *cat_box_art(const char *id)
{
    for (int i = 0; i < app.games.n; i++)
        if (!strcmp(app.games.items[i].id, id))
            return app.games.items[i].box_art_big[0] ? app.games.items[i].box_art_big : app.games.items[i].box_art;
    for (int i = 0; i < favcat_count(); i++) {
        const TwGame *g = favcat_get(i);
        if (g && !strcmp(g->id, id)) return g->box_art_big[0] ? g->box_art_big : g->box_art;
    }
    return "";
}

/* the hero for an open category: box art, name and Twitch's description blurb */
static void tv_hero_category(void)
{
    int px = SCREEN_W - 158 - 60;
    const char *box = cat_box_art(app.game_id);
    if (box[0]) draw_picture(img_get(box), px, HERO_Y, 158, HERO_H);
    else rect(px, HERO_Y, 158, HERO_H, T_PANEL);
    char f[160];
    text(30, HERO_Y + 28, T_DIM, 0.72f, "CATEGORY");
    fit(f, sizeof f, app.game_name, 1.3f, px - 60);
    text(30, HERO_Y + 66, T_TEXT, 1.3f, f);
    const char *d = !app.game_desc_loaded ? "Loading category info..."
                  : app.game_desc[0] ? app.game_desc : "No description for this category.";
    draw_wrapped(30, HERO_Y + 96, px - 60, 0.72f, app.game_desc[0] || !app.game_desc_loaded ? T_TEXT : T_DIM, d, 4, 23);
    text(30, HERO_Y + 196, T_DIM, 0.68f, "X  Watch a live channel below");
}

static void tv_hero_message(const char *a, const char *b)
{
    rect(24, HERO_Y, SCREEN_W - 48, HERO_H, T_PANEL);
    text(50, HERO_Y + 80, T_TEXT, 1.2f, a);
    if (b) draw_wrapped(50, HERO_Y + 116, SCREEN_W - 100, 0.85f, T_DIM, b, 3, 24);
}

static void tv_nav(void)
{
    rect(0, 0, SCREEN_W, NAV_H, T_BG);
    draw_wordmark(22, 32, 0.8f);
    int x = 190, cur = tab_of_view();
    for (int i = 0; i < NTABS; i++) {
        int w = text_w(0.9f, TAB_NAME[i]);
        if (g_nav && i == cur) { rect(x - 12, 7, w + 24, 30, T_FOCUS); text(x, 29, T_BG, 0.9f, TAB_NAME[i]); }
        else {
            text(x, 29, i == cur ? T_TEXT : T_DIM, 0.9f, TAB_NAME[i]);
            if (i == cur) rect(x, 38, w, 3, T_ACCENT);
        }
        x += w + 40;
    }
    if (app.signed_in && tw_user()[0]) text_right(SCREEN_W - 24, 29, T_DIM, 0.75f, tw_user());
}

static void tv_hint(const char *h)
{
    text(24, SCREEN_H - 8, RGBA8(110, 110, 120, 255), 0.62f, h);
}

/* ---------------------------------------------------------------- Home */

static void keep_col_visible(int r, int x_of_col, int w)
{
    if (x_of_col - g_hscroll[r] < ROW_X) g_hscroll[r] = x_of_col - ROW_X;
    if (x_of_col + w - g_hscroll[r] > SCREEN_W - ROW_X) g_hscroll[r] = x_of_col + w - (SCREEN_W - ROW_X);
    if (g_hscroll[r] < 0) g_hscroll[r] = 0;
}

static void tv_draw_home(void)
{
    int kinds[4], nr = home_rows(kinds);
    if (!app.signed_in) {
        tv_hero_message("Log in to start watching", "Go to Profile and choose Log in, then enter the code you see at twitch.tv/activate on a phone or computer.");
        return;
    }
    if (!app.games.loaded && !app_games_loading()) app_load_games();
    if (g_row >= nr) g_row = nr - 1;
    if (g_row < 0) g_row = 0;
    /* the preview */
    int k = kinds[g_row], c = g_col[k];
    if (row_count(k) && c >= row_count(k)) c = g_col[k] = row_count(k) - 1;
    if (!row_count(k)) { app_hero_category(""); tv_hero_message(app_list_loading(k == ROW_TOP ? LIST_FEATURED : LIST_FOLLOWING) || app_games_loading() ? "Loading..." : "Nothing here right now", NULL); }
    else if (is_cat_row(k)) { const TwGame *g = row_game(k, c); app_hero_category(g->id); tv_hero_game(g); }
    else { app_hero_category(""); tv_hero_stream(&app.lists[k == ROW_FOLLOW ? LIST_FOLLOWING : LIST_FEATURED].items[c]); }
    /* the rows: the focused one at the top of the area, the next one below */
    static float vy;
    float target = 0;
    for (int r = 0; r < g_row; r++) target += row_height(kinds[r]);
    vy += (target - vy) * 0.25f;
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, ROWS_Y - 4, SCREEN_W, SCREEN_H - 16);
    int y = ROWS_Y - (int)vy;
    for (int r = 0; r < nr; r++) {
        int kind = kinds[r], n = row_count(kind), h = row_height(kind);
        if (y + h > ROWS_Y - 10 && y < SCREEN_H) {
            text(ROW_X, y + 18, r == g_row && !g_nav ? T_TEXT : T_DIM, 0.85f, row_title(kind));
            int cw = is_cat_row(kind) ? GCARD_W + GCARD_G : CARD_W + CARD_G;
            if (n) keep_col_visible(kind, ROW_X + g_col[kind] * cw, cw - (is_cat_row(kind) ? GCARD_G : CARD_G));
            for (int i = 0; i < n; i++) {
                int x = ROW_X + i * cw - g_hscroll[kind];
                if (x + cw < 0 || x > SCREEN_W) continue;
                int foc = !g_nav && r == g_row && i == g_col[kind];
                if (is_cat_row(kind)) tv_game_card(row_game(kind, i), x, y + 28, GCARD_W, GCARD_H, foc);
                else tv_stream_card(&app.lists[kind == ROW_FOLLOW ? LIST_FOLLOWING : LIST_FEATURED].items[i], x, y + 28, foc);
            }
            if (!n) text(ROW_X, y + 70, T_DIM, 0.8f, "Loading...");
        }
        y += h;
    }
    vita2d_disable_clipping();
}

static void tv_home_input(const Input *in)
{
    uint32_t p = in->press;
    int kinds[4], nr = home_rows(kinds);
    if (!nr) { if (p & SCE_CTRL_UP) g_nav = 1; return; }
    int k = kinds[g_row], n = row_count(k);
    if (p & SCE_CTRL_LEFT && g_col[k] > 0) g_col[k]--;
    if (p & SCE_CTRL_RIGHT && g_col[k] < n - 1) g_col[k]++;
    if (p & SCE_CTRL_DOWN && g_row < nr - 1) g_row++;
    if (p & SCE_CTRL_UP) { if (g_row > 0) g_row--; else g_nav = 1; }
    if (p & SCE_CTRL_CROSS && n) {
        if (is_cat_row(k)) { const TwGame *g = row_game(k, g_col[k]); app.view = VIEW_GAMES; app_open_game(g->id, g->name); }
        else {
            const TwStream *s = &app.lists[k == ROW_FOLLOW ? LIST_FOLLOWING : LIST_FEATURED].items[g_col[k]];
            app_open_stream(s);
        }
    }
    if (p & SCE_CTRL_TRIANGLE) { app_load_list(LIST_FOLLOWING); app_load_list(LIST_FEATURED); app_load_games(); }
}

/* ---------------------------------------------------------------- grids (Browse, a category, Search) */

typedef struct { int x0, y0, cols, w, h, gx, gy, cap; } TGrid;

static void grid_keep(int sel, int *top, const TGrid *g, int rows_visible)
{
    int row = sel / g->cols;
    if (row < *top) *top = row;
    if (row >= *top + rows_visible) *top = row - rows_visible + 1;
    if (*top < 0) *top = 0;
}

/* d-pad in a grid: returns -1 when Up leaves the first row */
static int grid_move(int *sel, int n, int cols, uint32_t p)
{
    if (!n) return (p & SCE_CTRL_UP) ? -1 : 0;
    if (p & SCE_CTRL_UP) { if (*sel < cols) return -1; *sel -= cols; }
    if (p & SCE_CTRL_DOWN && *sel + cols < n) *sel += cols;
    else if (p & SCE_CTRL_DOWN && *sel / cols < (n - 1) / cols) *sel = n - 1;
    if (p & SCE_CTRL_LEFT && *sel % cols > 0) (*sel)--;
    if (p & SCE_CTRL_RIGHT && *sel % cols < cols - 1 && *sel + 1 < n) (*sel)++;
    return 0;
}

static const TGrid GAMES_G = { 24, 92, 7, 112, 149, 21, 14, 26 };
static const TGrid STREAMS_G = { 24, ROWS_Y, 4, CARD_W, CARD_H, 18, 10, 44 };

static void draw_stream_grid(StreamList *L, int list, const TGrid *g, int focus)
{
    int rows_visible = (SCREEN_H - 16 - g->y0) / (g->h + g->cap + g->gy);
    if (rows_visible < 1) rows_visible = 1;
    if (L->sel >= L->n) L->sel = L->n ? L->n - 1 : 0;
    grid_keep(L->sel, &L->top, g, rows_visible);
    if (!L->n) {
        int loading = app_list_loading(list);
        text(g->x0, g->y0 + 40, L->err[0] && !loading ? C_WARN : T_DIM, 0.9f, loading ? "Loading..." : L->err[0] ? L->err
             : list == LIST_SEARCH ? (app.search_query[0] ? "No channels found." : "Type a channel name to search.") : "Nobody is live here right now.");
        return;
    }
    for (int r = 0; r <= rows_visible; r++)
        for (int c = 0; c < g->cols; c++) {
            int i = (L->top + r) * g->cols + c;
            if (i >= L->n) break;
            int x = g->x0 + c * (g->w + g->gx), y = g->y0 + r * (g->h + g->cap + g->gy);
            if (y > SCREEN_H - 20) break;
            tv_stream_card(&L->items[i], x, y, focus && i == L->sel);
        }
}

static void draw_browse(void)
{
    if (!app.games.loaded && !app_games_loading()) app_load_games();
    if (app.game_open) {                                     /* a category: its info + channels (like Twitch's category page) */
        StreamList *L = &app.lists[LIST_GAME];
        tv_hero_category();
        char ct[160];
        snprintf(ct, sizeof ct, "%s   %s", app.game_name, app_cat_followed() ? "-  Following (Square: unfollow)" : "-  Square: follow");
        text(24, ROWS_Y - 6, app_cat_followed() ? T_ACCENT : T_DIM, 0.8f, ct);
        TGrid g = STREAMS_G;
        g.y0 = ROWS_Y + 4;
        draw_stream_grid(L, LIST_GAME, &g, !g_nav);
        return;
    }
    text(24, 76, T_TEXT, 1.1f, "Categories");
    GameList *G = &app.games;
    const TGrid *g = &GAMES_G;
    int rows_visible = 2;
    if (!G->n) { text(24, 160, T_DIM, 0.9f, app_games_loading() || !G->loaded ? "Loading..." : G->err[0] ? G->err : "No categories."); return; }
    if (G->sel >= G->n) G->sel = G->n - 1;
    grid_keep(G->sel, &G->top, g, rows_visible);
    for (int r = 0; r <= rows_visible; r++)
        for (int c = 0; c < g->cols; c++) {
            int i = (G->top + r) * g->cols + c;
            if (i >= G->n) break;
            int x = g->x0 + c * (g->w + g->gx), y = g->y0 + r * (g->h + g->cap + g->gy);
            if (y > SCREEN_H - 20) break;
            tv_game_card(&G->items[i], x, y, g->w, g->h, !g_nav && i == G->sel);
        }
}

static void draw_search(void)
{
    int x = 24, y = 58, w = SCREEN_W - 48, h = 40;
    rect(x, y, w, h, T_PANEL);
    if (!g_nav && g_search_box) outline(x, y, w, h, 3, T_FOCUS);
    draw_icon(ICON_SEARCH, x + 10, y + 8, 24, T_DIM);
    char f[200];
    fit(f, sizeof f, app.search_query[0] ? app.search_query : "Search channels", 0.9f, w - 60);
    text(x + 46, y + 27, app.search_query[0] ? T_TEXT : T_DIM, 0.9f, f);
    TGrid g = STREAMS_G;
    g.y0 = 118;
    draw_stream_grid(&app.lists[LIST_SEARCH], LIST_SEARCH, &g, !g_nav && !g_search_box);
}

static int g_prof_row, g_prof_col;      /* 0 the buttons, 1 the followed categories */

static void draw_profile(void)
{
    app_load_me();
    const TwUser *u = &app.me;
    if (app.me_loaded && u->avatar[0]) draw_picture(img_get(u->avatar), 30, 62, 120, 120);
    else rect(30, 62, 120, 120, T_PANEL);
    char f[240], line[200];
    if (!app.signed_in) {
        text(172, 102, T_TEXT, 1.4f, "Not signed in");
        draw_wrapped(172, 134, 740, 0.85f, T_DIM, "Log in to see your profile, follow categories and chat: you'll enter a code at twitch.tv/activate.", 2, 24);
    } else {
        fit(f, sizeof f, app.me_loaded ? u->name : tw_user(), 1.4f, 740);
        text(172, 102, T_TEXT, 1.4f, f);
        draw_wrapped(172, 132, 740, 0.8f, T_DIM, app.me_loaded ? (u->description[0] ? u->description : "No bio yet.") : "Loading your profile...", 2, 22);
        if (app.me_loaded) {
            char a[32] = "-", b[32] = "-", j[40] = "";
            if (u->followers >= 0) fmt_count(a, sizeof a, u->followers);
            if (u->following_total >= 0) fmt_count(b, sizeof b, u->following_total);
            fmt_joined(j, sizeof j, u->created_at);
            snprintf(line, sizeof line, "%s followers   |   following %s   |   %s", a, b, j);
            text(172, 182, T_ACCENT, 0.78f, line);
        }
    }
    /* signed out: a Log in button (Options: START) */
    int nb = app.signed_in ? 0 : 1;
    if (nb) {
        int foc = !g_nav && g_prof_row == 0;
        rect(30, 196, 170, 44, foc ? T_FOCUS : T_PANEL);
        text_center_in(30, 170, 224, foc ? T_BG : T_TEXT, 0.9f, "Log in");
    } else if (g_prof_row == 0) g_prof_row = 1;
    /* Local Pins: channels saved on this Vita (your real Twitch follows are the "Live Channels You Follow" row on Home) */
    int nc = favchan_count();
    text(30, 248, T_DIM, 0.85f, "Local Pins (this Vita)");
    if (!nc) text(30, 286, T_DIM, 0.72f, "Press the heart on a stream to pin it here. Your Twitch follows are on the Home screen.");
    for (int i = 0; i < nc; i++) {
        int x = 30 + i * 100;
        if (x + 88 > SCREEN_W) break;
        const TwStream *s = favchan_at(i);
        if (!s) break;
        if (s->avatar[0]) draw_picture(img_get(s->avatar), x, 260, 88, 88);
        else { rect(x, 260, 88, 88, T_PANEL); char ini[2] = { s->name[0] ? s->name[0] : '?', 0 }; text_center_in(x, 88, 260 + 54, RGBA8(120,110,140,255), 1.5f, ini); }
        char nm[64]; fit(nm, sizeof nm, s->name, 0.62f, 88);
        text(x, 366, !g_nav && g_prof_row == 1 && g_prof_col == i ? T_TEXT : T_DIM, 0.62f, nm);
        if (!g_nav && g_prof_row == 1 && g_prof_col == i) outline(x, 260, 88, 88, 3, T_FOCUS);
    }
    /* Categories You Follow (saved on this Vita) */
    text(30, 392, T_DIM, 0.85f, "Categories You Follow");
    int n = favcat_count();
    if (!n) { text(30, 430, T_DIM, 0.72f, "Open a category in Browse and press Square to follow it (saved on this Vita)."); return; }
    for (int i = 0; i < n; i++) {
        int x = 30 + i * (88 + 14);
        if (x + 88 > SCREEN_W) break;
        tv_game_card(favcat_get(i), x, 404, 88, 96, !g_nav && g_prof_row == 2 && g_prof_col == i);
    }
}

static int tv_prof_rows(int *rows)
{
    int k = 0;
    if (!app.signed_in) rows[k++] = 0;
    rows[k++] = 1;
    rows[k++] = 2;
    return k;
}
static int tv_prof_count(int row) { return row == 0 ? (app.signed_in ? 0 : 1) : row == 1 ? favchan_count() : favcat_count(); }

static void profile_input(const Input *in)
{
    uint32_t p = in->press;
    if (p & SCE_CTRL_CIRCLE) { g_nav = 1; return; }
    int rows[3], nr = tv_prof_rows(rows), cur = 0;
    for (int i = 0; i < nr; i++) if (rows[i] == g_prof_row) cur = i;
    if (p & SCE_CTRL_UP) { if (cur > 0) { g_prof_row = rows[--cur]; if (g_prof_col >= tv_prof_count(g_prof_row)) g_prof_col = 0; } else { g_nav = 1; return; } }
    if (p & SCE_CTRL_DOWN && cur < nr - 1) { g_prof_row = rows[++cur]; if (g_prof_col >= tv_prof_count(g_prof_row)) g_prof_col = 0; }
    int lim = tv_prof_count(g_prof_row);
    if (p & SCE_CTRL_LEFT && g_prof_col > 0) g_prof_col--;
    if (p & SCE_CTRL_RIGHT && g_prof_col + 1 < lim) g_prof_col++;
    if (!(p & SCE_CTRL_CROSS)) return;
    if (g_prof_row == 0) { if (!app.signed_in) app_login(); }
    else if (g_prof_row == 1) { const TwStream *s = favchan_at(g_prof_col); if (s) app_open_stream(s); }
    else { const TwGame *g = favcat_get(g_prof_col); if (g) { app.view = VIEW_GAMES; app_open_game(g->id, g->name); } }
}

static void draw_options(void)
{
    text(30, 84, T_TEXT, 1.2f, "Options");
    text(30, 108, T_DIM, 0.75f, "O: back to your profile");
    home_settings_draw(1, 116);
}

/* ---------------------------------------------------------------- entry points */

/* ---------------------------------------------------------------- clips (TV) */

static int g_clip_sel, g_clip_top;

static void tv_clip_card(const TwVideo *v, int x, int y, int focused)
{
    if (v->thumb[0]) draw_picture(img_get(v->thumb), x, y, CARD_W, CARD_H);
    else rect(x, y, CARD_W, CARD_H, T_PANEL);
    char d[24];
    fmt_duration(d, sizeof d, v->duration);
    int w = text_w(0.6f, d) + 10;
    rect(x + CARD_W - w - 6, y + CARD_H - 22, w, 16, RGBA8(0, 0, 0, 180));
    text(x + CARD_W - w - 1, y + CARD_H - 9, C_WHITE, 0.6f, d);
    char f[200];
    fit(f, sizeof f, v->title[0] ? v->title : "(untitled)", 0.78f, CARD_W - 4);
    text(x + 2, y + CARD_H + 18, focused ? T_TEXT : T_DIM, 0.78f, f);
    char line[80], vw[24], ago[32];
    fmt_count(vw, sizeof vw, v->views < 0 ? 0 : v->views);
    fmt_ago(ago, sizeof ago, v->created_at);
    snprintf(line, sizeof line, "%s views  -  %s", vw, ago);
    text(x + 2, y + CARD_H + 36, T_DIM, 0.66f, line);
    if (focused) outline(x, y, CARD_W, CARD_H, 3, T_FOCUS);
}

static void draw_tv_clips(void)
{
    char t[160];
    snprintf(t, sizeof t, "Clips - %s", app.clips_of.name);
    char tf[160]; fit(tf, sizeof tf, t, 1.3f, 760);
    text(24, 50, T_TEXT, 1.3f, tf);
    VideoList *V = &app.clips;
    if (!V->n) {
        text(24, 150, V->err[0] ? C_WARN : T_DIM, 0.9f, !V->loaded ? "Loading clips..." : V->err[0] ? V->err : "This channel has no clips.");
        return;
    }
    const TGrid g = { 24, 92, 4, CARD_W, CARD_H, 18, 10, 44 };
    int rows_visible = (SCREEN_H - 16 - g.y0) / (g.h + g.cap + g.gy);
    if (rows_visible < 1) rows_visible = 1;
    if (g_clip_sel >= V->n) g_clip_sel = V->n - 1;
    grid_keep(g_clip_sel, &g_clip_top, &g, rows_visible);
    for (int r = 0; r <= rows_visible; r++)
        for (int col = 0; col < g.cols; col++) {
            int i = (g_clip_top + r) * g.cols + col;
            if (i >= V->n) break;
            int x = g.x0 + col * (g.w + g.gx), y = g.y0 + r * (g.h + g.cap + g.gy);
            if (y > SCREEN_H - 20) break;
            tv_clip_card(&V->items[i], x, y, i == g_clip_sel);
        }
}

static void tv_clips_input(const Input *in)
{
    uint32_t p = in->press;
    VideoList *V = &app.clips;
    if (p & SCE_CTRL_CIRCLE) { app_clips_back(); return; }
    grid_move(&g_clip_sel, V->n, 4, p);
    if (p & SCE_CTRL_CROSS && V->n) app_play_clip(&V->items[g_clip_sel]);
}

void tv_draw(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, T_BG);
    if (app.view == VIEW_FEATURED) app.view = VIEW_FOLLOWING;    /* (TV: top channels live on Home) */
    switch (app.view) {
    case VIEW_GAMES: draw_browse(); break;
    case VIEW_SEARCH: draw_search(); break;
    case VIEW_PROFILE: draw_profile(); break;
    case VIEW_SETTINGS: draw_options(); break;
    case VIEW_CLIPS: draw_tv_clips(); break;
    default: app.view = VIEW_FOLLOWING; tv_draw_home(); break;
    }
    tv_nav();
    tv_hint(g_nav ? "Left/Right: tabs   Down: content   L/R: tabs"
            : app.view == VIEW_SETTINGS ? "Up/Down: choose   Left/Right/X: change   O: back to profile"
            : app.view == VIEW_PROFILE ? "X: open   START: Options   O: tabs"
            : app.view == VIEW_GAMES && app.game_open ? "X: watch   Square: follow / unfollow   O: back to categories"
            : app.view == VIEW_SEARCH ? "X: watch / type   Square: new search   O: tabs"
            : app.view == VIEW_CLIPS ? "X: play clip   O: back"
            : "X: open   Up/Down: rows   O: tabs   Triangle: refresh");
    dialog_draw();
}

static void set_tab(int t)
{
    app.view = TAB_VIEW[t];
    app.game_open = 0;
    if (app.view == VIEW_SEARCH) g_search_box = 1;
}

void tv_input(const Input *in)
{
    if (dialog_input(in)) return;
    uint32_t p = in->press;
    if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        int t = tab_of_view() + ((p & SCE_CTRL_RTRIGGER) ? 1 : -1);
        set_tab((t + NTABS) % NTABS);
        return;
    }
    if (p & SCE_CTRL_START) { app.view = VIEW_SETTINGS; g_nav = 0; return; }
    if (g_nav) {                                             /* the tabs */
        int t = tab_of_view();
        if (p & SCE_CTRL_LEFT && t > 0) set_tab(t - 1);
        if (p & SCE_CTRL_RIGHT && t < NTABS - 1) set_tab(t + 1);
        if (p & (SCE_CTRL_DOWN | SCE_CTRL_CROSS)) g_nav = 0;
        return;
    }
    if (app.view == VIEW_CLIPS) { tv_clips_input(in); return; }
    switch (app.view) {
    case VIEW_FOLLOWING:
        if (p & SCE_CTRL_CIRCLE) { g_nav = 1; return; }
        tv_home_input(in);
        break;
    case VIEW_GAMES:
        if (app.game_open) {
            StreamList *L = &app.lists[LIST_GAME];
            if (p & SCE_CTRL_CIRCLE) { app_close_game(); return; }
            if (p & SCE_CTRL_SQUARE) { if (app.signed_in) app_toggle_cat_follow(); else { app.dialog = DLG_SIGNIN; app.dlg_sel = 0; } return; }
            if (grid_move(&L->sel, L->n, STREAMS_G.cols, p) < 0) g_nav = 1;
            if (p & SCE_CTRL_CROSS && L->n) app_open_stream(&L->items[L->sel]);
            if (p & SCE_CTRL_TRIANGLE) app_load_list(LIST_GAME);
        } else {
            GameList *G = &app.games;
            if (p & SCE_CTRL_CIRCLE) { g_nav = 1; return; }
            if (grid_move(&G->sel, G->n, GAMES_G.cols, p) < 0) g_nav = 1;
            if (p & SCE_CTRL_CROSS && G->n) app_open_game(G->items[G->sel].id, G->items[G->sel].name);
            if (p & SCE_CTRL_TRIANGLE) app_load_games();
        }
        break;
    case VIEW_SEARCH: {
        StreamList *L = &app.lists[LIST_SEARCH];
        if (p & SCE_CTRL_CIRCLE) { g_nav = 1; return; }
        if (p & SCE_CTRL_SQUARE) { app_search_prompt(); return; }
        if (g_search_box) {
            if (p & SCE_CTRL_CROSS) app_search_prompt();
            if (p & SCE_CTRL_UP) g_nav = 1;
            if (p & SCE_CTRL_DOWN && L->n) g_search_box = 0;
        } else {
            if (grid_move(&L->sel, L->n, STREAMS_G.cols, p) < 0) g_search_box = 1;
            if (p & SCE_CTRL_CROSS && L->n) {
                if (L->items[L->sel].live) app_open_stream(&L->items[L->sel]);
                else app_set_status("That channel is offline right now.");
            }
        }
        break;
    }
    case VIEW_PROFILE:
        profile_input(in);
        break;
    case VIEW_SETTINGS:
        if (p & SCE_CTRL_CIRCLE) { app.view = VIEW_PROFILE; return; }
        if ((p & SCE_CTRL_UP) && app.settings_sel == 0) { g_nav = 1; return; }
        home_settings_input(in, 116);
        break;
    }
}
