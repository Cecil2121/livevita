#ifndef LV_JSON_H
#define LV_JSON_H
/* Small helpers over jsmn (third_party/jsmn): parse a JSON text once, then look values up by key. */
#include <stddef.h>

#define JSMN_HEADER
#include "jsmn.h"

typedef struct {
    const char *text;
    jsmntok_t *tok;
    int ntok;
} Json;

/* Parses text (kept by the caller while the Json is used). 0 = ok. Free with json_free. */
int  json_parse(Json *j, const char *text);
void json_free(Json *j);

/* Index of the token after the one at i, skipping all of i's children. */
int  json_skip(const Json *j, int i);
/* Value token of key in the object at obj, or -1. */
int  json_key(const Json *j, int obj, const char *key);
/* i-th element of the array at arr, or -1. */
int  json_index(const Json *j, int arr, int i);
/* Number of elements / members of the array or object at i (0 if not one). */
int  json_size(const Json *j, int i);
/* Copies the string at i, with its escapes decoded (\n, \", \uXXXX -> UTF-8). Returns out ("" if not a string). */
const char *json_str(const Json *j, int i, char *out, size_t cap);
/* Number at i (or 0). */
long json_int(const Json *j, int i);
/* 1 if the token at i is the literal true. */
int  json_bool(const Json *j, int i);

/* Shortcut: json_str(json_key(obj, key)). */
const char *json_get_str(const Json *j, int obj, const char *key, char *out, size_t cap);
long json_get_int(const Json *j, int obj, const char *key);

#endif
