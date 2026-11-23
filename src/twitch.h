#ifndef TWITCH_H
#define TWITCH_H
/* LiveVita's connection to Twitch.
 * - Sign-in, followed channels, top streams, categories and search use Twitch's official API (Helix) with
 *   the device code flow: the Vita shows a code, the user enters it at twitch.tv/activate on a phone or PC.
 * - The address of a stream's video (an ordinary, unencrypted HLS playlist) comes from the same playback
 *   access token request Twitch's own web player makes; the official API has no call for it.
 * All calls block: run them off the UI thread. */
#include <stddef.h>

typedef struct {
    char login[40];                     /* channel name in addresses ("somechannel") */
    char name[64];                      /* display name */
    char user_id[24];
    char game[80];
    char game_id[24];
    char title[200];
    char started_at[24];                /* "2026-11-22T18:00:00Z" ("" if offline / not known) */
    char thumb[200];                    /* preview picture (320x180 JPEG), "" if none */
    char thumb_big[200];                /* the same at 640x360 (the big featured tile) */
    char avatar[220];                   /* profile picture (offline followed channels) */
    int viewers;                        /* -1 = not known (search results) */
    int live;
    int saved;                          /* followed on this Vita (favchan.c), not on the Twitch account */
} TwStream;

typedef struct {
    char id[24];
    char name[80];
    char box_art[200];                  /* 144x192 JPEG */
    char box_art_big[200];              /* 288x384 (the big first tile) */
    int followed;                       /* followed on this Vita (favcat.c) */
} TwGame;

typedef struct {
    char id[24], login[40], name[64];
    char description[300];
    char avatar[220];                   /* profile picture (300x300) */
    long followers;                     /* -1 = not known */
    long following_total;               /* channels this user follows (own profile only; -1 = not known) */
    char created_at[24];                /* account created (ISO time) */
    int following;                      /* the signed-in user follows this channel (-1 = not known) */
} TwUser;

typedef struct {
    char id[24];
    char title[200];
    char created_at[24];                /* ISO time */
    char duration[16];                  /* "3h10m6s" as Twitch writes it */
    char thumb[220];                    /* 320x180, "" while Twitch is still processing it */
    char type[16];                      /* archive (past broadcast), highlight, upload; "clip" for a clip */
    int  views;                         /* view count (clips; -1 if not known) */
} TwVideo;

/* video quality choices for tw_playback_url */
enum { TWQ_720P60, TWQ_720P30, TWQ_480P30, TWQ_360P30, TWQ_AUDIO, TWQ_COUNT };
extern const char *const TWQ_NAME[TWQ_COUNT];

/* Client ID of the app registered at dev.twitch.tv (Client Type: Public). */
void tw_set_client_id(const char *id);
const char *tw_client_id(void);

/* Reads the saved sign-in (DATA_DIR/auth.txt). 1 = a sign-in was saved. */
int  tw_load_auth(void);
/* Checks the saved sign-in with Twitch, renewing it if needed. 0 = signed in, -401 = must sign in again,
 * other negative = network trouble (see tw_error). */
int  tw_check(void);
const char *tw_user(void);              /* login of the signed-in user ("" if none) */
int  tw_can_chat(void);                 /* the sign-in includes the chat:edit permission */
/* copies the access token (for chat); "" if not signed in */
void tw_token(char *out, size_t cap);
void tw_logout(void);

/* Device sign-in: asks for a code to show. 0 = ok. */
int  tw_device_start(char *user_code, size_t ucap, char *uri, size_t uricap, int *interval_s, int *expires_s);
/* Asks once whether the user has entered the code: 1 = signed in, 0 = not yet, negative = failed / expired. */
int  tw_device_poll(void);

/* Lists. Return the number put in out (0..max), or a negative error. */
int  tw_followed(TwStream *out, int max);
/* live streams of these channels (by user id), as tw_followed */
int  tw_streams_of(const TwStream *chans, int n, TwStream *out, int max);
/* the followed channels that are offline (A-Z), with their profile pictures */
int  tw_followed_offline(TwStream *out, int max);
int  tw_top(TwStream *out, int max);
int  tw_search(const char *query, TwStream *out, int max);
int  tw_games(TwGame *out, int max);
int  tw_game_streams(const char *game_id, TwStream *out, int max);
/* Fresh info for one live channel (viewers, title, game, start time). 1 = live, 0 = offline, < 0 error. */
int  tw_stream_info(const char *login, TwStream *out);

/* A channel's profile: picture, description, followers, whether the signed-in user follows it. 0 = ok. */
int  tw_user_info(const char *login, TwUser *out);
/* A channel's highlights and past broadcasts, newest first. */
int  tw_videos(const char *user_id, TwVideo *out, int max);
/* The HLS playlist of a past broadcast / highlight (as tw_playback_url). */
int  tw_vod_url(const char *video_id, int quality, char *url, size_t cap, char *label, size_t lcap);
/* a channel's top clips (last period), newest/most-viewed first */
int  tw_clips(const char *user_id, TwVideo *out, int max);
/* the direct MP4 of a clip (progressive, not HLS). label gets e.g. "480p". 0 = ok. */
int  tw_clip_url(const char *clip_id, int quality, char *url, size_t cap, char *label, size_t lcap);
/* a category's description (the IGDB blurb Twitch shows on a category page), by game id. 0 = ok. */
int  tw_game_desc(const char *game_id, char *out, size_t cap);

/* The HLS playlist (one variant, H.264 up to 720p30, or audio only) for a live channel.
 * label gets what was chosen ("480p30", "audio only"). 0 = ok. */
int  tw_playback_url(const char *login, int quality, char *url, size_t cap, char *label, size_t lcap);
/* The variant of a master playlist for a quality (exposed for tests). 0 = found. */
int  tw_pick_variant(const char *master, int quality, char *url, size_t cap, char *label, size_t lcap);

/* Text of the last error. */
const char *tw_error(void);

/* The User-Agent sent with every request (the Vita's own browser's). */
#define TW_USER_AGENT "Mozilla/5.0 (PlayStation Vita 3.74) AppleWebKit/537.73 (KHTML, like Gecko) Silk/3.2"

#endif
