#ifndef APP_H
#define APP_H
/* LiveVita: the state shared by the screens (main.c owns it). One backend (twitch.c, chat.c, the player)
 * and two layouts: UI_MODE_HANDHELD (PS Vita) and UI_MODE_TV (PS TV, or a Vita with a DualShock). */
#include "twitch.h"
#include <vita2d.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

typedef enum { UI_MODE_HANDHELD, UI_MODE_TV } UIMode;
enum { LAYOUT_AUTO, LAYOUT_HANDHELD, LAYOUT_TV, LAYOUT_COUNT };

/* top-level views (handheld: tabs; TV: sidebar entries) */
enum { VIEW_FOLLOWING, VIEW_FEATURED, VIEW_GAMES, VIEW_SEARCH, VIEW_SETTINGS, VIEW_COUNT };
/* handheld only: the first screen (menu on the left + a mosaic of top channels) */
#define VIEW_HOME VIEW_COUNT
/* handheld only: a channel's highlights and past broadcasts (from the player's film icon) */
#define VIEW_VODS (VIEW_COUNT + 1)
/* both layouts: the signed-in user's profile (Options from there opens VIEW_SETTINGS) */
#define VIEW_PROFILE (VIEW_COUNT + 2)
/* a channel's clips */
#define VIEW_CLIPS (VIEW_COUNT + 3)

/* dialogs over the player / pages (handheld) */
enum { DLG_NONE, DLG_SIGNIN, DLG_FOLLOW, DLG_CAT_FOLLOW };
/* stream lists */
enum { LIST_FOLLOWING, LIST_FEATURED, LIST_SEARCH, LIST_GAME, LIST_FOLLOW_OFF, LIST_COUNT };   /* FOLLOW_OFF: offline followed channels */
/* handheld Following page: sort orders (START) */
enum { FSORT_LIVE, FSORT_OFFLINE, FSORT_VIEWERS, FSORT_COUNT };

#define MAX_STREAMS 100
#define MAX_GAMES   60

typedef struct {
    TwStream items[MAX_STREAMS];
    int n, sel, top, loaded;
    char err[160];
} StreamList;

typedef struct {
    TwGame items[MAX_GAMES];
    int n, sel, top, loaded;
    char err[160];
} GameList;

#define MAX_VIDEOS 40
typedef struct {
    TwVideo items[MAX_VIDEOS];
    int n, sel, top, loaded;
    char err[160];
} VideoList;

typedef struct {
    /* settings (saved) */
    int quality;                        /* TWQ_* */
    int layout;                         /* LAYOUT_* */
    int chat_on;
    /* hardware */
    UIMode mode;                        /* the layout in use */
    UIMode detected;                    /* what Auto picks */
    int is_pstv, ext_pad;               /* a PS TV / a DualShock is connected */
    int signed_in;                      /* a Twitch account is signed in (lists need one) */
    /* home */
    int view;
    int game_open;                      /* VIEW_GAMES: showing one category's streams */
    char game_name[80], game_id[24];
    char game_desc[600];                /* the open category's description (Twitch's category blurb) */
    int game_desc_loaded;
    StreamList lists[LIST_COUNT];
    GameList games;
    int settings_sel;
    int side_focus, side_sel;           /* TV: the sidebar has the focus / its highlighted entry;
                                         * handheld home: the menu has the focus / its entry */
    int home_sel;                       /* handheld home: the tile selected (mosaic, then Games, then Top Channels) */
    int home_scroll;                    /* handheld home: how far the screen has slid left (pixels) */
    char search_query[128];
    TwUser me;                          /* the signed-in user's profile */
    int me_loaded;
    int prof_focus, prof_col;           /* profile page: 0 the buttons, 1 the followed categories; the one selected */
    int options_from_profile;           /* O on Options goes back to the profile */
    int follow_sort;                    /* handheld Following: FSORT_* */
    int sort_menu_open, sort_menu_sel;
    int cat_btn_focus;                  /* handheld category page: the Follow button has the focus */
    VideoList vods;                     /* the channel's past broadcasts (VIEW_VODS) */
    TwStream vods_of;                   /* ... whose (to go back to its live stream) */
    VideoList clips;                    /* the channel's clips (VIEW_CLIPS) */
    TwStream clips_of;
    TwVideo clip;
    int playing_clip;
    int tv_panel, tv_panel_sel;         /* TV player: the channel panel (info + Follow + Clips) */
    /* player */
    TwStream playing;
    int playing_vod;                    /* a past broadcast is playing (vod), not the live stream */
    TwVideo vod;
    TwUser user;                        /* the playing channel's profile (info panel, heart) */
    int user_loaded;
    int info_open;                      /* handheld: the channel info panel */
    int dialog, dlg_sel;                /* DLG_*, the highlighted button */
    char quality_label[24];
    int theater;                        /* 1: video + chat side by side, 0: full screen */
    int chat_scroll;                    /* messages scrolled back (0 = newest at the bottom) */
    int qmenu_open, qmenu_sel;
    unsigned osd_until;
    int show_stats;
    /* status line */
    char status[200];
    unsigned status_until;
} App;

extern App app;
extern vita2d_pgf *g_font;

/* actions (main.c) */
void app_load_list(int list);
void app_load_games(void);
void app_open_game(const char *id, const char *name);   /* id "" = top live streams */
void app_close_game(void);
void app_open_stream(const TwStream *s);
void app_change_quality(int q);
void app_search_prompt(void);
void app_chat_prompt(void);
void app_set_status(const char *msg);
void app_save_settings(void);
void app_set_layout(int layout);
void app_toggle_chat(void);
void app_logout(void);
void app_login(void);                   /* the twitch.tv/activate code screen */
void app_open_vods(void);               /* from the player: the channel's past broadcasts */
void app_play_vod(const TwVideo *v);
void app_vods_back(void);               /* O on the past broadcasts page */
void app_open_clips(void);              /* from the player: the channel's clips */
void app_play_clip(const TwVideo *v);
void app_clips_back(void);
void app_load_me(void);                 /* the signed-in user's profile, if not loaded */
void app_toggle_cat_follow(void);       /* follow / unfollow the open category (saved on the Vita) */
int  app_cat_followed(void);            /* the open category is followed */
int  app_chan_saved(void);              /* the playing channel is followed on this Vita */
void app_toggle_chan_saved(void);       /* follow / unfollow it on this Vita */
void app_back_from_player(void);
int  app_list_loading(int list);
int  app_games_loading(void);
StreamList *app_current_list(void);     /* the stream list the current view shows (NULL: none) */

#endif
