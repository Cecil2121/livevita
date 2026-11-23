/* LiveVita: the handheld (PS Vita) layout, in the style of the classic Vita app.
 *  - Home (the first screen): a column of big purple menu buttons on the left, and a mosaic of the top
 *    channels on the right: one big featured tile, smaller tiles, and a "..." tile for all of them.
 *  - Channel pages (Top Channels, Following, a category, search results): tiles flowing to the right in
 *    three rows, name and "playing <game>" over the picture.
 *  - Games: a big first category, then box art flowing to the right in two rows.
 * Tiles are placed at absolute positions and scrolled sideways; the d-pad moves to the nearest tile in
 * its direction, so the same code moves through the mosaic and the pages. */
#include "home.h"
#include "playerui.h"
#include "favcat.h"
#include "favchan.h"
#include "app.h"
#include "imgcache.h"
#include "ui.h"
#include "twitch.h"
#include <psp2/ctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_HH_BG    RGBA8(23, 17, 36, 255)        /* the deep purple-black background */
#define C_HH_BTN   RGBA8(100, 65, 165, 255)      /* menu buttons */
#define C_HH_TILE  RGBA8(38, 30, 56, 255)        /* a tile without a picture */
#define C_HH_GAME  RGBA8(185, 160, 240, 255)     /* "playing <game>" */
#define C_HH_SEL   RGBA8(255, 255, 255, 255)     /* the white selection frame */

#define MENU_X 25
#define MENU_W 197
#define MAX_TILES (MAX_STREAMS + 2)

typedef struct { int x, y, w, h; } Box;

/* ---------------------------------------------------------------- menu */

/* signed in: Profile, Search, Following (live count), Games, Channels
 * signed out: Search, Games, Channels, Log In */
enum { A_PROFILE, A_SEARCH, A_FOLLOWING, A_GAMES, A_CHANNELS, A_LOGIN };
static const int MENU_IN[] = { A_PROFILE, A_SEARCH, A_FOLLOWING, A_GAMES, A_CHANNELS };
static const int MENU_OUT[] = { A_SEARCH, A_GAMES, A_CHANNELS, A_LOGIN };
static const char *const ACTION_NAME[] = { "Profile", "Search", "Following", "Games", "Channels", "Log In" };

static int menu_n(void) { return app.signed_in ? 5 : 4; }
static int menu_action(int i) { return app.signed_in ? MENU_IN[i] : MENU_OUT[i]; }

static Box menu_box(int i)
{
    Box b;
    if (app.signed_in) { b.x = MENU_X; b.y = 98 + i * 84; b.w = MENU_W; b.h = 75; }   /* five buttons */
    else { b.x = MENU_X; b.y = 98 + i * 92; b.w = MENU_W; b.h = 80; }                /* four buttons */
    return b;
}

/* ---------------------------------------------------------------- tiles */

/* name over the bottom of a picture, then "playing <game>" (the game a little bolder) */
static void tile_caption(int x, int y, int w, int h, const char *name, const char *game, int viewers, int big)
{
    float ns = big ? 1.15f : 0.95f, gs = big ? 0.9f : 0.78f;
    int band = big ? 74 : game && game[0] ? 56 : 40;
    rect(x, y + h - band, w, band, RGBA8(12, 8, 22, 150));
    rect(x, y + h - band - 6, w, 6, RGBA8(12, 8, 22, 70));
    char line[240], f[240];
    fit(f, sizeof f, name, ns, w - 24);
    text(x + 12, y + h - (big ? 42 : 30), C_WHITE, ns, f);
    if (!game || !game[0]) return;
    int px = x + 12, py = y + h - (big ? 14 : 10);
    text(px, py, C_DIM, gs, "playing");
    px += text_w(gs, "playing ") + 2;
    char tail[60] = "";
    if (big && viewers >= 0) {
        char v[24];
        fmt_viewers(v, sizeof v, viewers);
        snprintf(tail, sizeof tail, " for %s viewers", v);
    }
    int room = x + w - 12 - px - (tail[0] ? text_w(gs, tail) : 0);
    fit(line, sizeof line, game, gs, room);
    text(px, py, C_HH_GAME, gs, line);                       /* (drawn twice, 1 px apart: bold) */
    text(px + 1, py, C_HH_GAME, gs, line);
    if (tail[0]) text(px + text_w(gs, line) + 1, py, C_DIM, gs, tail);
}

static void channel_tile(const TwStream *s, Box b, int big)
{
    int narrow = b.w < 150;
    if (narrow && s->avatar[0]) {                            /* small tiles: the streamer's profile picture, as a centered icon */
        rect(b.x, b.y, b.w, b.h, C_HH_TILE);
        int d = b.w < b.h ? b.w : b.h;
        draw_picture(img_get(s->avatar), b.x + (b.w - d) / 2, b.y + (b.h - d) / 2, d, d);
    } else {
        const char *url = big && s->thumb_big[0] ? s->thumb_big : s->thumb;
        if (s->live && url[0]) draw_picture(img_get(url), b.x, b.y, b.w, b.h);
        else rect(b.x, b.y, b.w, b.h, C_HH_TILE);
    }
    if (!s->live) {
        rect(b.x + 10, b.y + 10, 72, 22, C_SHADE);
        text_center_in(b.x + 10, 72, b.y + 27, C_DIM, 0.72f, "OFFLINE");
    }
    tile_caption(b.x, b.y, b.w, b.h, s->name, narrow ? "" : s->game, s->viewers, big);
    if (s->saved) {                                          /* followed on this Vita */
        int w = text_w(0.62f, "Saved") + 12;
        rect(b.x + b.w - w - 8, b.y + 8, w, 18, C_HH_BTN);
        text(b.x + b.w - w - 2, b.y + 22, C_WHITE, 0.62f, "Saved");
    }
}

static void more_tile(Box b)
{
    rect(b.x, b.y, b.w, b.h, C_HH_TILE);
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    for (int k = -1; k <= 1; k++) rect(cx + k * 20 - 5, cy - 5, 10, 10, RGBA8(100, 80, 170, 255));
}

static void game_tile(const TwGame *g, Box b, int big)
{
    const char *url = big && g->box_art_big[0] ? g->box_art_big : g->box_art;
    draw_picture(img_get(url), b.x, b.y, b.w, b.h);
    float s = big ? 1.1f : 0.9f;
    int band = big ? 56 : 40;
    rect(b.x, b.y + b.h - band, b.w, band, RGBA8(12, 8, 22, 150));
    char f[120];
    fit(f, sizeof f, g->name, s, b.w - 20);
    text(b.x + 12, b.y + b.h - (big ? 20 : 13), C_WHITE, s, f);
    if (g->followed) {                                       /* followed on this Vita */
        int w = text_w(0.62f, "Following") + 12;
        rect(b.x + 6, b.y + 6, w, 18, C_HH_BTN);
        text(b.x + 12, b.y + 20, C_WHITE, 0.62f, "Following");
    }
}

static void sel_frame(Box b) { outline(b.x, b.y, b.w, b.h, 4, C_HH_SEL); }

/* ---------------------------------------------------------------- layouts */

/* home mosaics.
 * signed in ("Followed Channels"):   [ big      ][a]     signed out ("Top Channels"):  [ big   ][a]
 *                                    [          ][b]                                   [       ][b]
 *                                    [c   ][d][e][...]                                 [c][d][...]   */
#define MOS_X 255
#define MOS_Y 97
#define MOS_TILES_IN 7
#define MOS_TILES_OUT 6
static Box mosaic_box(int i)
{
    static const Box IN[MOS_TILES_IN] = {
        { 255, 97, 479, 275 }, { 742, 97, 230, 132 }, { 742, 240, 230, 132 },
        { 255, 380, 232, 132 }, { 495, 380, 115, 132 }, { 618, 380, 116, 132 }, { 742, 380, 230, 132 } };
    static const Box OUT[MOS_TILES_OUT] = {
        { 252, 92, 466, 266 }, { 728, 92, 232, 128 }, { 728, 230, 232, 128 },
        { 252, 368, 228, 128 }, { 490, 368, 228, 128 }, { 728, 368, 232, 128 } };
    return app.signed_in ? IN[i] : OUT[i];
}
static int mosaic_tiles(void) { return app.signed_in ? MOS_TILES_IN : MOS_TILES_OUT; }
/* the channels it shows: signed in, the followed ones that are live */
static StreamList *mosaic_list(void) { return &app.lists[LIST_FOLLOWING]; }
/* the first channels, then "..." (all of them); fewer channels: fewer tiles */
static int mosaic_count(void)
{
    if (!app.signed_in) return 0;
    int n = mosaic_list()->n, t = mosaic_tiles();
    return n >= t - 1 ? t : n;
}
static int mosaic_is_more(int i) { return app.signed_in && i == mosaic_tiles() - 1 && mosaic_list()->n >= mosaic_tiles() - 1; }

/* channel pages: three rows flowing right */
#define PG_X 16
#define PG_Y 80
#define PG_W 236
#define PG_H 133
#define PG_G 12
static Box page_box(int i)
{
    Box b = { PG_X + (i / 3) * (PG_W + PG_G), PG_Y + (i % 3) * (PG_H + PG_G), PG_W, PG_H };
    return b;
}

/* games: box art in two rows flowing right (six columns on screen) */
#define GM_W 148
#define GM_H 197
#define GM_G 6
#define GM_Y 86
static Box games_box(int i)
{
    Box b = { PG_X + (i / 2) * (GM_W + GM_G), GM_Y + (i % 2) * (GM_H + GM_G), GM_W, GM_H };
    return b;
}

/* a category's channels: one row of big tiles flowing right, under the Follow button */
#define CT_Y 200
#define CT_W 556
#define CT_H 313
#define CT_G 10
static Box cat_box(int i) { Box b = { PG_X + i * (CT_W + CT_G), CT_Y, CT_W, CT_H }; return b; }
#define FOLLOW_BTN_X 25
#define FOLLOW_BTN_Y 90

/* past broadcasts: two rows flowing right */
#define VD_Y 82
#define VD_W 352
#define VD_H 198
#define VD_G 8
static Box vod_box(int i) { Box b = { PG_X + (i / 2) * (VD_W + VD_G), VD_Y + (i % 2) * (VD_H + VD_G), VD_W, VD_H }; return b; }

/* ---------------------------------------------------------------- navigation */

/* the tile nearest to `from` in a direction (dx, dy), or -1 */
static int nearest(Box (*boxf)(int), int n, int from, int dx, int dy)
{
    Box a = boxf(from);
    int ax = a.x + a.w / 2, ay = a.y + a.h / 2, best = -1;
    long best_score = 0;
    for (int i = 0; i < n; i++) {
        if (i == from) continue;
        Box b = boxf(i);
        int bx = b.x + b.w / 2, by = b.y + b.h / 2;
        int along = dx ? (bx - ax) * dx : (by - ay) * dy;
        int across = dx ? abs(by - ay) : abs(bx - ax);
        /* must lie in that direction, edges past ours (a tall tile's neighbours overlap it sideways) */
        if (dx > 0 && b.x < a.x + a.w - 2) continue;
        if (dx < 0 && b.x + b.w > a.x + 2) continue;
        if (dy > 0 && b.y < a.y + a.h - 2) continue;
        if (dy < 0 && b.y + b.h > a.y + 2) continue;
        if (along <= 0) continue;
        long score = (long)along + 2L * across;
        if (best < 0 || score < best_score) { best = i; best_score = score; }
    }
    return best;
}

/* d-pad on tiles: 1 moved, -1 Left with nothing there, 0 nothing */
static int tiles_move(Box (*boxf)(int), int n, int *sel, uint32_t p)
{
    if (n <= 0) return (p & SCE_CTRL_LEFT) ? -1 : 0;
    if (*sel >= n) *sel = n - 1;
    int dx = 0, dy = 0;
    if (p & SCE_CTRL_LEFT) dx = -1;
    else if (p & SCE_CTRL_RIGHT) dx = 1;
    else if (p & SCE_CTRL_UP) dy = -1;
    else if (p & SCE_CTRL_DOWN) dy = 1;
    else return 0;
    int k = nearest(boxf, n, *sel, dx, dy);
    if (k < 0) return dx < 0 ? -1 : 0;
    *sel = k;
    return 1;
}

/* sideways scroll (pixels) that keeps the selected tile on screen */
static void keep_in_view(Box b, int *scroll)
{
    if (b.x - *scroll < PG_X) *scroll = b.x - PG_X;
    if (b.x + b.w - *scroll > SCREEN_W - PG_X) *scroll = b.x + b.w - (SCREEN_W - PG_X);
    if (*scroll < 0) *scroll = 0;
}

static int hit(Box (*boxf)(int), int n, int scroll, int x, int y)
{
    for (int i = 0; i < n; i++) {
        Box b = boxf(i);
        b.x -= scroll;
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
    }
    return -1;
}

/* ---------------------------------------------------------------- top bar */

static void hint_pill(int x_right, int y, const char *key, const char *what)
{
    int kw = text_w(0.62f, key) + 10, ww = text_w(0.75f, what);
    int x = x_right - ww - 8 - kw;
    rect(x, y - 14, kw, 18, RGBA8(200, 200, 210, 255));
    text(x + 5, y, RGBA8(30, 25, 45, 255), 0.62f, key);
    text(x + kw + 8, y, C_TEXT, 0.75f, what);
}

static void page_title(const char *t, int back)
{
    char f[160];
    fit(f, sizeof f, t, 1.15f, 600);
    text(PG_X + 2, 52, C_TEXT, 1.15f, f);
    hint_pill(SCREEN_W - 20, 40, "START", "Options");
    if (back) hint_pill(SCREEN_W - 140, 40, "O", "Back");
}

/* ---------------------------------------------------------------- screens */

static void draw_message(int x, int w, int y, const char *msg, int loading, int err)
{
    char f[220];
    fit(f, sizeof f, msg, 0.95f, w - 40);
    text_center_in(x, w, y, err ? C_WARN : C_DIM, 0.95f, f);
    if (loading) spinner(x + w / 2, y + 40, C_SEL);
}

/* ---- the home screen is wider than the display: [menu][mosaic] [Games] [Top Channels].
 * Moving right slides it to the left; each section ends in "..." for its full page. */

#define SEC_B_X 1012                    /* Games: a big first category, then 2 x 2 */
#define SEC_C_X (SEC_B_X + 300 + 2 * (GM_W + 10) + 40)   /* Top Channels: 2 columns x 3 rows */
#define SEC_Y 92

enum { HK_FOLLOW, HK_FOLLOW_MORE, HK_GAME, HK_GAME_MORE, HK_TOP, HK_TOP_MORE };

static int sec_a_n(void) { return mosaic_count(); }
static int sec_b_n(void) { if (!app.signed_in) return 0; int n = app.games.n; return n >= 4 ? 5 : n; }
static int sec_c_n(void) { if (!app.signed_in) return 0; int n = app.lists[LIST_FEATURED].n; return n >= 6 ? 6 : n; }
static int home_n(void) { return sec_a_n() + sec_b_n() + sec_c_n(); }

/* what tile i is, and its index in its list */
static int home_kind(int i, int *idx)
{
    int a = sec_a_n(), b = sec_b_n();
    if (i < a) { *idx = i; return mosaic_is_more(i) ? HK_FOLLOW_MORE : HK_FOLLOW; }
    i -= a;
    if (i < b) { *idx = i; return (b == 5 && i == 4) ? HK_GAME_MORE : HK_GAME; }
    i -= b;
    *idx = i;
    return (i == 5) ? HK_TOP_MORE : HK_TOP;
}

static Box sec_b_box(int i)
{
    Box b;
    if (i == 0) { b.x = SEC_B_X; b.y = SEC_Y; b.w = 290; b.h = 404; return b; }
    int k = i - 1;                                           /* 1 2 / 3 more: columns of two */
    b.x = SEC_B_X + 300 + (k / 2) * (GM_W + 10);
    b.y = SEC_Y + (k % 2) * (GM_H + 10);
    b.w = GM_W;
    b.h = GM_H;
    return b;
}

static Box sec_c_box(int i)
{
    Box b = { SEC_C_X + (i % 2) * (PG_W + 10), SEC_Y + (i / 2) * (PG_H + 10), PG_W, PG_H };
    return b;
}

static Box home_box(int i)
{
    int k, kind = home_kind(i, &k);
    if (kind == HK_FOLLOW || kind == HK_FOLLOW_MORE) return mosaic_box(k);
    if (kind == HK_GAME || kind == HK_GAME_MORE) return sec_b_box(k);
    return sec_c_box(k);
}

/* where the screen should be: the mosaic keeps the menu in sight; further right, the tile stays on screen */
static void home_follow_selection(void)
{
    int n = home_n();
    if (app.side_focus || !n) { app.home_scroll = 0; return; }
    if (app.home_sel >= n) app.home_sel = n - 1;
    int k, kind = home_kind(app.home_sel, &k);
    if (kind == HK_FOLLOW || kind == HK_FOLLOW_MORE) { app.home_scroll = 0; return; }
    Box b = home_box(app.home_sel);
    int sec_x = kind == HK_GAME || kind == HK_GAME_MORE ? SEC_B_X : SEC_C_X;
    if (b.x - app.home_scroll < 30 || b.x + b.w - app.home_scroll > SCREEN_W - 20) app.home_scroll = sec_x - 30;
    if (b.x + b.w - app.home_scroll > SCREEN_W - 20) app.home_scroll = b.x + b.w - (SCREEN_W - 20);
}

static void draw_home_screen(void)
{
    static float cur;                                        /* slides toward app.home_scroll */
    cur += ((float)app.home_scroll - cur) * 0.22f;
    if (cur - app.home_scroll < 1 && app.home_scroll - cur < 1) cur = (float)app.home_scroll;
    int sx = (int)cur;

    /* menu and wordmark (they slide away with the rest) */
    if (sx < MENU_X + MENU_W) {
        draw_wordmark(MENU_X + 4 - sx, 62, 1.15f);
        for (int i = 0; i < menu_n(); i++) {
            Box b = menu_box(i);
            b.x -= sx;
            int a = menu_action(i);
            rect(b.x, b.y, b.w, b.h, C_HH_BTN);
            text(b.x + 18, b.y + b.h / 2 + 10, C_WHITE, 1.2f, ACTION_NAME[a]);
            if (a == A_FOLLOWING && app.lists[LIST_FOLLOWING].loaded) {   /* how many followed channels are live */
                char n[12];
                snprintf(n, sizeof n, "%d", app.lists[LIST_FOLLOWING].n);
                text_right(b.x + b.w - 22, b.y + b.h / 2 + 10, C_WHITE, 1.1f, n);
            }
            if (app.side_focus && i == app.side_sel) outline(b.x - 3, b.y - 3, b.w + 6, b.h + 6, 4, C_HH_SEL);
        }
    }
    /* section titles */
    text(MOS_X - sx, 64, C_TEXT, 1.1f, app.signed_in ? "Followed Channels" : "Top Channels");
    if (app.signed_in) {
        text(SEC_B_X - sx, 64, C_TEXT, 1.1f, "Games");
        text(SEC_C_X - sx, 64, C_TEXT, 1.1f, "Top Channels");
    }

    /* section A: the mosaic */
    if (!app.signed_in) {                                    /* waits for a sign-in */
        for (int i = 0; i < mosaic_tiles(); i++) { Box b = mosaic_box(i); if (i == mosaic_tiles() - 1) more_tile(b); else rect(b.x, b.y, b.w, b.h, C_HH_TILE); }
        Box big = mosaic_box(0);
        text_center_in(big.x, big.w, big.y + big.h / 2 - 12, C_TEXT, 1.0f, "Log in to see channels");
        text_center_in(big.x, big.w, big.y + big.h / 2 + 18, C_DIM, 0.8f, "Select Log In, then enter the code");
        text_center_in(big.x, big.w, big.y + big.h / 2 + 40, C_DIM, 0.8f, "at twitch.tv/activate on your phone or PC.");
        hint_pill(SCREEN_W - 20, 40, "START", "Options");
        return;
    }
    if (!sec_a_n()) {
        StreamList *L = mosaic_list();
        int loading = app_list_loading(LIST_FOLLOWING);
        Box big = mosaic_box(0);
        rect(big.x - sx, big.y, big.w, big.h, C_HH_TILE);
        char f[200];
        fit(f, sizeof f, loading || !L->loaded ? "Loading followed channels..." : L->err[0] ? L->err : "None of the channels you follow are live.", 0.9f, big.w - 30);
        text_center_in(big.x - sx, big.w, big.y + big.h / 2, L->err[0] && !loading ? C_WARN : C_DIM, 0.9f, f);
        if (loading || !L->loaded) spinner(big.x - sx + big.w / 2, big.y + big.h / 2 + 40, C_SEL);
    }
    if (!sec_b_n() && app.games.loaded == 0 && !app_games_loading()) app_load_games();   /* (the Games section) */
    if (!sec_b_n()) {
        Box b = sec_b_box(0);
        rect(b.x - sx, b.y, b.w, b.h, C_HH_TILE);
        text_center_in(b.x - sx, b.w, b.y + b.h / 2, C_DIM, 0.85f, app.games.err[0] ? "No games" : "Loading...");
    }
    if (!sec_c_n()) {
        Box b = sec_c_box(0);
        rect(b.x - sx, b.y, b.w * 2 + 10, b.h, C_HH_TILE);
        text_center_in(b.x - sx, b.w * 2 + 10, b.y + b.h / 2, C_DIM, 0.85f, "Loading top channels...");
    }

    /* every tile */
    int n = home_n();
    if (app.home_sel >= n) app.home_sel = n ? n - 1 : 0;
    for (int i = 0; i < n; i++) {
        Box b = home_box(i);
        b.x -= sx;
        if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
        int k, kind = home_kind(i, &k);
        switch (kind) {
        case HK_FOLLOW: channel_tile(&mosaic_list()->items[k], b, k == 0); break;
        case HK_TOP: channel_tile(&app.lists[LIST_FEATURED].items[k], b, 0); break;
        case HK_GAME: game_tile(&app.games.items[k], b, k == 0); break;
        default: more_tile(b); break;
        }
        if (!app.side_focus && i == app.home_sel) sel_frame(b);
    }
    hint_pill(SCREEN_W - 20, 40, "START", "Options");
}

static StreamList *page_list(void)
{
    switch (app.view) {
    case VIEW_FOLLOWING: return &app.lists[LIST_FOLLOWING];
    case VIEW_FEATURED: return &app.lists[LIST_FEATURED];
    case VIEW_SEARCH: return &app.lists[LIST_SEARCH];
    case VIEW_GAMES: return app.game_open ? &app.lists[LIST_GAME] : NULL;
    }
    return NULL;
}

static int page_list_id(void)
{
    return app.view == VIEW_FOLLOWING ? LIST_FOLLOWING : app.view == VIEW_FEATURED ? LIST_FEATURED
         : app.view == VIEW_SEARCH ? LIST_SEARCH : LIST_GAME;
}

static void draw_channel_page(void)
{
    StreamList *L = page_list();
    int list = page_list_id();
    const char *title = app.view == VIEW_FOLLOWING ? "Following" : app.view == VIEW_FEATURED ? "Top Channels"
                      : app.view == VIEW_SEARCH ? "Search" : app.game_name;
    char t[200];
    if (app.view == VIEW_SEARCH && app.search_query[0]) snprintf(t, sizeof t, "Search: %s", app.search_query);
    else snprintf(t, sizeof t, "%s", title);
    page_title(t, 1);
    if (app.view == VIEW_SEARCH) hint_pill(SCREEN_W - 230, 40, "[]", "New search");
    int loading = app_list_loading(list);
    if (!L->n) {
        const char *msg = loading ? "Loading..." : L->err[0] ? L->err
                        : list == LIST_SEARCH ? (app.search_query[0] ? "No channels found." : "Press X or Square to search for a channel.")
                        : list == LIST_FOLLOWING ? "None of the channels you follow are live right now." : "Nothing here right now.";
        draw_message(0, SCREEN_W, 280, msg, loading, L->err[0] && !loading);
        return;
    }
    if (L->sel >= L->n) L->sel = L->n - 1;
    keep_in_view(page_box(L->sel), &L->top);
    for (int i = 0; i < L->n; i++) {
        Box b = page_box(i);
        b.x -= L->top;
        if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
        channel_tile(&L->items[i], b, 0);
        if (i == L->sel) sel_frame(b);
    }
    if (loading) spinner(SCREEN_W - 40, 66, C_SEL);
}

static void draw_games_page(void)
{
    page_title("Games", 1);
    GameList *G = &app.games;
    int loading = app_games_loading();
    if (!G->n) {
        draw_message(0, SCREEN_W, 280, loading || !G->loaded ? "Loading games..." : G->err[0] ? G->err : "No categories right now.",
                     loading || !G->loaded, G->err[0] && !loading);
        return;
    }
    if (G->sel >= G->n) G->sel = G->n - 1;
    keep_in_view(games_box(G->sel), &G->top);
    for (int i = 0; i < G->n; i++) {
        Box b = games_box(i);
        b.x -= G->top;
        if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
        game_tile(&G->items[i], b, 0);
        if (i == G->sel) sel_frame(b);
    }
}

/* "Channels Playing <game>": the Follow button, then the channels in big tiles */
static void draw_category_page(void)
{
    char t[200];
    snprintf(t, sizeof t, "Channels Playing %s", app.game_name);
    page_title(t, 1);
    int fol = app_cat_followed();
    rect(FOLLOW_BTN_X, FOLLOW_BTN_Y, 197, 60, fol ? RGBA8(60, 50, 90, 255) : C_HH_BTN);
    text_center_in(FOLLOW_BTN_X, 197, FOLLOW_BTN_Y + 40, C_WHITE, 1.15f, fol ? "Following" : "Follow");
    if (app.cat_btn_focus) outline(FOLLOW_BTN_X, FOLLOW_BTN_Y, 197, 60, 4, C_HH_SEL);
    StreamList *L = &app.lists[LIST_GAME];
    int loading = app_list_loading(LIST_GAME);
    if (!L->n) {
        draw_message(0, SCREEN_W, 340, loading || !L->loaded ? "Loading channels..." : L->err[0] ? L->err : "Nobody is streaming this right now.",
                     loading || !L->loaded, L->err[0] && !loading);
        return;
    }
    if (L->sel >= L->n) L->sel = L->n - 1;
    keep_in_view(cat_box(L->sel), &L->top);
    for (int i = 0; i < L->n; i++) {
        Box b = cat_box(i);
        b.x -= L->top;
        if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
        channel_tile(&L->items[i], b, 1);
        if (i == L->sel && !app.cat_btn_focus) sel_frame(b);
    }
}

/* "Highlights & Past Broadcasts" */
static void vod_tile(const TwVideo *v, Box b)
{
    if (v->thumb[0]) draw_picture(img_get(v->thumb), b.x, b.y, b.w, b.h);
    else rect(b.x, b.y, b.w, b.h, C_HH_TILE);
    rect(b.x, b.y + b.h - 64, b.w, 64, RGBA8(12, 8, 22, 150));
    char f[220], d[24], ago[32], line[80];
    fit(f, sizeof f, v->title[0] ? v->title : "(untitled)", 1.0f, b.w - 24);
    text(b.x + 12, b.y + b.h - 34, C_WHITE, 1.0f, f);
    fmt_duration(d, sizeof d, v->duration);
    fmt_ago(ago, sizeof ago, v->created_at);
    if (v->views >= 0) { char vw[24]; fmt_count(vw, sizeof vw, v->views); snprintf(line, sizeof line, "%s views | %s | %s", vw, ago, d); }
    else snprintf(line, sizeof line, "%s | %s", ago, d);
    text(b.x + 12, b.y + b.h - 10, C_HH_GAME, 0.82f, line);
}

static VideoList *videos_active(void) { return app.view == VIEW_CLIPS ? &app.clips : &app.vods; }

static void draw_vods_page(void)
{
    int clips = app.view == VIEW_CLIPS;
    page_title(clips ? "Clips" : "Highlights & Past Broadcasts", 1);
    hint_pill(SCREEN_W - 320, 40, "[]", clips ? "Past broadcasts" : "Clips");
    VideoList *V = videos_active();
    if (!V->n) {
        draw_message(0, SCREEN_W, 280, !V->loaded ? "Loading..." : V->err[0] ? V->err : (clips ? "This channel has no clips." : "This channel has no saved videos."),
                     !V->loaded, V->err[0] != 0);
        return;
    }
    if (V->sel >= V->n) V->sel = V->n - 1;
    keep_in_view(vod_box(V->sel), &V->top);
    for (int i = 0; i < V->n; i++) {
        Box b = vod_box(i);
        b.x -= V->top;
        if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
        vod_tile(&V->items[i], b);
        if (i == V->sel) sel_frame(b);
    }
}

/* ---------------------------------------------------------------- the Following page (handheld)
 * "Followed Channels / Sort By : Live": one row that slides, the selected channel bigger and framed, the others
 * dimmed; START opens the sorting options (Live: newest streams first, Offline, View Count). */

static void go_home(void);

static const char *const FSORT_NAME[FSORT_COUNT] = { "Live", "Offline", "View Count" };

#define FW 400                          /* a tile */
#define FH 225
#define FSW 460                         /* the selected one */
#define FSH 259
#define FGAP 24
#define FX 38                           /* where the selected tile sits */
#define FCY 330                         /* the row's middle */

static StreamList *follow_list(void) { return &app.lists[app.follow_sort == FSORT_OFFLINE ? LIST_FOLLOW_OFF : LIST_FOLLOWING]; }

static const StreamList *g_sort_src;
static int cmp_started(const void *a, const void *b)       /* newest first */
{
    return strcmp(g_sort_src->items[*(const int *)b].started_at, g_sort_src->items[*(const int *)a].started_at);
}
static int cmp_viewers(const void *a, const void *b)
{
    return g_sort_src->items[*(const int *)b].viewers - g_sort_src->items[*(const int *)a].viewers;
}

/* the list in the chosen order: ord[i] = index in the list */
static int follow_order(int *ord)
{
    StreamList *L = follow_list();
    for (int i = 0; i < L->n; i++) ord[i] = i;
    g_sort_src = L;
    if (app.follow_sort == FSORT_LIVE) qsort(ord, (size_t)L->n, sizeof *ord, cmp_started);
    else if (app.follow_sort == FSORT_VIEWERS) qsort(ord, (size_t)L->n, sizeof *ord, cmp_viewers);
    return L->n;
}

static void offline_tile(const TwStream *s, Box b)
{
    rect(b.x, b.y, b.w, b.h, C_HH_TILE);
    int a = b.h * 55 / 100;
    if (s->avatar[0]) draw_picture(img_get(s->avatar), b.x + (b.w - a) / 2, b.y + 18, a, a);
    else rect(b.x + (b.w - a) / 2, b.y + 18, a, a, C_PANEL_L);
    tile_caption(b.x, b.y, b.w, b.h, s->name, "", -1, 0);
    text_right(b.x + b.w - 12, b.y + b.h - 30, C_DIM, 0.8f, s->saved ? "Offline  |  Saved" : "Offline");
}

/* x of tile at distance d (in tiles) from the selected one; pos is fractional while sliding */
static float follow_x(float d)
{
    if (d >= 0) return d <= 1 ? FX + d * (FSW + FGAP) : FX + (FSW + FGAP) + (d - 1) * (FW + FGAP);
    return FX + d * (FW + FGAP);
}

static void draw_following_page(void)
{
    text(FX, 46, C_TEXT, 0.95f, "Followed Channels");
    char s[60];
    snprintf(s, sizeof s, "Sort By : %s", FSORT_NAME[app.follow_sort]);
    text(FX, 72, C_DIM, 0.8f, s);
    hint_pill(SCREEN_W - 20, 40, "START", "Sorting Options");
    hint_pill(SCREEN_W - 200, 40, "O", "Back");
    StreamList *L = follow_list();
    int list = app.follow_sort == FSORT_OFFLINE ? LIST_FOLLOW_OFF : LIST_FOLLOWING;
    int loading = app_list_loading(list);
    if (!L->loaded && !loading) app_load_list(list);
    static int ord[MAX_STREAMS];
    int n = follow_order(ord);
    if (!n) {
        draw_message(0, SCREEN_W, FCY, loading || !L->loaded ? "Loading..."
                     : !app.signed_in ? "Log in to see the channels you follow on Twitch, or pin channels here with the heart."
                     : L->err[0] ? L->err
                     : app.follow_sort == FSORT_OFFLINE ? "Everyone you follow is live right now." : "None of the channels you follow are live right now.",
                     loading || !L->loaded, L->err[0] && !loading && app.signed_in);
    } else {
        if (L->sel >= n) L->sel = n - 1;
        static float cur;                                    /* slides toward the selection */
        cur += ((float)L->sel - cur) * 0.25f;
        if (cur - L->sel < 0.01f && L->sel - cur < 0.01f) cur = (float)L->sel;
        for (int i = 0; i < n; i++) {
            float d = (float)i - cur;
            if (d < -3 || d > 4) continue;
            float t = d < 0 ? -d : d;                        /* how far from the middle: 0 = selected size */
            if (t > 1) t = 1;
            int w = (int)(FSW + (FW - FSW) * t), h = (int)(FSH + (FH - FSH) * t);
            Box b = { (int)follow_x(d), FCY - h / 2, w, h };
            if (b.x + b.w < 0 || b.x > SCREEN_W) continue;
            const TwStream *st = &L->items[ord[i]];
            if (st->live) channel_tile(st, b, 1); else offline_tile(st, b);
            if (i != L->sel) rect(b.x, b.y, b.w, b.h, RGBA8(10, 6, 18, 120));   /* the others dimmed */
            else sel_frame(b);
        }
    }
    if (app.sort_menu_open) {                                /* sorting options */
        int w = 340, h = 60 + FSORT_COUNT * 50, x = SCREEN_W - w - 20, y = 60;
        rect(x, y, w, h, RGBA8(28, 22, 40, 245));
        outline(x, y, w, h, 1, C_PANEL_L);
        text(x + 20, y + 36, C_TEXT, 0.95f, "Sort By");
        for (int i = 0; i < FSORT_COUNT; i++) {
            int ry = y + 50 + i * 50;
            if (i == app.sort_menu_sel) { rect(x + 8, ry, w - 16, 44, C_HH_BTN); }
            text(x + 26, ry + 30, C_WHITE, 0.95f, FSORT_NAME[i]);
            if (i == app.follow_sort) text_right(x + w - 26, ry + 30, C_WHITE, 0.85f, "current");
        }
    }
}

static void following_input(const Input *in)
{
    uint32_t p = in->press;
    StreamList *L = follow_list();
    if (app.sort_menu_open) {
        if (p & SCE_CTRL_UP && app.sort_menu_sel > 0) app.sort_menu_sel--;
        if (p & SCE_CTRL_DOWN && app.sort_menu_sel < FSORT_COUNT - 1) app.sort_menu_sel++;
        int pick = -1;
        if (p & SCE_CTRL_CROSS) pick = app.sort_menu_sel;
        if (in->tap) {
            int x = SCREEN_W - 360, i = (in->tap_y - 110) / 50;
            if (in->tap_x >= x && in->tap_y >= 110 && i >= 0 && i < FSORT_COUNT) pick = i;
            else app.sort_menu_open = 0;
        }
        if (pick >= 0) {
            app.sort_menu_open = 0;
            if (pick != app.follow_sort) {
                app.follow_sort = pick;
                follow_list()->sel = 0;
                if (pick == FSORT_OFFLINE) app_load_list(LIST_FOLLOW_OFF);   /* (always fresh) */
            }
        }
        if (p & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) app.sort_menu_open = 0;
        return;
    }
    if (p & SCE_CTRL_CIRCLE) { go_home(); return; }
    if (p & SCE_CTRL_START) { app.sort_menu_open = 1; app.sort_menu_sel = app.follow_sort; return; }
    static int ord[MAX_STREAMS];
    int n = follow_order(ord);
    if (p & SCE_CTRL_LEFT && L->sel > 0) L->sel--;
    if (p & SCE_CTRL_RIGHT && L->sel < n - 1) L->sel++;
    if (p & SCE_CTRL_TRIANGLE) app_load_list(app.follow_sort == FSORT_OFFLINE ? LIST_FOLLOW_OFF : LIST_FOLLOWING);
    int k = -1;
    if ((p & SCE_CTRL_CROSS) && n) k = L->sel;
    if (in->tap && n) {                                      /* the selected tile, or a neighbour */
        if (in->tap_x < FX && L->sel > 0) L->sel--;
        else if (in->tap_x < FX + FSW) k = L->sel;
        else if (L->sel < n - 1) L->sel++;
    }
    static int acc, last_x = -1;                             /* swipes move along the row */
    if (in->touching) {
        if (last_x >= 0) acc += in->touch_x - last_x;
        last_x = in->touch_x;
        while (acc <= -120) { acc += 120; if (L->sel < n - 1) L->sel++; }
        while (acc >= 120) { acc -= 120; if (L->sel > 0) L->sel--; }
    } else { last_x = -1; acc = 0; }
    if (k >= 0 && k < n) {
        const TwStream *s = &L->items[ord[k]];
        if (s->live) app_open_stream(s);
        else { char m[120]; snprintf(m, sizeof m, "%s is offline right now.", s->name); app_set_status(m); }
    }
}

/* ---------------------------------------------------------------- the profile page (handheld) */

#define PF_BTN_X 236
#define PF_BTN_Y 262
#define PF_CH_Y  290            /* Followed Channels row */
#define PF_CH_S  74
#define PF_CAT_Y 406            /* Followed Categories row */
#define PF_CAT_W 68
#define PF_CAT_H 90
#define PF_TILE_G 10
/* profile sections (prof_focus): 0 buttons (signed out), 1 channels, 2 categories */
enum { PFS_BTN = 0, PFS_CH = 1, PFS_CAT = 2 };

static int prof_buttons(void) { return app.signed_in ? 0 : 1; }   /* signed out: Log In (Options: START) */

static void draw_profile_page(void)
{
    page_title("Profile", 1);
    const TwUser *u = &app.me;
    app_load_me();
    rect(36, 86, 168, 168, C_WHITE);
    if (app.me_loaded && u->avatar[0]) draw_picture(img_get(u->avatar), 40, 90, 160, 160);
    else rect(40, 90, 160, 160, C_HH_TILE);
    char f[300], line[200];
    if (!app.signed_in) {
        text(PF_BTN_X, 126, C_WHITE, 1.4f, "Not signed in");
        draw_wrapped(PF_BTN_X, 160, 680, 0.85f, C_DIM, "Log in to see your profile, follow categories and chat. You'll enter a code at twitch.tv/activate.", 3, 24);
    } else {
        fit(f, sizeof f, app.me_loaded ? u->name : tw_user(), 1.5f, 680);
        text(PF_BTN_X, 126, C_WHITE, 1.5f, f);
        draw_wrapped(PF_BTN_X, 160, 680, 0.82f, C_DIM, app.me_loaded ? (u->description[0] ? u->description : "No bio yet.") : "Loading your profile...", 3, 23);
        if (app.me_loaded) {
            char a[32] = "-", b[32] = "-", j[40] = "";
            if (u->followers >= 0) fmt_count(a, sizeof a, u->followers);
            if (u->following_total >= 0) fmt_count(b, sizeof b, u->following_total);
            fmt_joined(j, sizeof j, u->created_at);
            snprintf(line, sizeof line, "%s followers    |    following %s    |    %s", a, b, j);
            text(PF_BTN_X, 262, C_HH_GAME, 0.85f, line);
        }
    }
    /* Log In button (signed out) */
    if (!prof_buttons() && app.prof_focus == PFS_BTN) app.prof_focus = favchan_count() ? PFS_CH : PFS_CAT;
    if (prof_buttons()) {
        rect(PF_BTN_X, PF_BTN_Y, 197, 56, C_HH_BTN);
        text_center_in(PF_BTN_X, 197, PF_BTN_Y + 37, C_WHITE, 1.1f, "Log In");
        if (app.prof_focus == PFS_BTN) outline(PF_BTN_X, PF_BTN_Y, 197, 56, 4, C_HH_SEL);
    }
    const char *selname = NULL;
    /* Local Pins: channels saved on this Vita (your real Twitch follows are under "Followed Channels" on the home screen) */
    text(40, PF_CH_Y - 12, C_TEXT, 0.95f, "Local Pins (this Vita)");
    int nc = favchan_count();
    if (!nc) text(40, PF_CH_Y + 42, C_FAINT, 0.78f, "Press the heart on a stream to pin it here. Your Twitch account follows show under Followed Channels.");
    for (int i = 0; i < nc; i++) {
        Box b = { 40 + i * (PF_CH_S + PF_TILE_G), PF_CH_Y, PF_CH_S, PF_CH_S };
        if (b.x + b.w > SCREEN_W - 20) { text(b.x, b.y + 40, C_FAINT, 0.8f, "..."); break; }
        const TwStream *s = favchan_at(i);
        if (!s) break;
        if (s->avatar[0]) draw_picture(img_get(s->avatar), b.x, b.y, b.w, b.h);
        else { rect(b.x, b.y, b.w, b.h, C_HH_TILE); char ini[2] = { s->name[0] ? s->name[0] : '?', 0 }; text_center_in(b.x, b.w, b.y + b.h / 2 + 10, C_PANEL_L, 1.6f, ini); }
        if (app.prof_focus == PFS_CH && app.prof_col == i) { sel_frame(b); selname = s->name; }
    }
    /* Followed Categories (saved on this Vita) */
    text(40, PF_CAT_Y - 12, C_TEXT, 0.95f, "Followed Categories");
    int n = favcat_count();
    if (!n) text(40, PF_CAT_Y + 40, C_FAINT, 0.78f, "Open a category in Games and press Follow to save it here.");
    for (int i = 0; i < n; i++) {
        Box b = { 40 + i * (PF_CAT_W + PF_TILE_G), PF_CAT_Y, PF_CAT_W, PF_CAT_H };
        if (b.x + b.w > SCREEN_W - 20) { text(b.x, b.y + 40, C_FAINT, 0.8f, "..."); break; }
        const TwGame *g = favcat_get(i);
        draw_picture(img_get(g->box_art), b.x, b.y, b.w, b.h);
        if (app.prof_focus == PFS_CAT && app.prof_col == i) { sel_frame(b); selname = g->name; }
    }
    if (selname) { fit(f, sizeof f, selname, 0.8f, 600); text_right(SCREEN_W - 24, PF_CAT_Y + PF_CAT_H, C_HH_GAME, 0.8f, f); }
}

/* the sections present now, top to bottom */
static int prof_sections(int *secs)
{
    int n = 0;
    if (prof_buttons()) secs[n++] = PFS_BTN;
    secs[n++] = PFS_CH;                                       /* always shown (may be empty with a hint) */
    secs[n++] = PFS_CAT;
    return n;
}
static int prof_row_count(int sec) { return sec == PFS_BTN ? prof_buttons() : sec == PFS_CH ? favchan_count() : favcat_count(); }

static void prof_open(int sec, int col)
{
    if (sec == PFS_BTN) { if (!app.signed_in) app_login(); return; }
    if (sec == PFS_CH) {
        const TwStream *s = favchan_at(col);
        if (s) app_open_stream(s);                            /* offline -> a message, as elsewhere */
        return;
    }
    const TwGame *g = favcat_get(col);
    if (g) { app.view = VIEW_GAMES; app.cat_btn_focus = 0; app_open_game(g->id, g->name); }
}

static void profile_input(const Input *in)
{
    uint32_t p = in->press;
    if (p & SCE_CTRL_CIRCLE) { go_home(); return; }
    if (p & SCE_CTRL_START) { app.options_from_profile = 1; app.view = VIEW_SETTINGS; return; }
    int secs[3], ns = prof_sections(secs);
    int cur = 0;
    for (int i = 0; i < ns; i++) if (secs[i] == app.prof_focus) cur = i;
    if (p & SCE_CTRL_UP && cur > 0) { app.prof_focus = secs[--cur]; if (app.prof_col >= prof_row_count(app.prof_focus)) app.prof_col = 0; }
    if (p & SCE_CTRL_DOWN && cur < ns - 1) { app.prof_focus = secs[++cur]; if (app.prof_col >= prof_row_count(app.prof_focus)) app.prof_col = 0; }
    int lim = prof_row_count(app.prof_focus);
    if (p & SCE_CTRL_LEFT && app.prof_col > 0) app.prof_col--;
    if (p & SCE_CTRL_RIGHT && app.prof_col + 1 < lim) app.prof_col++;
    int act = (p & SCE_CTRL_CROSS) ? 1 : 0;
    if (in->tap) {                                           /* tap a tile or the button */
        if (prof_buttons() && in->tap_x >= PF_BTN_X && in->tap_x < PF_BTN_X + 197 && in->tap_y >= PF_BTN_Y && in->tap_y < PF_BTN_Y + 56) { app.prof_focus = PFS_BTN; app.prof_col = 0; act = 1; }
        for (int i = 0; i < favchan_count(); i++) {
            int x = 40 + i * (PF_CH_S + PF_TILE_G);
            if (in->tap_x >= x && in->tap_x < x + PF_CH_S && in->tap_y >= PF_CH_Y && in->tap_y < PF_CH_Y + PF_CH_S) { app.prof_focus = PFS_CH; app.prof_col = i; act = 1; }
        }
        for (int i = 0; i < favcat_count(); i++) {
            int x = 40 + i * (PF_CAT_W + PF_TILE_G);
            if (in->tap_x >= x && in->tap_x < x + PF_CAT_W && in->tap_y >= PF_CAT_Y && in->tap_y < PF_CAT_Y + PF_CAT_H) { app.prof_focus = PFS_CAT; app.prof_col = i; act = 1; }
        }
    }
    if (act && app.prof_col < prof_row_count(app.prof_focus)) prof_open(app.prof_focus, app.prof_col);
}

void hh_draw(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, C_HH_BG);
    if (app.view == VIEW_HOME) draw_home_screen();
    else if (app.view == VIEW_SETTINGS) {
        char t[100];
        snprintf(t, sizeof t, "Options");
        page_title(t, 1);
        home_settings_draw(0, 70);
    }
    else if (app.view == VIEW_GAMES && !app.game_open) draw_games_page();
    else if (app.view == VIEW_GAMES) draw_category_page();
    else if (app.view == VIEW_VODS || app.view == VIEW_CLIPS) draw_vods_page();
    else if (app.view == VIEW_FOLLOWING) draw_following_page();
    else if (app.view == VIEW_PROFILE) draw_profile_page();
    else draw_channel_page();
    dialog_draw();
}

/* ---------------------------------------------------------------- input */

static void open_view(int v)
{
    app.view = v;
    app.game_open = 0;
    if (v == VIEW_FOLLOWING && !app.lists[LIST_FOLLOWING].loaded) app_load_list(LIST_FOLLOWING);
    if (v == VIEW_FEATURED && !app.lists[LIST_FEATURED].loaded) app_load_list(LIST_FEATURED);
    if (v == VIEW_GAMES && !app.games.loaded) app_load_games();
    if (v == VIEW_SEARCH && !app.search_query[0]) app_search_prompt();
}

static void go_home(void)
{
    app.view = VIEW_HOME;
    app.game_open = 0;
}

static void watch(const TwStream *s)
{
    if (s->live) app_open_stream(s);
    else app_set_status("That channel is offline right now.");
}

static Box home_box_n(int i) { return home_box(i); }

static void menu_do(int a)
{
    switch (a) {
    case A_PROFILE: open_view(VIEW_PROFILE); break;
    case A_SEARCH: if (app.signed_in) open_view(VIEW_SEARCH); else app_set_status("Log in first: select Log In."); break;
    case A_FOLLOWING: open_view(VIEW_FOLLOWING); break;
    case A_GAMES: if (app.signed_in) open_view(VIEW_GAMES); else app_set_status("Log in first: select Log In."); break;
    case A_CHANNELS: if (app.signed_in) open_view(VIEW_FEATURED); else app_set_status("Log in first: select Log In."); break;
    case A_LOGIN: app_login(); break;
    }
}

static void open_home_tile(int i)
{
    int k, kind = home_kind(i, &k);
    switch (kind) {
    case HK_FOLLOW: watch(&mosaic_list()->items[k]); break;
    case HK_FOLLOW_MORE: open_view(VIEW_FOLLOWING); break;
    case HK_GAME: app.games.sel = k; app.cat_btn_focus = 0; app.view = VIEW_GAMES; app_open_game(app.games.items[k].id, app.games.items[k].name); break;
    case HK_GAME_MORE: open_view(VIEW_GAMES); break;
    case HK_TOP: watch(&app.lists[LIST_FEATURED].items[k]); break;
    case HK_TOP_MORE: open_view(VIEW_FEATURED); break;
    }
}

static void home_screen_input(const Input *in)
{
    uint32_t p = in->press;
    int n = home_n();
    if (!n) app.side_focus = 1;                              /* nothing to pick on the right: the menu */
    if (in->tap) {                                           /* touch: a menu button or a tile */
        for (int i = 0; i < menu_n() && app.home_scroll < MENU_X + MENU_W; i++) {
            Box b = menu_box(i);
            b.x -= app.home_scroll;
            if (in->tap_x >= b.x && in->tap_x < b.x + b.w && in->tap_y >= b.y && in->tap_y < b.y + b.h) { app.side_sel = i; menu_do(menu_action(i)); return; }
        }
        int k = hit(home_box_n, n, app.home_scroll, in->tap_x, in->tap_y);
        if (k >= 0) { app.home_sel = k; app.side_focus = 0; open_home_tile(k); }
        return;
    }
    if (app.side_sel >= menu_n()) app.side_sel = menu_n() - 1;
    if (app.side_focus) {
        if (p & SCE_CTRL_UP && app.side_sel > 0) app.side_sel--;
        if (p & SCE_CTRL_DOWN && app.side_sel < menu_n() - 1) app.side_sel++;
        if (p & SCE_CTRL_RIGHT && n) {                       /* into the tile beside the button */
            app.side_focus = 0;
            Box m = menu_box(app.side_sel);
            int best = 0, cy = m.y + m.h / 2;
            for (int i = 0; i < n && i < sec_a_n(); i++) {
                Box b = home_box(i), bb = home_box(best);
                if (abs(b.y + b.h / 2 - cy) + b.x / 4 < abs(bb.y + bb.h / 2 - cy) + bb.x / 4) best = i;
            }
            app.home_sel = best;
        }
        if (p & SCE_CTRL_CROSS) menu_do(menu_action(app.side_sel));
    } else {
        if (tiles_move(home_box_n, n, &app.home_sel, p) < 0) {
            /* Left out of the first section: the menu button beside the tile */
            Box b = home_box(app.home_sel);
            int cy = b.y + b.h / 2, best = 0;
            for (int i = 0; i < menu_n(); i++) { Box m = menu_box(i); if (abs(m.y + m.h / 2 - cy) < abs(menu_box(best).y + menu_box(best).h / 2 - cy)) best = i; }
            app.side_sel = best;
            app.side_focus = 1;
        }
        if (p & SCE_CTRL_CROSS && n) open_home_tile(app.home_sel);
    }
    if (p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER) && n && app.signed_in) {   /* L / R: jump between sections */
        int a = sec_a_n(), b = sec_b_n(), k, kind = home_kind(app.side_focus ? 0 : app.home_sel, &k);
        int sec = kind <= HK_FOLLOW_MORE ? 0 : kind <= HK_GAME_MORE ? 1 : 2;
        if (app.side_focus) sec = -1;
        sec += (p & SCE_CTRL_RTRIGGER) ? 1 : -1;
        if (sec < 0) { app.side_focus = 1; }
        else {
            int first[3] = { 0, a, a + b }, cnt[3] = { a, b, sec_c_n() };
            while (sec <= 2 && !cnt[sec]) sec += (p & SCE_CTRL_RTRIGGER) ? 1 : -1;
            if (sec >= 0 && sec <= 2) { app.side_focus = 0; app.home_sel = first[sec]; }
        }
    }
    if (p & SCE_CTRL_TRIANGLE && app.signed_in) { app_load_list(LIST_FOLLOWING); app_load_list(LIST_FEATURED); app_load_games(); }
    if (p & SCE_CTRL_START) open_view(VIEW_SETTINGS);
    if (p) home_follow_selection();
}

/* sideways swipes on the home screen slide it */
static void home_swipe(const Input *in)
{
    static int last_x = -1;
    if (!in->touching) { last_x = -1; return; }
    if (last_x >= 0) {
        int maxs = SEC_C_X + 2 * (PG_W + 10) - SCREEN_W + 20;
        app.home_scroll -= in->touch_x - last_x;
        if (app.home_scroll < 0) app.home_scroll = 0;
        if (app.home_scroll > maxs) app.home_scroll = maxs;
    }
    last_x = in->touch_x;
}

static void page_input(const Input *in)
{
    uint32_t p = in->press;
    if (dialog_input(in)) return;
    if (p & SCE_CTRL_CIRCLE) {
        if (app.view == VIEW_SETTINGS && app.options_from_profile) { app.options_from_profile = 0; app.view = VIEW_PROFILE; return; }
        if (app.view == VIEW_GAMES && app.game_open) app_close_game();
        else if (app.view == VIEW_VODS) app_vods_back();
        else if (app.view == VIEW_CLIPS) app_clips_back();
        else go_home();
        return;
    }
    if (p & SCE_CTRL_START) { if (app.view == VIEW_SETTINGS) go_home(); else open_view(VIEW_SETTINGS); return; }
    if (app.view == VIEW_SETTINGS) { home_settings_input(in, 70); return; }

    if (app.view == VIEW_GAMES && !app.game_open) {
        GameList *G = &app.games;
        tiles_move(games_box, G->n, &G->sel, p);
        int k = -1;
        if (p & SCE_CTRL_CROSS) k = G->sel;
        if (in->tap) k = hit(games_box, G->n, G->top, in->tap_x, in->tap_y);
        if (k >= 0 && k < G->n) { G->sel = k; app.cat_btn_focus = 0; app_open_game(G->items[k].id, G->items[k].name); }
        if (p & SCE_CTRL_TRIANGLE) app_load_games();
        return;
    }
    if (app.view == VIEW_GAMES && app.game_open) {           /* Follow button + one row of channels */
        StreamList *L = &app.lists[LIST_GAME];
        int follow = 0, k = -1;
        if (app.cat_btn_focus) {
            if ((p & SCE_CTRL_DOWN) && L->n) app.cat_btn_focus = 0;
            if (p & SCE_CTRL_CROSS) follow = 1;
        } else {
            if (p & SCE_CTRL_UP) app.cat_btn_focus = 1;
            else tiles_move(cat_box, L->n, &L->sel, p);
            if ((p & SCE_CTRL_CROSS) && L->n) k = L->sel;
        }
        if (in->tap && in->tap_x >= FOLLOW_BTN_X && in->tap_x < FOLLOW_BTN_X + 197 && in->tap_y >= FOLLOW_BTN_Y && in->tap_y < FOLLOW_BTN_Y + 60) follow = 1;
        else if (in->tap) k = hit(cat_box, L->n, L->top, in->tap_x, in->tap_y);
        if (follow) { if (app.signed_in) app_toggle_cat_follow(); else { app.dialog = DLG_SIGNIN; app.dlg_sel = 0; } }
        if (k >= 0 && k < L->n) { L->sel = k; app.cat_btn_focus = 0; watch(&L->items[k]); }
        if (p & SCE_CTRL_TRIANGLE) app_load_list(LIST_GAME);
        return;
    }
    if (app.view == VIEW_VODS || app.view == VIEW_CLIPS) {
        if (p & SCE_CTRL_SQUARE) {                           /* switch Past Broadcasts <-> Clips */
            if (app.view == VIEW_VODS) app_open_clips(); else app_open_vods();
            return;
        }
        VideoList *V = videos_active();
        tiles_move(vod_box, V->n, &V->sel, p);
        int k = -1;
        if (p & SCE_CTRL_CROSS) k = V->sel;
        if (in->tap) k = hit(vod_box, V->n, V->top, in->tap_x, in->tap_y);
        if (k >= 0 && k < V->n) { V->sel = k; if (app.view == VIEW_CLIPS) app_play_clip(&V->items[k]); else app_play_vod(&V->items[k]); }
        return;
    }
    StreamList *L = page_list();
    if (!L) return;
    tiles_move(page_box, L->n, &L->sel, p);
    int k = -1;
    if (p & SCE_CTRL_CROSS) {
        if (app.view == VIEW_SEARCH && !L->n) { app_search_prompt(); return; }
        k = L->sel;
    }
    if (p & SCE_CTRL_SQUARE && app.view == VIEW_SEARCH) app_search_prompt();
    if (in->tap) k = hit(page_box, L->n, L->top, in->tap_x, in->tap_y);
    if (k >= 0 && k < L->n) { L->sel = k; watch(&L->items[k]); }
    if (p & SCE_CTRL_TRIANGLE) app_load_list(page_list_id());
}

/* sideways swipes scroll the pages; the selection moves along so it stays on screen */
static void swipe(const Input *in)
{
    static int last_x = -1;
    if (!in->touching) { last_x = -1; return; }
    if (last_x >= 0 && app.view != VIEW_HOME && app.view != VIEW_SETTINGS) {
        int dx = in->touch_x - last_x;
        if (app.view == VIEW_GAMES && !app.game_open) {
            GameList *G = &app.games;
            G->top -= dx;
            if (G->top < 0) G->top = 0;
            if (G->n) {                                      /* keep the selection on screen */
                Box b = games_box(G->sel);
                int col = -1, row = G->sel % 2;
                if (b.x < G->top) col = (G->top + GM_W + GM_G - 1) / (GM_W + GM_G);
                else if (b.x + b.w > G->top + SCREEN_W) col = (G->top + SCREEN_W - PG_X) / (GM_W + GM_G) - 1;
                if (col >= 0) { int s = col * 2 + row; G->sel = s < G->n ? s : G->n - 1; }
            }
        } else if (app.view == VIEW_VODS || app.view == VIEW_CLIPS || (app.view == VIEW_GAMES && app.game_open)) {
            int vids = (app.view == VIEW_VODS || app.view == VIEW_CLIPS);
            VideoList *VL = videos_active();
            int *top = vids ? &VL->top : &app.lists[LIST_GAME].top;
            int *sel = vids ? &VL->sel : &app.lists[LIST_GAME].sel;
            int n = vids ? VL->n : app.lists[LIST_GAME].n;
            int rows = vids ? 2 : 1, cw = vids ? VD_W + VD_G : CT_W + CT_G;
            *top -= dx;
            if (*top < 0) *top = 0;
            if (n) {
                int col = (*sel / rows), row = *sel % rows, x = PG_X + col * cw;
                if (x < *top) col = (*top + cw - 1) / cw;
                else if (x + cw > *top + SCREEN_W) col = (*top + SCREEN_W - PG_X) / cw - 1;
                if (col < 0) col = 0;
                int s = col * rows + row;
                *sel = s < n ? s : n - 1;
            }
        } else if (page_list()) {
            StreamList *L = page_list();
            L->top -= dx;
            if (L->top < 0) L->top = 0;
            if (L->n) {
                Box b = page_box(L->sel);
                int row = L->sel % 3, col;
                if (b.x < L->top) col = (L->top + PG_W + PG_G - 1) / (PG_W + PG_G);
                else if (b.x + b.w > L->top + SCREEN_W) col = (L->top + SCREEN_W - PG_X) / (PG_W + PG_G) - 1;
                else col = -1;
                if (col >= 0) {
                    int s = col * 3 + row;
                    L->sel = s < L->n ? s : L->n - 1;
                }
            }
        }
    }
    last_x = in->touch_x;
}

void hh_input(const Input *in)
{
    if (app.view == VIEW_FOLLOWING) { if (!dialog_input(in)) following_input(in); return; }
    if (app.view == VIEW_PROFILE) { if (!dialog_input(in)) profile_input(in); return; }
    if (app.view == VIEW_HOME) { if (!dialog_input(in)) { home_screen_input(in); if (app.signed_in) home_swipe(in); } }
    else { page_input(in); swipe(in); }
}
