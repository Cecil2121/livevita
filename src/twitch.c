#include "twitch.h"
#include "applog.h"
#include "json.h"
#include "player/curlio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AUTH_PATH  DATA_DIR "/auth.txt"
/* follows (lists), chat:read + chat:edit (reading and sending chat as the user) */
#define SCOPES_ENC "user%3Aread%3Afollows%20chat%3Aread%20chat%3Aedit"
#define RESP_MAX   (1024 * 1024)

/* The public client ID of Twitch's web player, used only for the playback access token request (the same
 * request the twitch.tv website and other third-party players make). Not a secret. */
#define WEB_PLAYER_CLIENT_ID "kimne78kx3ncx6brgo4mv6wki5h1ko"

const char *const TWQ_NAME[TWQ_COUNT] = { "1080p60 (experimental)", "720p60", "720p30", "480p30", "360p30", "Audio only" };

static char g_client_id[64];
static char g_access[128], g_refresh[128];
static char g_user[64], g_user_id[32];
static int g_can_chat;
static char g_device_code[128];
static char g_err[160];

void tw_set_client_id(const char *id) { snprintf(g_client_id, sizeof g_client_id, "%s", id ? id : ""); }
const char *tw_client_id(void) { return g_client_id; }
const char *tw_user(void) { return g_user; }
const char *tw_error(void) { return g_err; }
int tw_can_chat(void) { return g_can_chat && g_access[0]; }
void tw_token(char *out, size_t cap) { snprintf(out, cap, "%s", g_access); }

static void set_err(const char *msg)
{
    snprintf(g_err, sizeof g_err, "%s", msg);
    plog("twitch: %s", g_err);
}

/* ---- HTTP ---- */

typedef struct { char *buf; size_t len, cap; } Mem;

static int mem_data(void *ctx, const uint8_t *d, size_t n)
{
    Mem *m = ctx;
    if (m->len + n + 1 > m->cap) {
        if (m->len + n + 1 > RESP_MAX) return 1;            /* too big: stop */
        size_t nc = m->cap ? m->cap * 2 : 16384;
        while (nc < m->len + n + 1) nc *= 2;
        if (nc > RESP_MAX) nc = RESP_MAX;
        char *nb = realloc(m->buf, nc);
        if (!nb) return 1;
        m->buf = nb;
        m->cap = nc;
    }
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    m->buf[m->len] = 0;
    return 0;
}

/* One request. *resp gets the body (malloc'd, "" when empty; free it). Returns the HTTP status, or a negative
 * error (g_err says what). */
static int http(const char *method, const char *url, const char *body, const char *ctype, const char *headers, char **resp)
{
    Mem m = { NULL, 0, 0 };
    CioRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.connect_timeout_s = 10;
    rq.idle_timeout_s = 20;
    rq.data = mem_data;
    rq.ctx = &m;
    rq.ua = TW_USER_AGENT;
    rq.method = method;
    rq.body = body;
    rq.content_type = ctype;
    rq.extra_headers = headers;
    rq.body_on_error = 1;
    int status = 0;
    char err[120];
    int r = cio_get(&rq, &status, err, sizeof err);
    if (!m.buf) { m.buf = malloc(1); if (m.buf) m.buf[0] = 0; }
    *resp = m.buf;
    if (r < 0 || !status) {
        snprintf(g_err, sizeof g_err, "Network error: %s (%d)", err, r);
        plog("twitch: %s", g_err);
        return r < 0 ? r : -1;
    }
    return status;
}

static void urlenc(const char *s, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* Twitch's error text ("message" in the JSON answer) */
static void api_err(const char *what, int status, const char *resp)
{
    char msg[100] = "";
    Json j;
    if (resp && !json_parse(&j, resp)) { json_get_str(&j, 0, "message", msg, sizeof msg); json_free(&j); }
    snprintf(g_err, sizeof g_err, "%s failed (HTTP %d%s%s)", what, status, msg[0] ? ": " : "", msg);
    plog("twitch: %s", g_err);
}

/* replaces "{width}" and "{height}" in a Twitch picture address */
static void size_url(const char *tpl, int w, int h, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = tpl; *p && o + 8 < cap; ) {
        if (!strncmp(p, "{width}", 7)) { o += (size_t)snprintf(out + o, cap - o, "%d", w); p += 7; }
        else if (!strncmp(p, "{height}", 8)) { o += (size_t)snprintf(out + o, cap - o, "%d", h); p += 8; }
        else if (!strncmp(p, "%{width}", 8)) { o += (size_t)snprintf(out + o, cap - o, "%d", w); p += 8; }
        else if (!strncmp(p, "%{height}", 9)) { o += (size_t)snprintf(out + o, cap - o, "%d", h); p += 9; }
        else out[o++] = *p++;
    }
    out[o < cap ? o : cap - 1] = 0;
}

/* ---- saved sign-in ---- */

static void save_auth(void)
{
    FILE *f = fopen(AUTH_PATH, "w");
    if (!f) { plog("twitch: could not save %s", AUTH_PATH); return; }
    fprintf(f, "%s\n%s\n", g_access, g_refresh);
    fclose(f);
}

int tw_load_auth(void)
{
    g_access[0] = g_refresh[0] = 0;
    FILE *f = fopen(AUTH_PATH, "r");
    if (!f) return 0;
    if (fgets(g_access, sizeof g_access, f)) g_access[strcspn(g_access, "\r\n")] = 0;
    if (fgets(g_refresh, sizeof g_refresh, f)) g_refresh[strcspn(g_refresh, "\r\n")] = 0;
    fclose(f);
    return g_access[0] != 0;
}

void tw_logout(void)
{
    if (g_access[0]) {                                      /* tell Twitch the token is no longer wanted */
        char body[300], *resp = NULL;
        snprintf(body, sizeof body, "client_id=%s&token=%s", g_client_id, g_access);
        http("POST", "https://id.twitch.tv/oauth2/revoke", body, NULL, NULL, &resp);
        free(resp);
    }
    g_access[0] = g_refresh[0] = g_user[0] = g_user_id[0] = 0;
    g_can_chat = 0;
    remove(AUTH_PATH);
}

/* takes access_token / refresh_token from a token answer */
static int take_tokens(const char *resp)
{
    Json j;
    if (json_parse(&j, resp)) { set_err("Bad answer from Twitch"); return -1; }
    json_get_str(&j, 0, "access_token", g_access, sizeof g_access);
    char rt[128];
    json_get_str(&j, 0, "refresh_token", rt, sizeof rt);
    if (rt[0]) snprintf(g_refresh, sizeof g_refresh, "%s", rt);
    json_free(&j);
    if (!g_access[0]) { set_err("Twitch sent no access token"); return -1; }
    save_auth();
    return 0;
}

static int refresh(void)
{
    if (!g_refresh[0]) return -401;
    char body[400], enc[300], *resp = NULL;
    urlenc(g_refresh, enc, sizeof enc);
    snprintf(body, sizeof body, "client_id=%s&grant_type=refresh_token&refresh_token=%s", g_client_id, enc);
    int st = http("POST", "https://id.twitch.tv/oauth2/token", body, NULL, NULL, &resp);
    int r;
    if (st == 200) r = take_tokens(resp);
    else if (st == 400 || st == 401) { api_err("Renewing the sign-in", st, resp); r = -401; }
    else { if (st > 0) api_err("Renewing the sign-in", st, resp); r = st < 0 ? st : -1; }
    free(resp);
    plog("twitch: refresh -> %d", r);
    return r;
}

/* asks Twitch who the token belongs to; 0 ok, -401 invalid, < 0 other */
static int validate(void)
{
    char hdr[200], *resp = NULL;
    snprintf(hdr, sizeof hdr, "Authorization: OAuth %s\r\n", g_access);
    int st = http("GET", "https://id.twitch.tv/oauth2/validate", NULL, NULL, hdr, &resp);
    int r;
    if (st == 200) {
        Json j;
        r = -1;
        if (!json_parse(&j, resp)) {
            json_get_str(&j, 0, "login", g_user, sizeof g_user);
            json_get_str(&j, 0, "user_id", g_user_id, sizeof g_user_id);
            int sc = json_key(&j, 0, "scopes");
            g_can_chat = 0;
            for (int i = 0; i < json_size(&j, sc); i++) {
                char s[40];
                json_str(&j, json_index(&j, sc, i), s, sizeof s);
                if (!strcmp(s, "chat:edit")) g_can_chat = 1;
            }
            json_free(&j);
            r = g_user_id[0] ? 0 : -1;
        }
    } else if (st == 401) r = -401;
    else { if (st > 0) api_err("Checking the sign-in", st, resp); r = st < 0 ? st : -1; }
    free(resp);
    return r;
}

int tw_check(void)
{
    if (!g_access[0]) return -401;
    int r = validate();
    if (r == -401) {
        r = refresh();
        if (!r) r = validate();
    }
    if (r == -401) {
        g_access[0] = g_refresh[0] = 0;
        g_can_chat = 0;
        remove(AUTH_PATH);
        set_err("Please sign in again");
    }
    plog("twitch: check -> %d (%s, chat %d)", r, g_user, g_can_chat);
    return r;
}

/* ---- device sign-in ---- */

int tw_device_start(char *user_code, size_t ucap, char *uri, size_t uricap, int *interval_s, int *expires_s)
{
    char body[200], *resp = NULL;
    snprintf(body, sizeof body, "client_id=%s&scopes=%s", g_client_id, SCOPES_ENC);
    int st = http("POST", "https://id.twitch.tv/oauth2/device", body, NULL, NULL, &resp);
    int r = -1;
    if (st == 200) {
        Json j;
        if (!json_parse(&j, resp)) {
            json_get_str(&j, 0, "device_code", g_device_code, sizeof g_device_code);
            json_get_str(&j, 0, "user_code", user_code, ucap);
            json_get_str(&j, 0, "verification_uri", uri, uricap);
            *interval_s = (int)json_get_int(&j, 0, "interval");
            *expires_s = (int)json_get_int(&j, 0, "expires_in");
            if (*interval_s < 1) *interval_s = 5;
            json_free(&j);
            r = g_device_code[0] && user_code[0] ? 0 : -1;
            if (r) set_err("Twitch sent no sign-in code");
        }
    } else if (st > 0) api_err("Getting a sign-in code", st, resp);
    free(resp);
    return r;
}

int tw_device_poll(void)
{
    char body[400], *resp = NULL;
    snprintf(body, sizeof body,
             "client_id=%s&scopes=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
             g_client_id, SCOPES_ENC, g_device_code);
    int st = http("POST", "https://id.twitch.tv/oauth2/token", body, NULL, NULL, &resp);
    int r;
    if (st == 200) {
        r = take_tokens(resp) == 0 ? 1 : -1;
        if (r == 1 && validate() != 0) r = -1;
    } else if (st == 400 && resp && strstr(resp, "authorization_pending")) r = 0;
    else if (st == 429 || (st == 400 && resp && strstr(resp, "slow_down"))) r = 0;
    else if (st > 0) { api_err("Signing in", st, resp); r = -1; }
    else r = 0;                                             /* network hiccup: try again next time */
    free(resp);
    return r;
}

/* ---- lists ---- */

/* GET on the official API with the user's token (or, signed out, an app needs a token too: Helix always
 * wants one, so lists need a sign-in); renews the sign-in once on 401 */
static int helix(const char *url, char **resp)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        char hdr[300];
        snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\nClient-Id: %s\r\n", g_access, g_client_id);
        int st = http("GET", url, NULL, NULL, hdr, resp);
        if (st != 401 || attempt) return st;
        free(*resp);
        *resp = NULL;
        if (refresh() != 0) return 401;
    }
    return 401;
}

/* Streams from a Helix answer: search results use other field names than /streams */
static int parse_streams(const char *resp, TwStream *out, int max, int search)
{
    Json j;
    if (json_parse(&j, resp)) { set_err("Bad answer from Twitch"); return -1; }
    int data = json_key(&j, 0, "data"), n = 0;
    for (int i = 0; i < json_size(&j, data) && n < max; i++) {
        int o = json_index(&j, data, i);
        TwStream *s = &out[n];
        char tpl[200];
        memset(s, 0, sizeof *s);
        json_get_str(&j, o, search ? "broadcaster_login" : "user_login", s->login, sizeof s->login);
        json_get_str(&j, o, search ? "display_name" : "user_name", s->name, sizeof s->name);
        json_get_str(&j, o, search ? "id" : "user_id", s->user_id, sizeof s->user_id);
        json_get_str(&j, o, "game_name", s->game, sizeof s->game);
        json_get_str(&j, o, "game_id", s->game_id, sizeof s->game_id);
        json_get_str(&j, o, "title", s->title, sizeof s->title);
        json_get_str(&j, o, "started_at", s->started_at, sizeof s->started_at);
        s->viewers = search ? -1 : (int)json_get_int(&j, o, "viewer_count");
        s->live = search ? json_bool(&j, json_key(&j, o, "is_live")) : 1;
        json_get_str(&j, o, search ? "thumbnail_url" : "thumbnail_url", tpl, sizeof tpl);
        if (!search && tpl[0]) { size_url(tpl, 320, 180, s->thumb, sizeof s->thumb); size_url(tpl, 640, 360, s->thumb_big, sizeof s->thumb_big); }
        if (!s->name[0]) snprintf(s->name, sizeof s->name, "%s", s->login);
        if (s->login[0]) n++;
    }
    json_free(&j);
    return n;
}

static int list(const char *url, const char *what, TwStream *out, int max, int search)
{
    char *resp = NULL;
    int st = helix(url, &resp);
    int r;
    if (st == 200) r = parse_streams(resp, out, max, search);
    else if (st == 401) { set_err("Please sign in again"); r = -401; }
    else { if (st > 0) api_err(what, st, resp); r = st < 0 ? st : -1; }
    free(resp);
    return r;
}

int tw_followed(TwStream *out, int max)
{
    char url[200];
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/streams/followed?user_id=%s&first=100", g_user_id);
    return list(url, "Loading followed channels", out, max, 0);
}

static int cmp_name(const void *a, const void *b) { return strcasecmp(((const TwStream *)a)->name, ((const TwStream *)b)->name); }

int tw_followed_offline(TwStream *out, int max)
{
    /* who is live now (to leave them out) */
    TwStream *live = malloc(sizeof *live * 100);
    if (!live) return -1;
    int nlive = tw_followed(live, 100);
    if (nlive < 0) { free(live); return nlive; }
    /* every followed channel, 100 a page (up to 300) */
    int n = 0;
    char cursor[200] = "";
    for (int page = 0; page < 3 && n < max; page++) {
        char url[460], *resp = NULL;
        snprintf(url, sizeof url, "https://api.twitch.tv/helix/channels/followed?user_id=%s&first=100%s%s", g_user_id,
                 cursor[0] ? "&after=" : "", cursor);
        int st = helix(url, &resp);
        if (st != 200) { if (st > 0) api_err("Loading followed channels", st, resp); free(resp); if (!n) { free(live); return st == 401 ? -401 : -1; } break; }
        Json j;
        cursor[0] = 0;
        if (!json_parse(&j, resp)) {
            int data = json_key(&j, 0, "data");
            for (int i = 0; i < json_size(&j, data) && n < max; i++) {
                int o = json_index(&j, data, i);
                TwStream *s = &out[n];
                memset(s, 0, sizeof *s);
                json_get_str(&j, o, "broadcaster_login", s->login, sizeof s->login);
                json_get_str(&j, o, "broadcaster_name", s->name, sizeof s->name);
                json_get_str(&j, o, "broadcaster_id", s->user_id, sizeof s->user_id);
                s->viewers = -1;
                int is_live = 0;
                for (int k = 0; k < nlive; k++) if (!strcmp(live[k].user_id, s->user_id)) is_live = 1;
                if (!s->name[0]) snprintf(s->name, sizeof s->name, "%s", s->login);
                if (s->login[0] && !is_live) n++;
            }
            json_get_str(&j, json_key(&j, 0, "pagination"), "cursor", cursor, sizeof cursor);
            json_free(&j);
        }
        free(resp);
        if (!cursor[0]) break;
    }
    free(live);
    /* their profile pictures, 100 at a time */
    for (int base = 0; base < n; base += 100) {
        char *url = malloc(4096), *resp = NULL;
        if (!url) break;
        int k = snprintf(url, 4096, "https://api.twitch.tv/helix/users?");
        for (int i = base; i < n && i < base + 100 && k < 4000; i++) k += snprintf(url + k, 4096 - (size_t)k, "%sid=%s", i > base ? "&" : "", out[i].user_id);
        if (helix(url, &resp) == 200) {
            Json j;
            if (!json_parse(&j, resp)) {
                int data = json_key(&j, 0, "data");
                for (int i = 0; i < json_size(&j, data); i++) {
                    int o = json_index(&j, data, i);
                    char id[24], av[220];
                    json_get_str(&j, o, "id", id, sizeof id);
                    json_get_str(&j, o, "profile_image_url", av, sizeof av);
                    for (int m = base; m < n && m < base + 100; m++) if (!strcmp(out[m].user_id, id)) snprintf(out[m].avatar, sizeof out[m].avatar, "%s", av);
                }
                json_free(&j);
            }
        }
        free(resp);
        free(url);
    }
    qsort(out, (size_t)n, sizeof *out, cmp_name);
    return n;
}

int tw_streams_of(const TwStream *chans, int n, TwStream *out, int max)
{
    if (n <= 0) return 0;
    char *url = malloc(4096);
    if (!url) return -1;
    int k = snprintf(url, 4096, "https://api.twitch.tv/helix/streams?first=100");
    for (int i = 0; i < n && i < 100 && k < 4000; i++) k += snprintf(url + k, 4096 - (size_t)k, "&user_id=%s", chans[i].user_id);
    int r = list(url, "Loading saved channels", out, max, 0);
    free(url);
    return r;
}

int tw_top(TwStream *out, int max)
{
    return list("https://api.twitch.tv/helix/streams?first=60", "Loading featured streams", out, max, 0);
}

int tw_game_streams(const char *game_id, TwStream *out, int max)
{
    char url[200];
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/streams?game_id=%s&first=60", game_id);
    return list(url, "Loading the category", out, max, 0);
}

int tw_search(const char *query, TwStream *out, int max)
{
    char enc[300], url[400];
    urlenc(query, enc, sizeof enc);
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/search/channels?query=%s&first=40", enc);
    int n = list(url, "Searching", out, max, 1);
    if (n > 1) {                                            /* live channels first, keeping Twitch's order */
        TwStream *tmp = malloc(sizeof *tmp * (size_t)n);
        if (tmp) {
            int k = 0;
            for (int i = 0; i < n; i++) if (out[i].live) tmp[k++] = out[i];
            for (int i = 0; i < n; i++) if (!out[i].live) tmp[k++] = out[i];
            memcpy(out, tmp, sizeof *tmp * (size_t)n);
            free(tmp);
        }
    }
    return n;
}

int tw_games(TwGame *out, int max)
{
    char *resp = NULL;
    int st = helix("https://api.twitch.tv/helix/games/top?first=60", &resp);
    int r = -1;
    if (st == 200) {
        Json j;
        if (!json_parse(&j, resp)) {
            int data = json_key(&j, 0, "data");
            r = 0;
            for (int i = 0; i < json_size(&j, data) && r < max; i++) {
                int o = json_index(&j, data, i);
                TwGame *g = &out[r];
                char tpl[200];
                memset(g, 0, sizeof *g);
                json_get_str(&j, o, "id", g->id, sizeof g->id);
                json_get_str(&j, o, "name", g->name, sizeof g->name);
                json_get_str(&j, o, "box_art_url", tpl, sizeof tpl);
                if (tpl[0]) { size_url(tpl, 144, 192, g->box_art, sizeof g->box_art); size_url(tpl, 288, 384, g->box_art_big, sizeof g->box_art_big); }
                if (g->id[0]) r++;
            }
            json_free(&j);
        } else set_err("Bad answer from Twitch");
    } else if (st == 401) { set_err("Please sign in again"); r = -401; }
    else { if (st > 0) api_err("Loading categories", st, resp); r = st < 0 ? st : -1; }
    free(resp);
    return r;
}

int tw_stream_info(const char *login, TwStream *out)
{
    char url[200];
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/streams?user_login=%s", login);
    TwStream s;
    int n = list(url, "Loading stream info", &s, 1, 0);
    if (n == 1) *out = s;
    return n;
}

/* ---- channel profile and videos ---- */

int tw_user_info(const char *login, TwUser *out)
{
    memset(out, 0, sizeof *out);
    out->followers = -1;
    out->following = -1;
    out->following_total = -1;
    char url[260], *resp = NULL;
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/users?login=%s", login);
    int st = helix(url, &resp), r = -1;
    if (st == 200) {
        Json j;
        if (!json_parse(&j, resp)) {
            int o = json_index(&j, json_key(&j, 0, "data"), 0);
            if (o >= 0) {
                json_get_str(&j, o, "id", out->id, sizeof out->id);
                json_get_str(&j, o, "login", out->login, sizeof out->login);
                json_get_str(&j, o, "display_name", out->name, sizeof out->name);
                json_get_str(&j, o, "description", out->description, sizeof out->description);
                json_get_str(&j, o, "profile_image_url", out->avatar, sizeof out->avatar);
                json_get_str(&j, o, "created_at", out->created_at, sizeof out->created_at);
                r = 0;
            } else set_err("Channel not found");
            json_free(&j);
        }
    } else if (st > 0) api_err("Loading the channel", st, resp);
    free(resp);
    if (r) return r;
    /* followers: the total comes with any user's token */
    resp = NULL;
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/channels/followers?broadcaster_id=%s&first=1", out->id);
    if (helix(url, &resp) == 200) {
        Json j;
        if (!json_parse(&j, resp)) { out->followers = json_get_int(&j, 0, "total"); json_free(&j); }
    }
    free(resp);
    /* one's own profile: how many channels one follows */
    if (g_user_id[0] && !strcmp(out->id, g_user_id)) {
        resp = NULL;
        snprintf(url, sizeof url, "https://api.twitch.tv/helix/channels/followed?user_id=%s&first=1", g_user_id);
        if (helix(url, &resp) == 200) {
            Json j;
            if (!json_parse(&j, resp)) { out->following_total = json_get_int(&j, 0, "total"); json_free(&j); }
        }
        free(resp);
        return 0;
    }
    /* does the signed-in user follow it? */
    if (g_user_id[0]) {
        resp = NULL;
        snprintf(url, sizeof url, "https://api.twitch.tv/helix/channels/followed?user_id=%s&broadcaster_id=%s", g_user_id, out->id);
        if (helix(url, &resp) == 200) {
            Json j;
            if (!json_parse(&j, resp)) { out->following = json_size(&j, json_key(&j, 0, "data")) > 0; json_free(&j); }
        }
        free(resp);
    }
    return 0;
}

int tw_videos(const char *user_id, TwVideo *out, int max)
{
    char url[200], *resp = NULL;
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/videos?user_id=%s&first=%d", user_id, max > 100 ? 100 : max);
    int st = helix(url, &resp), r = -1;
    if (st == 200) {
        Json j;
        if (!json_parse(&j, resp)) {
            int data = json_key(&j, 0, "data");
            r = 0;
            for (int i = 0; i < json_size(&j, data) && r < max; i++) {
                int o = json_index(&j, data, i);
                TwVideo *v = &out[r];
                char tpl[220];
                memset(v, 0, sizeof *v);
                v->views = -1;
                json_get_str(&j, o, "id", v->id, sizeof v->id);
                json_get_str(&j, o, "title", v->title, sizeof v->title);
                json_get_str(&j, o, "created_at", v->created_at, sizeof v->created_at);
                json_get_str(&j, o, "duration", v->duration, sizeof v->duration);
                json_get_str(&j, o, "type", v->type, sizeof v->type);
                json_get_str(&j, o, "thumbnail_url", tpl, sizeof tpl);
                if (tpl[0] && !strstr(tpl, "_404/")) size_url(tpl, 320, 180, v->thumb, sizeof v->thumb);
                if (v->id[0]) r++;
            }
            json_free(&j);
        } else set_err("Bad answer from Twitch");
    } else if (st == 401) { set_err("Please sign in again"); r = -401; }
    else if (st > 0) api_err("Loading past broadcasts", st, resp);
    free(resp);
    return r;
}

/* ---- playback ---- */

/* attribute of an #EXT-X-STREAM-INF line ("" if absent); quotes removed */
static void inf_attr(const char *line, const char *name, char *out, size_t cap)
{
    out[0] = 0;
    size_t n = strlen(name);
    for (const char *p = strchr(line, ':'); p && *p; ) {
        p++;
        if (!strncasecmp(p, name, n) && p[n] == '=') {
            const char *v = p + n + 1, *e;
            if (*v == '"') { v++; e = strchr(v, '"'); }
            else e = v + strcspn(v, ",\r\n");
            if (!e) e = v + strlen(v);
            size_t l = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return;
        }
        /* to the next attribute (commas inside quotes are skipped) */
        int q = 0;
        while (*p && *p != '\n' && (q || *p != ',')) { if (*p == '"') q = !q; p++; }
        if (*p != ',') break;
    }
}

typedef struct { int h, fps, audio, avc; long bw; const char *uri; size_t ulen; } Var;

int tw_pick_variant(const char *master, int quality, char *url, size_t cap, char *label, size_t lcap)
{
    Var v[24];
    int n = 0;
    const char *p = master;
    while (*p && n < 24) {
        const char *e = p + strcspn(p, "\r\n");
        if (!strncmp(p, "#EXT-X-STREAM-INF:", 18)) {
            char line[1024], a[128];
            size_t l = (size_t)(e - p) < sizeof line - 1 ? (size_t)(e - p) : sizeof line - 1;
            memcpy(line, p, l);
            line[l] = 0;
            Var *x = &v[n];
            memset(x, 0, sizeof *x);
            inf_attr(line, "RESOLUTION", a, sizeof a);
            const char *xx = strchr(a, 'x');
            x->h = xx ? atoi(xx + 1) : 0;
            inf_attr(line, "FRAME-RATE", a, sizeof a);
            x->fps = a[0] ? (int)(atof(a) + 0.5) : 0;
            inf_attr(line, "BANDWIDTH", a, sizeof a);
            x->bw = atol(a);
            inf_attr(line, "CODECS", a, sizeof a);
            x->avc = !a[0] || strstr(a, "avc1") != NULL;
            char vid[64];
            inf_attr(line, "VIDEO", vid, sizeof vid);
            x->audio = !strcmp(vid, "audio_only") || (a[0] && !strstr(a, "avc1") && !strstr(a, "hvc1") && !strstr(a, "hev1") && !strstr(a, "av01"));
            /* the URI is the next line that is not a tag */
            const char *u = e;
            while (*u == '\r' || *u == '\n') u++;
            while (*u == '#') { u += strcspn(u, "\r\n"); while (*u == '\r' || *u == '\n') u++; }
            if (*u) { x->uri = u; x->ulen = strcspn(u, "\r\n"); n++; }
            p = e;
        } else p = e;
        while (*p == '\r' || *p == '\n') p++;
    }
    int best = -1;
    if (quality == TWQ_AUDIO) {
        for (int i = 0; i < n; i++) if (v[i].audio && (best < 0 || v[i].bw > v[best].bw)) best = i;
    } else {
        int max_h = quality == TWQ_1080P60 ? 1080 : (quality == TWQ_720P60 || quality == TWQ_720P30) ? 720 : quality == TWQ_480P30 ? 480 : 360;
        int allow60 = (quality == TWQ_1080P60 || quality == TWQ_720P60);   /* 1080p and 720p60 let a 60 fps variant through */
        int hard = quality == TWQ_1080P60 ? 1080 : 720;     /* never pick above this, even as a last resort */
        for (int pass = 0; pass < 3 && best < 0; pass++) {
            for (int i = 0; i < n; i++) {
                const Var *x = &v[i];
                if (x->audio || !x->avc || x->h <= 0 || x->h > hard) continue;
                if (pass < 2 && x->h > max_h) continue;
                if (!allow60 && x->fps > 31) continue;      /* the Vita decodes 30 fps most smoothly */
                if (best < 0) { best = i; continue; }
                const Var *b = &v[best];
                int better;
                if (pass < 2) {                             /* biggest, then (for 720p60) highest fps, then bitrate */
                    if (x->h != b->h) better = x->h > b->h;
                    else if (allow60 && x->fps != b->fps) better = x->fps > b->fps;
                    else better = x->bw > b->bw;
                } else better = (x->h < b->h || (x->h == b->h && x->bw < b->bw));   /* smallest as a last resort */
                if (better) best = i;
            }
        }
    }
    if (best < 0 || v[best].ulen >= cap) return -1;
    memcpy(url, v[best].uri, v[best].ulen);
    url[v[best].ulen] = 0;
    if (v[best].audio) snprintf(label, lcap, "audio only");
    else snprintf(label, lcap, "%dp%d", v[best].h, v[best].fps ? v[best].fps : 30);
    return 0;
}

/* the playback access token (as Twitch's web player asks for it), then the master playlist, then one variant */
static int playback(int vod, const char *what, int quality, char *url, size_t cap, char *label, size_t lcap)
{
    char body[1200], *resp = NULL;
    if (!vod)
        snprintf(body, sizeof body,
                 "{\"operationName\":\"PlaybackAccessToken\",\"query\":\"query PlaybackAccessToken($login: String!) "
                 "{ streamPlaybackAccessToken(channelName: $login, params: {platform: \\\"web\\\", playerBackend: "
                 "\\\"mediaplayer\\\", playerType: \\\"site\\\"}) { value signature } }\","
                 "\"variables\":{\"login\":\"%s\"}}", what);
    else
        snprintf(body, sizeof body,
                 "{\"operationName\":\"PlaybackAccessToken\",\"query\":\"query PlaybackAccessToken($id: ID!) "
                 "{ videoPlaybackAccessToken(id: $id, params: {platform: \\\"web\\\", playerBackend: "
                 "\\\"mediaplayer\\\", playerType: \\\"site\\\"}) { value signature } }\","
                 "\"variables\":{\"id\":\"%s\"}}", what);
    char hdr[120];
    snprintf(hdr, sizeof hdr, "Client-ID: %s\r\n", WEB_PLAYER_CLIENT_ID);
    int st = http("POST", "https://gql.twitch.tv/gql", body, "application/json", hdr, &resp);
    if (st != 200) {
        if (st > 0) api_err("Getting the stream", st, resp);
        free(resp);
        return -1;
    }
    char *value = malloc(4096), *enc = malloc(12288), *usher = malloc(16384), sig[100] = "";
    int r = -1;
    Json j;
    if (value && enc && usher && !json_parse(&j, resp)) {
        int tok = json_key(&j, json_key(&j, 0, "data"), vod ? "videoPlaybackAccessToken" : "streamPlaybackAccessToken");
        json_get_str(&j, tok, "value", value, 4096);
        json_get_str(&j, tok, "signature", sig, sizeof sig);
        json_free(&j);
        if (value[0] && sig[0]) {
            urlenc(value, enc, 12288);
            /* the plain HLS request: no supported_codecs / fast_bread, so Twitch sends MPEG-TS segments where it can
             * (the player reads fragmented MP4 too, in case it does not) */
            snprintf(usher, 16384,
                     "https://usher.ttvnw.net/%s/%s.m3u8?allow_source=true&allow_audio_only=true"
                     "&player_backend=mediaplayer&playlist_include_framerate=true&reassignments_supported=true"
                     "&p=%u&sig=%s&token=%s",
                     vod ? "vod" : "api/channel/hls", what, (unsigned)(rand() % 9000000 + 1000000), sig, enc);
            char *master = NULL;
            int ms = http("GET", usher, NULL, NULL, NULL, &master);
            if (ms == 200 && master && strstr(master, "#EXTM3U")) {
                if (tw_pick_variant(master, quality, url, cap, label, lcap) == 0) r = 0;
                else set_err(quality == TWQ_AUDIO ? "This video has no audio-only version" : "This channel only streams above 720p, which the Vita cannot play");
            } else if (ms == 404 || ms == 403) set_err(vod ? "This video is not available (subscribers only?)" : "This channel is offline or not available");
            else if (ms > 0) api_err("Getting the stream playlist", ms, NULL);
            free(master);
        } else set_err(vod ? "This video is not available" : "This channel is offline or not available");
    } else set_err("Bad answer from Twitch");
    free(value);
    free(enc);
    free(usher);
    free(resp);
    plog("twitch: playback %s %s q=%d -> %d (%s)", vod ? "video" : "live", what, quality, r, r ? g_err : label);
    return r;
}

int tw_playback_url(const char *login, int quality, char *url, size_t cap, char *label, size_t lcap)
{
    return playback(0, login, quality, url, cap, label, lcap);
}

int tw_vod_url(const char *video_id, int quality, char *url, size_t cap, char *label, size_t lcap)
{
    return playback(1, video_id, quality, url, cap, label, lcap);
}

int tw_clips(const char *user_id, TwVideo *out, int max)
{
    char url[200], *resp = NULL;
    snprintf(url, sizeof url, "https://api.twitch.tv/helix/clips?broadcaster_id=%s&first=%d", user_id, max > 50 ? 50 : max);
    int st = helix(url, &resp), r = -1;
    if (st == 200) {
        Json j;
        if (!json_parse(&j, resp)) {
            int data = json_key(&j, 0, "data");
            r = 0;
            for (int i = 0; i < json_size(&j, data) && r < max; i++) {
                int o = json_index(&j, data, i);
                TwVideo *v = &out[r];
                char tpl[220];
                memset(v, 0, sizeof *v);
                json_get_str(&j, o, "id", v->id, sizeof v->id);
                json_get_str(&j, o, "title", v->title, sizeof v->title);
                json_get_str(&j, o, "created_at", v->created_at, sizeof v->created_at);
                json_get_str(&j, o, "thumbnail_url", tpl, sizeof tpl);
                if (tpl[0]) snprintf(v->thumb, sizeof v->thumb, "%s", tpl);   /* clip thumbs are fixed-size URLs */
                v->views = (int)json_get_int(&j, o, "view_count");
                long dur = (long)json_get_int(&j, o, "duration");            /* seconds (rounded) */
                snprintf(v->duration, sizeof v->duration, "%ldm%lds", dur / 60, dur % 60);
                snprintf(v->type, sizeof v->type, "clip");
                if (v->id[0]) r++;
            }
            json_free(&j);
        } else set_err("Bad answer from Twitch");
    } else if (st == 401) { set_err("Please sign in again"); r = -401; }
    else if (st > 0) api_err("Loading clips", st, resp);
    free(resp);
    return r;
}

int tw_game_desc(const char *game_id, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!game_id || !game_id[0]) return -1;
    char body[200], *resp = NULL;
    snprintf(body, sizeof body, "{\"query\":\"query { game(id: \\\"%s\\\") { description } }\"}", game_id);
    char hdr[120];
    snprintf(hdr, sizeof hdr, "Client-ID: %s\r\n", WEB_PLAYER_CLIENT_ID);
    int st = http("POST", "https://gql.twitch.tv/gql", body, "application/json", hdr, &resp);
    if (st != 200) { if (st > 0) api_err("Loading the category", st, resp); free(resp); return -1; }
    int r = -1;
    Json j;
    if (!json_parse(&j, resp)) {
        int game = json_key(&j, json_key(&j, 0, "data"), "game");
        if (game > 0) {
            json_get_str(&j, game, "description", out, cap);
            for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';   /* one flowing blurb */
            if (out[0]) r = 0;
        }
        json_free(&j);
    } else set_err("Bad answer from Twitch");
    free(resp);
    plog("twitch: game_desc %s -> %d (%d chars)", game_id, r, (int)strlen(out));
    return r;
}

int tw_clip_url(const char *clip_id, int quality, char *url, size_t cap, char *label, size_t lcap)
{
    char body[700], *resp = NULL;
    snprintf(body, sizeof body,
             "{\"query\":\"query { clip(slug: \\\"%s\\\") { playbackAccessToken(params: {platform: \\\"web\\\", "
             "playerBackend: \\\"mediaplayer\\\", playerType: \\\"clips-watch\\\"}) { signature value } "
             "videoQualities { frameRate quality sourceURL } } }\"}", clip_id);
    char hdr[120];
    snprintf(hdr, sizeof hdr, "Client-ID: %s\r\n", WEB_PLAYER_CLIENT_ID);
    int st = http("POST", "https://gql.twitch.tv/gql", body, "application/json", hdr, &resp);
    if (st != 200) { if (st > 0) api_err("Getting the clip", st, resp); free(resp); return -1; }
    int want = quality == TWQ_360P30 ? 360 : quality == TWQ_480P30 ? 480 : 720;   /* clips cap at 720 for the Vita */
    char *src = malloc(2048), *enc = malloc(2048), sig[120] = "", value[2048] = "";
    int r = -1;
    Json j;
    if (src && enc && !json_parse(&j, resp)) {
        int clip = json_key(&j, json_key(&j, 0, "data"), "clip");
        int tok = json_key(&j, clip, "playbackAccessToken");
        json_get_str(&j, tok, "signature", sig, sizeof sig);
        json_get_str(&j, tok, "value", value, sizeof value);
        int vq = json_key(&j, clip, "videoQualities"), best = -1, best_h = -1;
        char besturl[2048] = "";
        for (int i = 0; i < json_size(&j, vq); i++) {
            int o = json_index(&j, vq, i);
            char q[12], su[2048];
            json_get_str(&j, o, "quality", q, sizeof q);
            json_get_str(&j, o, "sourceURL", su, sizeof su);
            int h = atoi(q);
            if (h <= 0 || h > 720 || !su[0]) continue;
            /* the biggest that is <= the wanted height, else the smallest available */
            if (best < 0 || (h <= want && (h > best_h || best_h > want)) || (best_h > want && h < best_h)) {
                best = i; best_h = h; snprintf(besturl, sizeof besturl, "%s", su);
            }
        }
        if (best >= 0 && sig[0] && value[0]) {
            urlenc(value, enc, 2048);
            int n = snprintf(url, cap, "%s%ssig=%s&token=%s", besturl, strchr(besturl, '?') ? "&" : "?", sig, enc);
            if (n > 0 && (size_t)n < cap) { snprintf(label, lcap, "%dp", best_h); r = 0; }
            else set_err("The clip address is too long");
        } else set_err("This clip is not available");
        json_free(&j);
    } else set_err("Bad answer from Twitch");
    free(src);
    free(enc);
    free(resp);
    plog("twitch: clip %s q=%d -> %d (%s)", clip_id, quality, r, r ? g_err : label);
    return r;
}
