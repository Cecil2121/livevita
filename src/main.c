/* LiveVita: an unofficial Twitch viewer for the PS Vita and PS TV (homebrew).
 * main.c: start-up, the app state, network jobs on a worker thread, and switching between the screens
 * (sign-in, home in the handheld or TV layout, player). Chat and pictures have threads of their own. */
#include "app.h"
#include "applog.h"
#include "chat.h"
#include "favcat.h"
#include "favchan.h"
#include "tpemote.h"
#include "home.h"
#include "imgcache.h"
#include "playerui.h"
#include "twitch.h"
#include "ui.h"
#include "player/curlio.h"
#include "player/hls.h"
#include "player/nettls.h"
#include "player/tsplayer.h"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef LV_CLIENT_ID
#define LV_CLIENT_ID ""                 /* set at build time (CMake -DLV_CLIENT_ID=...), or put it in client_id.txt */
#endif

#define NET_POOL_SIZE (4 * 1024 * 1024)
#define SETTINGS_PATH DATA_DIR "/settings.txt"
#define CLIENT_ID_PATH DATA_DIR "/client_id.txt"

App app;
vita2d_pgf *g_font;

static unsigned now_ms(void) { return (unsigned)(sceKernelGetProcessTimeWide() / 1000); }

/* ---------------------------------------------------------------- settings */

static void load_settings(void)
{
    app.quality = TWQ_480P30;
    app.layout = LAYOUT_AUTO;
    app.chat_on = 1;
    app.theater = 0;                                         /* full screen, as the classic app (Square: video + chat) */
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) return;
    char line[80];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "quality=%d", &v) == 1 && v >= 0 && v < TWQ_COUNT) app.quality = v;
        else if (sscanf(line, "layout=%d", &v) == 1 && v >= 0 && v < LAYOUT_COUNT) app.layout = v;
        else if (sscanf(line, "chat=%d", &v) == 1) app.chat_on = v != 0;
        else if (sscanf(line, "theater=%d", &v) == 1) app.theater = v != 0;
    }
    fclose(f);
}

void app_save_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "quality=%d\nlayout=%d\nchat=%d\ntheater=%d\n", app.quality, app.layout, app.chat_on, app.theater);
    fclose(f);
}

static void load_client_id(void)
{
    char id[64] = LV_CLIENT_ID;
    FILE *f = fopen(CLIENT_ID_PATH, "r");
    if (f) {
        char line[64] = "";
        if (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n \t")] = 0;
            if (line[0]) snprintf(id, sizeof id, "%s", line);
        }
        fclose(f);
    }
    tw_set_client_id(id);
}

static void apply_layout(void)
{
    UIMode m = app.layout == LAYOUT_TV ? UI_MODE_TV : app.layout == LAYOUT_HANDHELD ? UI_MODE_HANDHELD : app.detected;
    if (m != app.mode) {
        app.mode = m;
        app.side_focus = 0;
        /* the two layouts use the scroll positions differently (rows / pixels): start them over */
        for (int l = 0; l < LIST_COUNT; l++) app.lists[l].top = 0;
        app.games.top = 0;
        if (m == UI_MODE_TV && app.view == VIEW_HOME) app.view = VIEW_FOLLOWING;
    }
}

void app_set_layout(int layout)
{
    app.layout = layout;
    if (layout == LAYOUT_AUTO) {                             /* look again (a DualShock may have been connected) */
        app.is_pstv = detect_pstv(&app.ext_pad);
        app.detected = app.is_pstv || app.ext_pad ? UI_MODE_TV : UI_MODE_HANDHELD;
    }
    apply_layout();
    app_save_settings();
}

void app_set_status(const char *msg)
{
    snprintf(app.status, sizeof app.status, "%s", msg);
    app.status_until = now_ms() + 4500;
}

/* ---------------------------------------------------------------- worker thread */

enum { JOB_NONE, JOB_CHECK, JOB_DEVICE_START, JOB_DEVICE_POLL, JOB_LIST, JOB_GAMES, JOB_PLAY, JOB_INFO, JOB_LOGOUT,
       JOB_USER, JOB_VODS, JOB_VOD, JOB_ME, JOB_CLIPS, JOB_CLIP, JOB_GAMEDESC, JOB_HERODESC };

static pthread_mutex_t job_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cv = PTHREAD_COND_INITIALIZER;
static volatile int job_req = JOB_NONE, job_busy, job_done = JOB_NONE, job_result;
static int job_list, job_q;
static char job_arg[128];
static int running_job = JOB_NONE, running_list = -1;

/* results (written by the worker, read by the UI after job_done) */
static TwStream res_streams[MAX_STREAMS];
static TwGame res_games[MAX_GAMES];
static TwStream res_info;
static TwUser res_user, res_me;
static TwGame raw_games[MAX_GAMES];              /* the top categories as Twitch sent them */
static int n_raw_games;
static TwVideo res_videos[MAX_VIDEOS];
static char res_code[32], res_uri[160];
static int res_interval = 5, res_expires = 1800;
static char *res_url;
static char res_label[24];
static char res_err[160];
static char res_gamedesc[600], res_herodesc[600];

/* channels followed on this Vita, merged into the Following lists (worker thread) */
static int cmp_stream_name(const void *a, const void *b) { return strcasecmp(((const TwStream *)a)->name, ((const TwStream *)b)->name); }

static int merge_saved(int list, TwStream *out, int n, int max)
{
    static TwStream saved[FAVCHAN_MAX], live[FAVCHAN_MAX];
    int ns = favchan_snapshot(saved, FAVCHAN_MAX);
    if (!ns || n < 0) return n;
    int nl = tw_streams_of(saved, ns, live, FAVCHAN_MAX);
    if (nl < 0) nl = 0;
    if (list == LIST_FOLLOWING) {                            /* the saved ones that are live */
        for (int i = 0; i < nl && n < max; i++) {
            int dup = 0;
            for (int k = 0; k < n && !dup; k++) if (!strcmp(out[k].user_id, live[i].user_id)) { dup = 1; out[k].saved = 1; }
            if (!dup) { out[n] = live[i]; out[n].saved = 1; n++; }
        }
        return n;
    }
    for (int i = 0; i < ns && n < max; i++) {                /* LIST_FOLLOW_OFF: the saved ones that are not live */
        int is_live = 0, dup = 0;
        for (int k = 0; k < nl; k++) if (!strcmp(live[k].user_id, saved[i].user_id)) is_live = 1;
        for (int k = 0; k < n; k++) if (!strcmp(out[k].user_id, saved[i].user_id)) { dup = 1; out[k].saved = 1; }
        if (!is_live && !dup) { out[n] = saved[i]; out[n].live = 0; n++; }
    }
    qsort(out, (size_t)n, sizeof *out, cmp_stream_name);
    return n;
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&job_mx);
        while (job_req == JOB_NONE) pthread_cond_wait(&job_cv, &job_mx);
        int job = job_req, list = job_list, q = job_q;
        char a[128];
        snprintf(a, sizeof a, "%s", job_arg);
        job_req = JOB_NONE;
        job_busy = 1;
        pthread_mutex_unlock(&job_mx);

        int r = 0;
        res_err[0] = 0;
        switch (job) {
        case JOB_CHECK: r = tw_check(); break;
        case JOB_DEVICE_START: r = tw_device_start(res_code, sizeof res_code, res_uri, sizeof res_uri, &res_interval, &res_expires); break;
        case JOB_DEVICE_POLL: r = tw_device_poll(); break;
        case JOB_LIST:
            r = list == LIST_FOLLOWING ? merge_saved(LIST_FOLLOWING, res_streams, tw_followed(res_streams, MAX_STREAMS), MAX_STREAMS)
              : list == LIST_FEATURED ? tw_top(res_streams, MAX_STREAMS)
              : list == LIST_SEARCH ? tw_search(a, res_streams, MAX_STREAMS)
              : list == LIST_FOLLOW_OFF ? merge_saved(LIST_FOLLOW_OFF, res_streams, tw_followed_offline(res_streams, MAX_STREAMS), MAX_STREAMS)
              : tw_game_streams(a, res_streams, MAX_STREAMS);
            break;
        case JOB_GAMES: r = tw_games(res_games, MAX_GAMES); break;
        case JOB_PLAY: r = tw_playback_url(a, q, res_url, HLS_URL_MAX, res_label, sizeof res_label); break;
        case JOB_INFO: r = tw_stream_info(a, &res_info); break;
        case JOB_LOGOUT: tw_logout(); break;
        case JOB_USER: r = tw_user_info(a, &res_user); break;
        case JOB_VODS: r = tw_videos(a, res_videos, MAX_VIDEOS); break;
        case JOB_VOD: r = tw_vod_url(a, q, res_url, HLS_URL_MAX, res_label, sizeof res_label); break;
        case JOB_CLIPS: r = tw_clips(a, res_videos, MAX_VIDEOS); break;
        case JOB_CLIP: r = tw_clip_url(a, q, res_url, HLS_URL_MAX, res_label, sizeof res_label); break;
        case JOB_ME: r = tw_user_info(a, &res_me); break;
        case JOB_GAMEDESC: r = tw_game_desc(a, res_gamedesc, sizeof res_gamedesc); break;
        case JOB_HERODESC: r = tw_game_desc(a, res_herodesc, sizeof res_herodesc); break;
        }
        if (r < 0) snprintf(res_err, sizeof res_err, "%s", tw_error());

        pthread_mutex_lock(&job_mx);
        job_result = r;
        job_done = job;
        job_busy = 0;
        pthread_mutex_unlock(&job_mx);
    }
    return NULL;
}

static int job_start(int job, int list, int q, const char *arg)
{
    pthread_mutex_lock(&job_mx);
    if (job_busy || job_req != JOB_NONE || job_done != JOB_NONE) { pthread_mutex_unlock(&job_mx); return 0; }
    job_list = list;
    job_q = q;
    snprintf(job_arg, sizeof job_arg, "%s", arg ? arg : "");
    job_req = job;
    running_job = job;
    running_list = job == JOB_LIST ? list : -1;
    pthread_cond_signal(&job_cv);
    pthread_mutex_unlock(&job_mx);
    return 1;
}

static int job_working(void) { return job_busy || job_req != JOB_NONE; }

static int job_take(int *result)
{
    pthread_mutex_lock(&job_mx);
    int j = job_done;
    if (j != JOB_NONE) { *result = job_result; job_done = JOB_NONE; }
    pthread_mutex_unlock(&job_mx);
    return j;
}

/* lists asked for while the worker was busy: started when it is free */
static int pending_list[LIST_COUNT], pending_games;

int app_list_loading(int list) { return (job_working() && running_job == JOB_LIST && running_list == list) || pending_list[list]; }
int app_games_loading(void) { return (job_working() && running_job == JOB_GAMES) || pending_games; }

void app_load_list(int list)
{
    if (!app.signed_in) { snprintf(app.lists[list].err, sizeof app.lists[list].err, "Log in to see channels."); app.lists[list].loaded = 1; return; }
    if (list == LIST_SEARCH && !app.search_query[0]) return;
    app.lists[list].err[0] = 0;
    const char *arg = list == LIST_SEARCH ? app.search_query : list == LIST_GAME ? app.game_id : "";
    if (!job_start(JOB_LIST, list, 0, arg)) pending_list[list] = 1;
    else pending_list[list] = 0;
}

void app_load_games(void)
{
    if (!app.signed_in) { snprintf(app.games.err, sizeof app.games.err, "Log in to browse games."); app.games.loaded = 1; return; }
    app.games.err[0] = 0;
    pending_games = !job_start(JOB_GAMES, 0, 0, NULL);
}

StreamList *app_current_list(void)
{
    switch (app.view) {
    case VIEW_FOLLOWING: return &app.lists[LIST_FOLLOWING];
    case VIEW_FEATURED: return &app.lists[LIST_FEATURED];
    case VIEW_SEARCH: return &app.lists[LIST_SEARCH];
    case VIEW_GAMES: return app.game_open ? &app.lists[LIST_GAME] : NULL;
    case VIEW_HOME: return &app.lists[LIST_FEATURED];
    }
    return NULL;
}

/* ---------------------------------------------------------------- screens and actions */

enum { ST_NO_CLIENT, ST_CHECKING, ST_LOGIN, ST_HOME, ST_OPENING, ST_PLAYING };
static int state = ST_CHECKING;
static unsigned login_next_poll, login_expires_at, info_next;
static int login_have_code;
static int play_pending_q = -1;                              /* a quality change waiting for the worker */
static int pending_user, pending_vods, pending_me, pending_clips, pending_gamedesc, pending_herodesc;   /* the channel's profile / videos / category blurb, when the worker is free */

static void start_login(void)
{
    state = ST_LOGIN;
    login_have_code = 0;
    res_code[0] = 0;
    chat_set_auth(NULL, NULL);
    job_start(JOB_DEVICE_START, 0, 0, NULL);
}

static void update_chat_auth(void);

/* back to browsing signed out (the handheld shows the Top Channels screen with Log In) */
static void signed_out(const char *why)
{
    app.signed_in = 0;
    chat_set_auth(NULL, NULL);
    memset(app.lists, 0, sizeof app.lists);
    memset(&app.games, 0, sizeof app.games);
    app.game_open = 0;
    if (state != ST_PLAYING && state != ST_OPENING) state = ST_HOME;
    app.view = app.mode == UI_MODE_TV ? VIEW_PROFILE : VIEW_HOME;
    app.me_loaded = 0;
    app.side_focus = app.mode != UI_MODE_TV;                /* (handheld: the menu, on Log In) */
    app.side_sel = 3;
    if (why) app_set_status(why);
}

void app_login(void)
{
    if (app.signed_in) return;
    start_login();
}

static void signed_in_now(void)
{
    app.signed_in = 1;
    app.me_loaded = 0;
    state = ST_HOME;
    app.view = app.mode == UI_MODE_TV ? VIEW_FOLLOWING : VIEW_HOME;
    app.side_focus = 0;
    app.home_sel = 0;
    update_chat_auth();
    app_load_list(LIST_FOLLOWING);
    app_load_list(LIST_FEATURED);
}

static void update_chat_auth(void)
{
    if (tw_can_chat()) {
        char tok[128];
        tw_token(tok, sizeof tok);
        chat_set_auth(tw_user(), tok);
    } else chat_set_auth(NULL, NULL);
}

void app_open_game(const char *id, const char *name)
{
    snprintf(app.game_id, sizeof app.game_id, "%s", id);
    snprintf(app.game_name, sizeof app.game_name, "%s", name);
    app.game_desc[0] = 0;
    app.game_desc_loaded = 0;
    pending_gamedesc = id[0] ? 1 : 0;                        /* "" = top live streams, no category blurb */
    app.game_open = 1;
    app.lists[LIST_GAME].n = app.lists[LIST_GAME].sel = app.lists[LIST_GAME].top = 0;
    app.lists[LIST_GAME].loaded = 0;
    app_load_list(LIST_GAME);
}

void app_close_game(void) { app.game_open = 0; }

/* The Home rows ask for the blurb of whichever category is highlighted; it loads
 * in the background and is cached, so scrolling the row only fetches each one once. */
void app_hero_category(const char *id)
{
    if (!id || !id[0]) { app.hero_cat_id[0] = 0; app.hero_cat_desc[0] = 0; app.hero_cat_desc_loaded = 0; pending_herodesc = 0; return; }
    if (!strcmp(id, app.hero_cat_id)) return;                /* same category still highlighted */
    snprintf(app.hero_cat_id, sizeof app.hero_cat_id, "%s", id);
    app.hero_cat_desc[0] = 0;
    app.hero_cat_desc_loaded = 0;
    pending_herodesc = 1;
}

void app_open_stream(const TwStream *s)
{
    if (!job_start(JOB_PLAY, 0, app.quality, s->login)) { app_set_status("Busy, try again in a moment."); return; }
    app.playing = *s;
    app.playing_vod = 0;
    app.playing_clip = 0;
    app.tv_panel = 0;
    app.user_loaded = 0;
    app.info_open = 0;
    app.dialog = DLG_NONE;
    app.chat_scroll = 0;
    app.qmenu_open = 0;
    app.quality_label[0] = 0;
    state = ST_OPENING;
    if (app.chat_on) chat_join(s->login);
    tpemote_set_channel(s->user_id);   /* that channel's BTTV / FFZ emotes */
}

void app_change_quality(int q)
{
    app.quality = q;
    app_save_settings();
    if (state != ST_PLAYING && state != ST_OPENING) return;
    tsp_stop();
    int ok = app.playing_clip ? job_start(JOB_CLIP, 0, q, app.clip.id)
           : app.playing_vod ? job_start(JOB_VOD, 0, q, app.vod.id) : job_start(JOB_PLAY, 0, q, app.playing.login);
    state = ST_OPENING;
    play_pending_q = ok ? -1 : q;                            /* (started when the worker is free) */
}

void app_back_from_player(void)
{
    tsp_stop();
    chat_leave();
    state = ST_HOME;
    play_pending_q = -1;
    app.info_open = 0;
    app.dialog = DLG_NONE;
    if (app.playing_clip) { app.playing_clip = 0; app.view = VIEW_CLIPS; }   /* a clip: back to the clips list */
    else if (app.playing_vod) { app.playing_vod = 0; app.view = VIEW_VODS; }   /* a past broadcast: back to the list */
}

void app_open_vods(void)
{
    if (!app.signed_in) { app.dialog = DLG_SIGNIN; app.dlg_sel = 0; return; }
    app.vods_of = app.playing;
    tsp_stop();
    chat_leave();
    state = ST_HOME;
    app.info_open = 0;
    app.dialog = DLG_NONE;
    app.view = VIEW_VODS;
    memset(&app.vods, 0, sizeof app.vods);
    pending_vods = 1;
}

void app_play_vod(const TwVideo *v)
{
    if (!job_start(JOB_VOD, 0, app.quality, v->id)) { app_set_status("Busy, try again in a moment."); return; }
    app.vod = *v;
    app.playing = app.vods_of;
    snprintf(app.playing.title, sizeof app.playing.title, "%s", v->title);
    app.playing_vod = 1;
    app.info_open = 0;
    app.dialog = DLG_NONE;
    state = ST_OPENING;
}

void app_load_me(void)
{
    if (!app.signed_in || app.me_loaded || pending_me || (job_working() && running_job == JOB_ME)) return;
    pending_me = 1;
}

/* the open category, as a TwGame (from the list, or what is known of it) */
static TwGame open_category(void)
{
    TwGame g;
    memset(&g, 0, sizeof g);
    for (int i = 0; i < n_raw_games; i++) if (!strcmp(raw_games[i].id, app.game_id)) g = raw_games[i];
    for (int i = 0; i < app.games.n && !g.id[0]; i++) if (!strcmp(app.games.items[i].id, app.game_id)) g = app.games.items[i];
    if (!g.id[0]) { snprintf(g.id, sizeof g.id, "%s", app.game_id); snprintf(g.name, sizeof g.name, "%s", app.game_name); }
    return g;
}

int app_cat_followed(void) { return favcat_has(app.game_id); }

int app_chan_saved(void)
{
    const char *id = app.playing.user_id[0] ? app.playing.user_id : app.user_loaded ? app.user.id : "";
    return id[0] && favchan_has(id);
}

void app_toggle_chan_saved(void)
{
    TwStream s = app.playing;
    if (!s.user_id[0] && app.user_loaded) snprintf(s.user_id, sizeof s.user_id, "%s", app.user.id);
    if (app.user_loaded) snprintf(s.avatar, sizeof s.avatar, "%s", app.user.avatar);
    if (!s.user_id[0]) { app_set_status("Still loading this channel, try again in a moment."); return; }
    int now = favchan_toggle(&s);
    char msg[160];
    snprintf(msg, sizeof msg, now ? "Following %s on this Vita" : "Unfollowed %s on this Vita", s.name);
    app_set_status(msg);
    app.lists[LIST_FOLLOWING].loaded = 0;                    /* (the lists show it next time) */
    app.lists[LIST_FOLLOW_OFF].loaded = 0;
    pending_list[LIST_FOLLOWING] = 1;
}

void app_toggle_cat_follow(void)
{
    if (!app.game_id[0]) return;
    TwGame g = open_category();
    int now = favcat_toggle(&g);
    app.games.n = favcat_merge(raw_games, n_raw_games, app.games.items, MAX_GAMES);
    char msg[160];
    snprintf(msg, sizeof msg, now ? "Following %s (saved on this Vita)" : "Unfollowed %s", g.name);
    app_set_status(msg);
}

void app_vods_back(void)
{
    if (app.vods_of.live) app_open_stream(&app.vods_of);    /* back to the live stream it came from */
    else app.view = VIEW_HOME;
}

void app_open_clips(void)
{
    if (!app.signed_in) { app.dialog = DLG_SIGNIN; app.dlg_sel = 0; return; }
    app.clips_of = app.playing;
    tsp_stop();
    chat_leave();
    state = ST_HOME;
    app.info_open = 0;
    app.tv_panel = 0;
    app.dialog = DLG_NONE;
    app.view = VIEW_CLIPS;
    memset(&app.clips, 0, sizeof app.clips);
    pending_clips = 1;
}

void app_play_clip(const TwVideo *v)
{
    if (!job_start(JOB_CLIP, 0, app.quality, v->id)) { app_set_status("Busy, try again in a moment."); return; }
    app.clip = *v;
    app.playing = app.clips_of;
    snprintf(app.playing.title, sizeof app.playing.title, "%s", v->title);
    app.playing_clip = 1;
    app.playing_vod = 0;
    app.info_open = 0;
    app.tv_panel = 0;
    app.dialog = DLG_NONE;
    state = ST_OPENING;
}

void app_clips_back(void)
{
    if (app.clips_of.live) app_open_stream(&app.clips_of);
    else app.view = VIEW_HOME;
}

void app_search_prompt(void)
{
    if (ime_open(IME_SEARCH, "Search channels", app.search_query, 60) < 0) app_set_status("The keyboard could not be opened.");
}

void app_chat_prompt(void)
{
    if (!app.signed_in) { app.dialog = DLG_SIGNIN; app.dlg_sel = 0; return; }
    if (!app.chat_on) { app_set_status("Chat is turned off (Settings)."); return; }
    if (!tw_can_chat()) { app_set_status("To send messages, sign out and sign in again (Settings)."); return; }
    if (!chat_signed_in()) { app_set_status("Chat is still connecting..."); return; }
    if (ime_open(IME_CHAT, "Send a message", "", 200) < 0) app_set_status("The keyboard could not be opened.");
}

void app_toggle_chat(void)
{
    app.chat_on = !app.chat_on;
    chat_set_enabled(app.chat_on);
    app_save_settings();
}

void app_logout(void)
{
    if (job_start(JOB_LOGOUT, 0, 0, NULL)) app_set_status("Signing out...");
}

static void start_playback(const char *url)
{
    tsp_set_options(1, 1080);                                /* HD decoder on: the variant is already chosen, so 1080p/720p60 only arrive when picked */
    int r = tsp_start(url);
    plog("player: start %s (%s) -> %d", app.playing.login, res_label, r);
    if (r != 0) { app_set_status("The player could not start (see log.txt)"); app_back_from_player(); return; }
    snprintf(app.quality_label, sizeof app.quality_label, "%s", res_label);
    state = ST_PLAYING;
    app.osd_until = now_ms() + 5000;
    info_next = now_ms() + 60000;
    if (!app.user_loaded) pending_user = 1;   /* description, followers, avatar are public; `following` needs auth */
}

/* ---------------------------------------------------------------- simple screens */

static void draw_center_title(int y)
{
    int w = text_w(1.25f * 1.4f, "LiveVita") + 30 * 1.4f + 8 * 1.4f;
    draw_wordmark((SCREEN_W - w) / 2, y, 1.4f);
}

/* handheld: the classic two-step screen (steps on the left, the code on the right) */
static void draw_login_hh(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(16, 10, 26, 255));
    text(50, 125, C_TEXT, 1.0f, "Sign in with a free Twitch account to chat and see who you follow!");
    static const struct { const char *a, *b, *c; } S[2] = {
        { "Go to", "twitch.tv/activate", "on a phone or computer" },
        { "On that page", "enter the code on the right", "to connect your account" },
    };
    for (int i = 0; i < 2; i++) {
        int y = 240 + i * 140;
        vita2d_draw_fill_circle(62, y - 8, 13, C_WHITE);
        char n[2] = { (char)('1' + i), 0 };
        text_center_in(49, 26, y - 1, RGBA8(16, 10, 26, 255), 0.8f, n);
        text(104, y, C_TEXT, 1.0f, S[i].a);
        text(104, y + 26, C_SEL, 1.0f, S[i].b);
        text(104, y + 54, C_TEXT, 1.0f, S[i].c);
    }
    rect(524, 230, 2, 250, RGBA8(90, 80, 110, 255));
    if (!login_have_code) {
        if (job_working()) spinner(524 + 218, 300, C_WHITE);
        else {
            text_center_in(540, 400, 290, C_WARN, 0.9f, "Could not get a code");
            text_center_in(540, 400, 320, C_DIM, 0.8f, "Check your Wi-Fi, then press X");
        }
    } else {
        text_center_in(540, 400, 280, C_DIM, 0.85f, "Your code");
        text_center_in(540, 400, 340, C_WHITE, 2.0f, res_code);
        unsigned left = login_expires_at > now_ms() ? (login_expires_at - now_ms()) / 1000 : 0;
        char l[64];
        snprintf(l, sizeof l, "expires in %u:%02u", left / 60, left % 60);
        text_center_in(540, 400, 380, C_FAINT, 0.75f, l);
        spinner(524 + 218, 430, C_DIM);
    }
    text(50, SCREEN_H - 20, C_FAINT, 0.75f, "O back     Square new code");
}

static void draw_login(void)
{
    if (app.mode != UI_MODE_TV) { draw_login_hh(); return; }
    rect(0, 0, SCREEN_W, 70, C_PURPLE);
    draw_center_title(48);
    text_center_in(0, SCREEN_W, 110, C_DIM, 0.9f, "Unofficial Twitch viewer for PS Vita and PS TV");
    if (!login_have_code) {
        if (job_working()) { text_center_in(0, SCREEN_W, 270, C_DIM, 1.0f, "Getting a sign-in code..."); spinner(SCREEN_W / 2, 320, C_SEL); }
        else {
            text_center_in(0, SCREEN_W, 260, C_WARN, 1.0f, res_err[0] ? res_err : "Could not get a sign-in code.");
            text_center_in(0, SCREEN_W, 300, C_DIM, 0.9f, "Check your Wi-Fi, then press X to try again.");
        }
        return;
    }
    text_center_in(0, SCREEN_W, 190, C_TEXT, 1.0f, "On your phone or computer, go to");
    text_center_in(0, SCREEN_W, 236, C_SEL, 1.4f, "twitch.tv/activate");
    text_center_in(0, SCREEN_W, 284, C_TEXT, 1.0f, "and enter this code:");
    int cw = text_w(2.2f, res_code);
    rect((SCREEN_W - cw) / 2 - 26, 304, cw + 52, 74, C_PANEL);
    outline((SCREEN_W - cw) / 2 - 26, 304, cw + 52, 74, 2, C_PURPLE);
    text_center_in(0, SCREEN_W, 361, C_WHITE, 2.2f, res_code);
    unsigned left = login_expires_at > now_ms() ? (login_expires_at - now_ms()) / 1000 : 0;
    char l[96];
    snprintf(l, sizeof l, "Waiting for you to approve...  (code expires in %u:%02u)", left / 60, left % 60);
    text_center_in(0, SCREEN_W, 420, C_DIM, 0.85f, l);
    spinner(SCREEN_W / 2, 460, C_SEL);
    rect(0, SCREEN_H - 30, SCREEN_W, 30, C_PANEL);
    text(14, SCREEN_H - 9, C_DIM, 0.8f, "LiveVita signs in by itself once you approve.   Square: new code   O: back");
}

static void draw_no_client(void)
{
    rect(0, 0, SCREEN_W, 70, C_PURPLE);
    draw_center_title(48);
    text_center_in(0, SCREEN_W, 120, C_WARN, 1.1f, "LiveVita needs a Twitch Client ID");
    static const char *const L[] = {
        "1. On a computer, go to dev.twitch.tv/console and register an application:",
        "      OAuth Redirect URL: http://localhost     Client Type: Public",
        "2. Copy its Client ID into this file on your Vita:",
        "      ux0:data/LiveVita/client_id.txt",
        "   (with VitaShell's FTP or USB). Then start LiveVita again.",
        "",
        "Built with Build-VPK.bat or GitHub? Give it the Client ID there and",
        "every build will include it (see the README).",
    };
    for (size_t i = 0; i < sizeof L / sizeof L[0]; i++) text(70, 170 + (int)i * 34, C_TEXT, 0.9f, L[i]);
    rect(0, SCREEN_H - 30, SCREEN_W, 30, C_PANEL);
    text(14, SCREEN_H - 9, C_DIM, 0.8f, "START: exit");
}

static void draw_opening(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    if (app.playing.thumb[0]) {
        vita2d_texture *t = img_get(app.playing.thumb);
        if (t) { draw_picture(t, 0, 0, SCREEN_W, SCREEN_H); rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(25, 23, 28, 200)); }
    }
    char line[220];
    fit(line, sizeof line, app.playing.name, 1.4f, SCREEN_W - 80);
    text_center_in(0, SCREEN_W, 230, C_TEXT, 1.4f, line);
    fit(line, sizeof line, app.playing.title, 0.9f, SCREEN_W - 80);
    text_center_in(0, SCREEN_W, 268, C_DIM, 0.9f, line);
    spinner(SCREEN_W / 2, 320, C_SEL);
    char q[80];
    snprintf(q, sizeof q, "Opening stream (%s)...   O: cancel", TWQ_NAME[app.quality]);
    text_center_in(0, SCREEN_W, 380, C_DIM, 0.85f, q);
}

/* ---------------------------------------------------------------- start-up */

static void net_init(void)
{
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);
    SceNetInitParam np = { malloc(NET_POOL_SIZE), NET_POOL_SIZE, 0 };
    sceNetInit(&np);
    sceNetCtlInit();
    sceSslInit(300 * 1024);
    sceHttpInit(1024 * 1024);
    cio_init();
}

int main(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    app_log_reset();
    plog("LiveVita %s", APP_VERSION);
    SceAppUtilInitParam aup;
    SceAppUtilBootParam abp;
    memset(&aup, 0, sizeof aup);
    memset(&abp, 0, sizeof abp);
    sceAppUtilInit(&aup, &abp);
    srand((unsigned)time(NULL) ^ now_ms());

    vita2d_init();
    vita2d_set_clear_color(C_BG);
    g_font = vita2d_load_default_pgf();
    ui_set_font(g_font);

    scePowerSetArmClockFrequency(444);                       /* full speed for TLS and the video pipeline */
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    net_init();
    input_init();

    /* hardware: PS TV (or a DualShock on a Vita) -> TV layout */
    app.is_pstv = detect_pstv(&app.ext_pad);
    app.detected = app.is_pstv || app.ext_pad ? UI_MODE_TV : UI_MODE_HANDHELD;
    load_settings();
    app.mode = (UIMode)-1;
    apply_layout();
    plog("ui: %s layout (setting %d, detected %s)", app.mode == UI_MODE_TV ? "TV" : "handheld", app.layout, app.detected == UI_MODE_TV ? "TV" : "handheld");
    app.view = app.mode == UI_MODE_TV ? VIEW_FOLLOWING : VIEW_HOME;   /* handheld: the menu + mosaic screen first */

    res_url = malloc(HLS_URL_MAX);
    load_client_id();
    favcat_load();
    favchan_load();
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&th, &at, worker, NULL);
    img_start();
    chat_set_enabled(app.chat_on);
    chat_start();
    tpemote_start();

    if (!tw_client_id()[0]) state = ST_NO_CLIENT;
    else if (tw_load_auth()) { state = ST_CHECKING; job_start(JOB_CHECK, 0, 0, NULL); }
    else { state = ST_HOME; signed_out(NULL); }              /* browse signed out; Log In when wanted */

    unsigned last_tick = 0;
    int quit = 0;
    Input in;
    while (!quit) {
        input_read(&in, ime_active());

        /* ---- finished network jobs ---- */
        int jr = 0, jd = job_take(&jr);
        if (jd == JOB_CHECK) {
            if (jr == 0) signed_in_now();
            else if (jr == -401) signed_out("Your sign-in expired: select Log In.");
            else { state = ST_HOME; app_set_status(res_err[0] ? res_err : "Could not reach Twitch"); }
        } else if (jd == JOB_DEVICE_START) {
            if (jr == 0) {
                login_have_code = 1;
                login_next_poll = now_ms() + (unsigned)res_interval * 1000;
                login_expires_at = now_ms() + (unsigned)res_expires * 1000;
            }
        } else if (jd == JOB_DEVICE_POLL) {
            if (jr == 1 && state == ST_LOGIN) {
                signed_in_now();
                app_set_status("Signed in. Welcome!");
            } else if (jr < 0) { login_have_code = 0; snprintf(res_err, sizeof res_err, "%s", tw_error()); }
            else login_next_poll = now_ms() + (unsigned)res_interval * 1000;
        } else if (jd == JOB_LIST && running_list >= 0) {
            StreamList *L = &app.lists[running_list];
            L->loaded = 1;
            if (jr >= 0) {
                memcpy(L->items, res_streams, sizeof(TwStream) * (size_t)jr);
                L->n = jr;
                if (L->sel >= jr) L->sel = jr ? jr - 1 : 0;
                L->err[0] = 0;
            } else if (jr == -401) signed_out("Your sign-in expired: select Log In.");
            else snprintf(L->err, sizeof L->err, "%s", res_err);
            running_list = -1;
        } else if (jd == JOB_GAMES) {
            app.games.loaded = 1;
            if (jr >= 0) {
                memcpy(raw_games, res_games, sizeof(TwGame) * (size_t)jr);
                n_raw_games = jr;
                app.games.n = favcat_merge(raw_games, n_raw_games, app.games.items, MAX_GAMES);   /* followed ones first */
                if (app.games.sel > jr) app.games.sel = 0;
            } else if (jr == -401) signed_out("Your sign-in expired: select Log In.");
            else snprintf(app.games.err, sizeof app.games.err, "%s", res_err);
        } else if (jd == JOB_USER) {
            if (jr == 0 && !strcasecmp(res_user.login, app.playing.login)) { app.user = res_user; app.user_loaded = 1; }
        } else if (jd == JOB_ME) {
            if (jr == 0) { app.me = res_me; app.me_loaded = 1; }
        } else if (jd == JOB_GAMEDESC) {
            if (jr == 0) snprintf(app.game_desc, sizeof app.game_desc, "%s", res_gamedesc);
            app.game_desc_loaded = 1;
        } else if (jd == JOB_HERODESC) {
            if (!strcmp(job_arg, app.hero_cat_id)) {            /* ignore a result for a category we've since scrolled past */
                if (jr == 0) snprintf(app.hero_cat_desc, sizeof app.hero_cat_desc, "%s", res_herodesc);
                app.hero_cat_desc_loaded = 1;
            }
        } else if (jd == JOB_VODS) {
            app.vods.loaded = 1;
            if (jr >= 0) { memcpy(app.vods.items, res_videos, sizeof(TwVideo) * (size_t)jr); app.vods.n = jr; }
            else snprintf(app.vods.err, sizeof app.vods.err, "%s", res_err);
        } else if (jd == JOB_CLIPS) {
            app.clips.loaded = 1;
            if (jr >= 0) { memcpy(app.clips.items, res_videos, sizeof(TwVideo) * (size_t)jr); app.clips.n = jr; }
            else snprintf(app.clips.err, sizeof app.clips.err, "%s", res_err);
        } else if (jd == JOB_PLAY || jd == JOB_VOD || jd == JOB_CLIP) {
            if (state == ST_OPENING && play_pending_q < 0) {
                if (jr == 0) start_playback(res_url);
                else { app_back_from_player(); app_set_status(res_err[0] ? res_err : "Could not open the stream"); }
            }
        } else if (jd == JOB_INFO) {
            if (jr == 1 && !strcmp(res_info.login, app.playing.login)) {
                char thumb[200];
                snprintf(thumb, sizeof thumb, "%s", app.playing.thumb);
                app.playing = res_info;
                snprintf(app.playing.thumb, sizeof app.playing.thumb, "%s", thumb);
            }
        } else if (jd == JOB_LOGOUT) {
            memset(app.lists, 0, sizeof app.lists);
            memset(&app.games, 0, sizeof app.games);
            if (state == ST_PLAYING || state == ST_OPENING) app_back_from_player();
            signed_out("Signed out.");
        }
        /* waiting work for the worker */
        if (!job_working()) {
            if (play_pending_q >= 0 && state == ST_OPENING) {
                if (app.playing_clip ? job_start(JOB_CLIP, 0, play_pending_q, app.clip.id)
                    : app.playing_vod ? job_start(JOB_VOD, 0, play_pending_q, app.vod.id) : job_start(JOB_PLAY, 0, play_pending_q, app.playing.login)) play_pending_q = -1;
            }
            else if (pending_vods) {
                const char *uid = app.vods_of.user_id[0] ? app.vods_of.user_id : app.user_loaded ? app.user.id : "";
                if (!uid[0]) { pending_vods = 0; app.vods.loaded = 1; snprintf(app.vods.err, sizeof app.vods.err, "This channel's videos cannot be found."); }
                else if (job_start(JOB_VODS, 0, 0, uid)) pending_vods = 0;
            }
            else if (pending_clips) {
                const char *uid = app.clips_of.user_id[0] ? app.clips_of.user_id : app.user_loaded ? app.user.id : "";
                if (!uid[0]) { pending_clips = 0; app.clips.loaded = 1; snprintf(app.clips.err, sizeof app.clips.err, "This channel's clips cannot be found."); }
                else if (job_start(JOB_CLIPS, 0, 0, uid)) pending_clips = 0;
            }
            else if (pending_gamedesc && app.game_open && app.game_id[0]) { if (job_start(JOB_GAMEDESC, 0, 0, app.game_id)) pending_gamedesc = 0; }
            else if (pending_herodesc && app.hero_cat_id[0]) { if (job_start(JOB_HERODESC, 0, 0, app.hero_cat_id)) pending_herodesc = 0; }
            else if (pending_me && app.signed_in) { if (job_start(JOB_ME, 0, 0, tw_user())) pending_me = 0; }
            else if (pending_user && state == ST_PLAYING) { if (job_start(JOB_USER, 0, 0, app.playing.login)) pending_user = 0; }
            else if (pending_games) app_load_games();
            else for (int l = 0; l < LIST_COUNT; l++) if (pending_list[l]) { app_load_list(l); break; }
        }
        if (state == ST_PLAYING && now_ms() >= info_next && !job_working()) {   /* fresh viewers / title */
            if (job_start(JOB_INFO, 0, 0, app.playing.login)) info_next = now_ms() + 60000;
        }

        /* ---- keyboard results ---- */
        char typed[260];
        int ip = ime_poll(typed, sizeof typed);
        if (ip == IME_SEARCH) {
            snprintf(app.search_query, sizeof app.search_query, "%s", typed);
            app.view = VIEW_SEARCH;
            app.lists[LIST_SEARCH].n = app.lists[LIST_SEARCH].sel = app.lists[LIST_SEARCH].top = 0;
            app_load_list(LIST_SEARCH);
        } else if (ip == IME_CHAT) {
            int r = chat_send(typed);
            if (r == -3) app_set_status("Sending too fast: wait a moment.");
            else if (r < 0) app_set_status("Not connected to chat yet.");
            app.chat_scroll = 0;
        }

        /* ---- input ---- */
        switch (state) {
        case ST_NO_CLIENT:
            if (in.press & SCE_CTRL_START) quit = 1;
            break;
        case ST_LOGIN:
            if (login_have_code && !job_working() && now_ms() >= login_next_poll) {
                if (now_ms() >= login_expires_at) { login_have_code = 0; snprintf(res_err, sizeof res_err, "The code expired."); }
                else job_start(JOB_DEVICE_POLL, 0, 0, NULL);
            }
            if (!login_have_code && !job_working() && (in.press & SCE_CTRL_CROSS)) start_login();
            if (login_have_code && !job_working() && (in.press & SCE_CTRL_SQUARE)) start_login();
            if (in.press & SCE_CTRL_CIRCLE) { state = ST_HOME; signed_out(NULL); }   /* (back to browsing signed out) */
            break;
        case ST_HOME:
            home_input(&in);
            break;
        case ST_OPENING:
            if (in.press & SCE_CTRL_CIRCLE) app_back_from_player();
            break;
        case ST_PLAYING:
            player_screen_input(&in);
            if (now_ms() - last_tick > 1000) {               /* keep the Vita awake while watching */
                last_tick = now_ms();
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
            }
            break;
        }

        /* ---- draw ---- */
        img_frame();
        vita2d_start_drawing();
        vita2d_clear_screen();
        switch (state) {
        case ST_NO_CLIENT: draw_no_client(); break;
        case ST_CHECKING:
            rect(0, 0, SCREEN_W, 70, C_PURPLE);
            draw_center_title(48);
            text_center_in(0, SCREEN_W, 290, C_DIM, 1.0f, "Signing in...");
            spinner(SCREEN_W / 2, 340, C_SEL);
            break;
        case ST_LOGIN: draw_login(); break;
        case ST_HOME: home_draw(); break;
        case ST_OPENING: draw_opening(); break;
        case ST_PLAYING: player_screen_draw(); break;
        }
        if (now_ms() < app.status_until) {
            int w = text_w(0.85f, app.status);
            rect((SCREEN_W - w) / 2 - 18, SCREEN_H - 82, w + 36, 36, C_PURPLE);
            text_center_in(0, SCREEN_W, SCREEN_H - 58, C_WHITE, 0.85f, app.status);
        }
        vita2d_end_drawing();
        if (ime_active()) vita2d_common_dialog_update();
        vita2d_swap_buffers();
    }

    if (state == ST_PLAYING) tsp_stop();
    img_shutdown();
    vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
