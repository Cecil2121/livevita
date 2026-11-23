#include "tpemote.h"
#include "applog.h"
#include "json.h"
#include "twitch.h"
#include "player/curlio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GLOB_MAX 220
#define CHAN_MAX 320
#define RESP_MAX (1024 * 1024)

typedef struct { char name[40]; char url[168]; } TpEmote;

static TpEmote g_glob[GLOB_MAX];
static int g_nglob;
static TpEmote g_chan[CHAN_MAX];
static int g_nchan;
static char g_loaded_id[24];                    /* channel whose emotes are in g_chan */
static char g_pending_id[24];                   /* channel the UI wants */
static int g_globals_done;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

/* ---- download ---- */

typedef struct { char *buf; size_t len, cap; } Mem;

static int mem_data(void *ctx, const uint8_t *d, size_t n)
{
    Mem *m = ctx;
    if (m->len + n + 1 > m->cap) {
        if (m->len + n + 1 > RESP_MAX) return 1;
        size_t nc = m->cap ? m->cap * 2 : 16384;
        while (nc < m->len + n + 1) nc *= 2;
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

/* GETs url into a malloc'd string (caller frees), or NULL */
static char *fetch(const char *url)
{
    Mem m = { NULL, 0, 0 };
    CioRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.connect_timeout_s = 8;
    rq.idle_timeout_s = 12;
    rq.data = mem_data;
    rq.ctx = &m;
    rq.ua = TW_USER_AGENT;
    int status = 0;
    char err[80];
    int r = cio_get(&rq, &status, err, sizeof err);
    if (r < 0 || status != 200 || !m.buf) { free(m.buf); plog("tpemote: %s -> r%d http%d", url, r, status); return NULL; }
    return m.buf;
}

/* ---- parsing ---- */

static void add(TpEmote *arr, int *n, int max, const char *name, const char *url)
{
    if (*n >= max || !name[0] || !url[0]) return;
    for (int i = 0; i < *n; i++) if (!strcmp(arr[i].name, name)) return;   /* channel emote already added: keep it */
    snprintf(arr[*n].name, sizeof arr[*n].name, "%s", name);
    snprintf(arr[*n].url, sizeof arr[*n].url, "%s", url);
    (*n)++;
}

/* BetterTTV: array of { id, code, imageType } (png only). returns emotes added. */
static int parse_bttv_array(const Json *j, int arr, TpEmote *out, int *n, int max)
{
    int added = 0;
    for (int i = 0; i < json_size(j, arr); i++) {
        int o = json_index(j, arr, i);
        char type[12], code[40], id[40];
        json_get_str(j, o, "imageType", type, sizeof type);
        if (strcmp(type, "png")) continue;                      /* skip gif: the Vita can't decode it */
        json_get_str(j, o, "code", code, sizeof code);
        json_get_str(j, o, "id", id, sizeof id);
        char url[168];
        snprintf(url, sizeof url, "https://cdn.betterttv.net/emote/%s/1x", id);
        int before = *n;
        add(out, n, max, code, url);
        added += *n - before;
    }
    return added;
}

static void load_bttv(const char *id, TpEmote *out, int *n, int max)
{
    char url[160];
    char *resp;
    Json j;
    if (!id) {                                                  /* global */
        resp = fetch("https://api.betterttv.net/3/cached/emotes/global");
        if (resp) {
            if (!json_parse(&j, resp)) { parse_bttv_array(&j, 0, out, n, max); json_free(&j); }
            free(resp);
        }
        return;
    }
    snprintf(url, sizeof url, "https://api.betterttv.net/3/cached/users/twitch/%s", id);
    resp = fetch(url);
    if (!resp) return;
    if (!json_parse(&j, resp)) {
        parse_bttv_array(&j, json_key(&j, 0, "channelEmotes"), out, n, max);
        parse_bttv_array(&j, json_key(&j, 0, "sharedEmotes"), out, n, max);
        json_free(&j);
    }
    free(resp);
}

/* FrankerFaceZ: sets.<id>.emoticons[] with { name, urls:{ "1": "//cdn..." } } (all png) */
static void parse_ffz_set(const Json *j, int set, TpEmote *out, int *n, int max)
{
    int emo = json_key(j, set, "emoticons");
    for (int i = 0; i < json_size(j, emo); i++) {
        int o = json_index(j, emo, i);
        char name[40], u[168];
        json_get_str(j, o, "name", name, sizeof name);
        int urls = json_key(j, o, "urls");
        json_get_str(j, urls, "1", u, sizeof u);
        if (!u[0]) continue;
        char url[172];
        if (!strncmp(u, "//", 2)) snprintf(url, sizeof url, "https:%s", u);     /* protocol-relative */
        else snprintf(url, sizeof url, "%s", u);
        add(out, n, max, name, url);
    }
}

static void load_ffz(const char *id, TpEmote *out, int *n, int max)
{
    char url[160];
    char *resp;
    Json j;
    if (!id) {
        resp = fetch("https://api.frankerfacez.com/v1/set/global");
        if (resp) {
            if (!json_parse(&j, resp)) {
                int ds = json_key(&j, 0, "default_sets");         /* the set ids that are global */
                int sets = json_key(&j, 0, "sets");
                for (int i = 0; i < json_size(&j, ds); i++) {
                    char key[16];
                    snprintf(key, sizeof key, "%ld", json_int(&j, json_index(&j, ds, i)));
                    parse_ffz_set(&j, json_key(&j, sets, key), out, n, max);
                }
                json_free(&j);
            }
            free(resp);
        }
        return;
    }
    snprintf(url, sizeof url, "https://api.frankerfacez.com/v1/room/id/%s", id);
    resp = fetch(url);
    if (!resp) return;
    if (!json_parse(&j, resp)) {
        char key[16];
        snprintf(key, sizeof key, "%ld", json_get_int(&j, json_key(&j, 0, "room"), "set"));
        parse_ffz_set(&j, json_key(&j, json_key(&j, 0, "sets"), key), out, n, max);
        json_free(&j);
    }
    free(resp);
}

/* ---- thread ---- */

static void *worker(void *arg)
{
    (void)arg;
    /* the global emotes once, into a local buffer, then publish */
    TpEmote glob[GLOB_MAX];
    int ng = 0;
    load_bttv(NULL, glob, &ng, GLOB_MAX);
    load_ffz(NULL, glob, &ng, GLOB_MAX);
    pthread_mutex_lock(&g_mx);
    memcpy(g_glob, glob, sizeof glob[0] * (size_t)ng);
    g_nglob = ng;
    g_globals_done = 1;
    pthread_mutex_unlock(&g_mx);
    plog("tpemote: %d global emotes", ng);

    for (;;) {
        char id[24];
        pthread_mutex_lock(&g_mx);
        while (!strcmp(g_pending_id, g_loaded_id)) pthread_cond_wait(&g_cv, &g_mx);
        snprintf(id, sizeof id, "%s", g_pending_id);
        pthread_mutex_unlock(&g_mx);

        TpEmote chan[CHAN_MAX];
        int nc = 0;
        if (id[0]) {
            load_bttv(id, chan, &nc, CHAN_MAX);
            load_ffz(id, chan, &nc, CHAN_MAX);
        }
        pthread_mutex_lock(&g_mx);
        if (!strcmp(id, g_pending_id)) {                        /* still wanted */
            memcpy(g_chan, chan, sizeof chan[0] * (size_t)nc);
            g_nchan = nc;
            snprintf(g_loaded_id, sizeof g_loaded_id, "%s", id);
        }
        pthread_mutex_unlock(&g_mx);
        plog("tpemote: channel %s -> %d emotes", id, nc);
    }
    return NULL;
}

void tpemote_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 192 * 1024);
    pthread_create(&th, &at, worker, NULL);
}

void tpemote_set_channel(const char *id)
{
    pthread_mutex_lock(&g_mx);
    snprintf(g_pending_id, sizeof g_pending_id, "%s", id ? id : "");
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
}

int tpemote_lookup(const char *word, char *url, size_t cap)
{
    if (!word[0]) return 0;
    int r = 0;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < g_nchan && !r; i++) if (!strcmp(g_chan[i].name, word)) { snprintf(url, cap, "%s", g_chan[i].url); r = 1; }
    for (int i = 0; i < g_nglob && !r; i++) if (!strcmp(g_glob[i].name, word)) { snprintf(url, cap, "%s", g_glob[i].url); r = 1; }
    pthread_mutex_unlock(&g_mx);
    return r;
}
