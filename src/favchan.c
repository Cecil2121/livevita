#include "favchan.h"
#include "applog.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define FAVCHAN_PATH DATA_DIR "/channels.txt"

static TwStream g_ch[FAVCHAN_MAX];
static int g_n;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static void field(const char *line, int k, char *out, size_t cap)
{
    const char *p = line;
    for (int i = 0; i < k && p; i++) { p = strchr(p, '\t'); if (p) p++; }
    out[0] = 0;
    if (!p) return;
    size_t n = strcspn(p, "\t\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void save(void)                                     /* (locked) */
{
    FILE *f = fopen(FAVCHAN_PATH, "w");
    if (!f) { plog("favchan: cannot write %s", FAVCHAN_PATH); return; }
    for (int i = 0; i < g_n; i++) fprintf(f, "%s\t%s\t%s\t%s\n", g_ch[i].user_id, g_ch[i].login, g_ch[i].name, g_ch[i].avatar);
    fclose(f);
}

void favchan_load(void)
{
    pthread_mutex_lock(&g_mx);
    g_n = 0;
    FILE *f = fopen(FAVCHAN_PATH, "r");
    if (f) {
        char line[600];
        while (g_n < FAVCHAN_MAX && fgets(line, sizeof line, f)) {
            TwStream *s = &g_ch[g_n];
            memset(s, 0, sizeof *s);
            field(line, 0, s->user_id, sizeof s->user_id);
            field(line, 1, s->login, sizeof s->login);
            field(line, 2, s->name, sizeof s->name);
            field(line, 3, s->avatar, sizeof s->avatar);
            s->viewers = -1;
            s->saved = 1;
            if (s->user_id[0] && s->login[0]) g_n++;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_mx);
}

int favchan_count(void)
{
    pthread_mutex_lock(&g_mx);
    int n = g_n;
    pthread_mutex_unlock(&g_mx);
    return n;
}

static TwStream g_one;                                       /* (UI thread only) */
const TwStream *favchan_at(int i)
{
    pthread_mutex_lock(&g_mx);
    const TwStream *r = NULL;
    if (i >= 0 && i < g_n) { g_one = g_ch[i]; r = &g_one; }
    pthread_mutex_unlock(&g_mx);
    return r;
}

int favchan_has(const char *user_id)
{
    int r = 0;
    pthread_mutex_lock(&g_mx);
    for (int i = 0; i < g_n && !r; i++) if (!strcmp(g_ch[i].user_id, user_id)) r = 1;
    pthread_mutex_unlock(&g_mx);
    return r;
}

int favchan_toggle(const TwStream *s)
{
    int r = 0;
    pthread_mutex_lock(&g_mx);
    int found = -1;
    for (int i = 0; i < g_n; i++) if (!strcmp(g_ch[i].user_id, s->user_id)) found = i;
    if (found >= 0) {
        memmove(&g_ch[found], &g_ch[found + 1], sizeof g_ch[0] * (size_t)(g_n - found - 1));
        g_n--;
    } else if (g_n < FAVCHAN_MAX && s->user_id[0]) {
        TwStream *c = &g_ch[g_n++];
        memset(c, 0, sizeof *c);
        snprintf(c->user_id, sizeof c->user_id, "%s", s->user_id);
        snprintf(c->login, sizeof c->login, "%s", s->login);
        snprintf(c->name, sizeof c->name, "%s", s->name[0] ? s->name : s->login);
        snprintf(c->avatar, sizeof c->avatar, "%s", s->avatar);
        c->viewers = -1;
        c->saved = 1;
        r = 1;
    }
    save();
    pthread_mutex_unlock(&g_mx);
    return r;
}

int favchan_snapshot(TwStream *out, int max)
{
    pthread_mutex_lock(&g_mx);
    int n = g_n < max ? g_n : max;
    memcpy(out, g_ch, sizeof g_ch[0] * (size_t)n);
    pthread_mutex_unlock(&g_mx);
    return n;
}
