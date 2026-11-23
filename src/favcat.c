#include "favcat.h"
#include "applog.h"
#include <stdio.h>
#include <string.h>

#define FAVCAT_PATH DATA_DIR "/categories.txt"

static TwGame g_fav[FAVCAT_MAX];
static int g_n;

static void save(void)
{
    FILE *f = fopen(FAVCAT_PATH, "w");
    if (!f) { plog("favcat: cannot write %s", FAVCAT_PATH); return; }
    for (int i = 0; i < g_n; i++) fprintf(f, "%s\t%s\t%s\t%s\n", g_fav[i].id, g_fav[i].name, g_fav[i].box_art, g_fav[i].box_art_big);
    fclose(f);
}

/* copies field k (tab separated) of a line */
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

void favcat_load(void)
{
    g_n = 0;
    FILE *f = fopen(FAVCAT_PATH, "r");
    if (!f) return;
    char line[700];
    while (g_n < FAVCAT_MAX && fgets(line, sizeof line, f)) {
        TwGame *g = &g_fav[g_n];
        memset(g, 0, sizeof *g);
        field(line, 0, g->id, sizeof g->id);
        field(line, 1, g->name, sizeof g->name);
        field(line, 2, g->box_art, sizeof g->box_art);
        field(line, 3, g->box_art_big, sizeof g->box_art_big);
        g->followed = 1;
        if (g->id[0] && g->name[0]) g_n++;
    }
    fclose(f);
}

int favcat_count(void) { return g_n; }
const TwGame *favcat_get(int i) { return i >= 0 && i < g_n ? &g_fav[i] : NULL; }

int favcat_has(const char *id)
{
    for (int i = 0; i < g_n; i++) if (!strcmp(g_fav[i].id, id)) return 1;
    return 0;
}

int favcat_toggle(const TwGame *g)
{
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_fav[i].id, g->id)) {                   /* unfollow */
            memmove(&g_fav[i], &g_fav[i + 1], sizeof g_fav[0] * (size_t)(g_n - i - 1));
            g_n--;
            save();
            return 0;
        }
    if (g_n >= FAVCAT_MAX) return 0;
    g_fav[g_n] = *g;
    g_fav[g_n].followed = 1;
    g_n++;
    save();
    return 1;
}

int favcat_merge(const TwGame *top, int ntop, TwGame *out, int max)
{
    int n = 0;
    for (int i = 0; i < g_n && n < max; i++) {
        out[n] = g_fav[i];
        for (int k = 0; k < ntop; k++)                       /* fresher picture addresses from Twitch, if listed */
            if (!strcmp(top[k].id, g_fav[i].id)) { out[n] = top[k]; break; }
        out[n].followed = 1;
        n++;
    }
    for (int k = 0; k < ntop && n < max; k++)
        if (!favcat_has(top[k].id)) { out[n] = top[k]; out[n].followed = 0; n++; }
    return n;
}
