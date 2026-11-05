/* json.c - recursive-descent JSON parser into a small tree. */
#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int depth; } P;

static void ws(P *s) { while (*s->p && isspace((unsigned char)*s->p)) s->p++; }

static JNode *node(JType t) {
    JNode *n = calloc(1, sizeof *n);
    if (n) n->t = t;
    return n;
}

static char *parse_string(P *s) {
    if (*s->p != '"') return NULL;
    s->p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (*s->p && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\') {
            char e = *s->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*s->p); i++, s->p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*s->p) ? *s->p - '0' : (tolower((unsigned char)*s->p) - 'a' + 10));
                c = v < 128 ? (char)v : '?';
                break;
            }
            case 0: free(out); return NULL;
            default: c = e; /* \" \\ \/ */
            }
        }
        if (len + 2 > cap) { cap *= 2; char *t = realloc(out, cap); if (!t) { free(out); return NULL; } out = t; }
        out[len++] = c;
    }
    if (*s->p != '"') { free(out); return NULL; }
    s->p++;
    out[len] = 0;
    return out;
}

static JNode *value(P *s);

static JNode *container(P *s, bool obj) {
    JNode *n = node(obj ? J_OBJ : J_ARR), *last = NULL;
    if (!n || ++s->depth > 64) { json_free(n); return NULL; }
    s->p++;
    ws(s);
    if (*s->p == (obj ? '}' : ']')) { s->p++; s->depth--; return n; }
    for (;;) {
        char *key = NULL;
        ws(s);
        if (obj) {
            key = parse_string(s);
            ws(s);
            if (!key || *s->p != ':') { free(key); json_free(n); return NULL; }
            s->p++;
        }
        JNode *v = value(s);
        if (!v) { free(key); json_free(n); return NULL; }
        v->key = key;
        if (last) last->next = v; else n->child = v;
        last = v;
        ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == (obj ? '}' : ']')) { s->p++; s->depth--; return n; }
        json_free(n);
        return NULL;
    }
}

static JNode *value(P *s) {
    ws(s);
    char c = *s->p;
    if (c == '{') return container(s, true);
    if (c == '[') return container(s, false);
    if (c == '"') {
        char *str = parse_string(s);
        if (!str) return NULL;
        JNode *n = node(J_STR);
        if (!n) { free(str); return NULL; }
        n->str = str;
        return n;
    }
    if (!strncmp(s->p, "true", 4)) { s->p += 4; JNode *n = node(J_BOOL); if (n) n->b = true; return n; }
    if (!strncmp(s->p, "false", 5)) { s->p += 5; return node(J_BOOL); }
    if (!strncmp(s->p, "null", 4)) { s->p += 4; return node(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        double d = strtod(s->p, &end);
        if (end == s->p) return NULL;
        s->p = end;
        JNode *n = node(J_NUM);
        if (n) n->num = d;
        return n;
    }
    return NULL;
}

JNode *json_parse(const char *text) {
    if (!text) return NULL;
    P s = {text, 0};
    JNode *n = value(&s);
    if (!n) return NULL;
    ws(&s);
    if (*s.p) { json_free(n); return NULL; }
    return n;
}

void json_free(JNode *n) {
    while (n) {
        JNode *next = n->next;
        json_free(n->child);
        free(n->key);
        free(n->str);
        free(n);
        n = next;
    }
}

JNode *json_get(JNode *n, const char *path) {
    char part[64];
    while (n && path && *path) {
        const char *dot = strchr(path, '.');
        size_t l = dot ? (size_t)(dot - path) : strlen(path);
        if (l >= sizeof part) return NULL;
        memcpy(part, path, l);
        part[l] = 0;
        if (n->t != J_OBJ) return NULL;
        JNode *c = n->child;
        while (c && (!c->key || strcmp(c->key, part))) c = c->next;
        n = c;
        path = dot ? dot + 1 : NULL;
    }
    return n;
}

const char *json_str(JNode *n, const char *path, const char *def) {
    JNode *v = json_get(n, path);
    return v && v->t == J_STR ? v->str : def;
}

double json_num(JNode *n, const char *path, double def) {
    JNode *v = json_get(n, path);
    return v && v->t == J_NUM ? v->num : def;
}

bool json_bool(JNode *n, const char *path, bool def) {
    JNode *v = json_get(n, path);
    return v && v->t == J_BOOL ? v->b : def;
}
