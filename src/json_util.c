/*
 * json_util.c — Minimal JSON escape + field extractors.
 * See include/json_util.h for usage contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_util.h"

char *json_escape(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  *q++ = '\\'; *q++ = '"';  break;
            case '\\': *q++ = '\\'; *q++ = '\\'; break;
            case '\n': *q++ = '\\'; *q++ = 'n';  break;
            case '\r': *q++ = '\\'; *q++ = 'r';  break;
            case '\t': *q++ = '\\'; *q++ = 't';  break;
            default:
                if (c < 0x20) q += sprintf(q, "\\u%04x", c);
                else          *q++ = (char)c;
        }
    }
    *q = '\0';
    return out;
}

char *json_extract_string(const char *json, const char *key) {
    if (!json || !key) return NULL;

    size_t klen = strlen(key);
    char  *needle = malloc(klen + 4);
    if (!needle) return NULL;
    snprintf(needle, klen + 4, "\"%s\"", key);

    const char *p = strstr(json, needle);
    free(needle);
    if (!p) return NULL;
    p += klen + 2;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;

    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p += 2;
        else                       p++;
    }
    if (*p != '"') return NULL;

    size_t len = (size_t)(p - start);
    char  *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

int json_extract_int(const char *json, const char *key, int def) {
    if (!json || !key) return def;

    size_t klen = strlen(key);
    char *needle = malloc(klen + 4);
    if (!needle) return def;
    snprintf(needle, klen + 4, "\"%s\"", key);

    const char *p = strstr(json, needle);
    free(needle);
    if (!p) return def;
    p += klen + 2;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return def;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return atoi(p);
    return def;
}
