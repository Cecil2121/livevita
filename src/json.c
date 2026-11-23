#include "jsmn.h"                       /* the implementation, once (before json.h, which includes only its declarations) */
#include "json.h"
#include <stdlib.h>
#include <string.h>

int json_parse(Json *j, const char *text)
{
    memset(j, 0, sizeof *j);
    j->text = text;
    size_t len = strlen(text);
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, text, len, NULL, 0);          /* count first */
    if (n <= 0) return -1;
    j->tok = malloc(sizeof(jsmntok_t) * (size_t)n);
    if (!j->tok) return -1;
    jsmn_init(&p);
    j->ntok = jsmn_parse(&p, text, len, j->tok, (unsigned)n);
    if (j->ntok <= 0) { json_free(j); return -1; }
    return 0;
}

void json_free(Json *j)
{
    free(j->tok);
    j->tok = NULL;
    j->ntok = 0;
}

int json_skip(const Json *j, int i)
{
    if (i < 0 || i >= j->ntok) return j->ntok;
    int end = j->tok[i].end, k = i + 1;
    while (k < j->ntok && j->tok[k].start < end) k++;
    return k;
}

static int tok_eq(const Json *j, int i, const char *s)
{
    const jsmntok_t *t = &j->tok[i];
    size_t n = (size_t)(t->end - t->start);
    return t->type == JSMN_STRING && strlen(s) == n && !strncmp(j->text + t->start, s, n);
}

int json_key(const Json *j, int obj, const char *key)
{
    if (obj < 0 || obj >= j->ntok || j->tok[obj].type != JSMN_OBJECT) return -1;
    int k = obj + 1;
    for (int m = 0; m < j->tok[obj].size && k + 1 < j->ntok; m++) {
        if (tok_eq(j, k, key)) return k + 1;
        k = json_skip(j, k + 1);                          /* past the key's value */
    }
    return -1;
}

int json_index(const Json *j, int arr, int i)
{
    if (arr < 0 || arr >= j->ntok || j->tok[arr].type != JSMN_ARRAY || i < 0 || i >= j->tok[arr].size) return -1;
    int k = arr + 1;
    for (int m = 0; m < i; m++) k = json_skip(j, k);
    return k < j->ntok ? k : -1;
}

int json_size(const Json *j, int i)
{
    if (i < 0 || i >= j->ntok) return 0;
    return j->tok[i].type == JSMN_ARRAY || j->tok[i].type == JSMN_OBJECT ? j->tok[i].size : 0;
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static unsigned read_u4(const char *s)
{
    unsigned v = 0;
    for (int k = 0; k < 4; k++) { int h = hexv(s[k]); if (h < 0) return 0xFFFD; v = v * 16 + (unsigned)h; }
    return v;
}

static size_t put_utf8(char *o, size_t room, unsigned cp)
{
    if (cp < 0x80) { if (room < 1) return 0; o[0] = (char)cp; return 1; }
    if (cp < 0x800) { if (room < 2) return 0; o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        if (room < 3) return 0;
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    if (room < 4) return 0;
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

const char *json_str(const Json *j, int i, char *out, size_t cap)
{
    if (!cap) return out;
    out[0] = 0;
    if (i < 0 || i >= j->ntok) return out;
    const jsmntok_t *t = &j->tok[i];
    if (t->type != JSMN_STRING && t->type != JSMN_PRIMITIVE) return out;
    const char *s = j->text + t->start, *e = j->text + t->end;
    if (t->type == JSMN_PRIMITIVE && !strncmp(s, "null", 4)) return out;
    size_t o = 0;
    while (s < e && o + 1 < cap) {
        if (*s != '\\' || s + 1 >= e) { out[o++] = *s++; continue; }
        s++;
        char c = *s++;
        switch (c) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = ' '; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u':
            if (e - s >= 4) {
                unsigned cp = read_u4(s);
                s += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && e - s >= 6 && s[0] == '\\' && s[1] == 'u') {   /* surrogate pair */
                    unsigned lo = read_u4(s + 2);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
                }
                o += put_utf8(out + o, cap - 1 - o, cp);
            }
            break;
        default: out[o++] = c; break;                    /* \" \\ \/ */
        }
    }
    out[o] = 0;
    return out;
}

long json_int(const Json *j, int i)
{
    if (i < 0 || i >= j->ntok || j->tok[i].type != JSMN_PRIMITIVE) return 0;
    return strtol(j->text + j->tok[i].start, NULL, 10);
}

int json_bool(const Json *j, int i)
{
    return i >= 0 && i < j->ntok && j->tok[i].type == JSMN_PRIMITIVE && j->text[j->tok[i].start] == 't';
}

const char *json_get_str(const Json *j, int obj, const char *key, char *out, size_t cap)
{
    return json_str(j, json_key(j, obj, key), out, cap);
}

long json_get_int(const Json *j, int obj, const char *key)
{
    return json_int(j, json_key(j, obj, key));
}
