/* lichess.c - threaded libcurl streams and requests for the Board API. */
#include "lichess.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UA "notation-trainer/0.1 (+https://github.com/rolandnsharp/chess)"
#define QMAX 256

struct LStream {
    pthread_t thread;
    char url[512], body[512], auth[256];
    atomic_int stop, done;
    pthread_mutex_t mu;
    char *queue[QMAX];
    int qhead, qtail;
    char *partial;
    size_t plen;
    long http;
    char err[256];
};

static pthread_once_t once = PTHREAD_ONCE_INIT;

/* CHESS_LICHESS_URL points the client at a test server */
static const char *base_url(void) {
    const char *u = getenv("CHESS_LICHESS_URL");
    return u && *u ? u : LICHESS_API;
}
static void init_curl(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static void push_line(LStream *s, const char *p, size_t n) {
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
    if (!n) return; /* keep-alive */
    char *line = malloc(n + 1);
    if (!line) return;
    memcpy(line, p, n);
    line[n] = 0;
    pthread_mutex_lock(&s->mu);
    int next = (s->qtail + 1) % QMAX;
    if (next == s->qhead) free(line); /* window loop stalled: drop */
    else { s->queue[s->qtail] = line; s->qtail = next; }
    pthread_mutex_unlock(&s->mu);
}

static size_t on_data(char *data, size_t sz, size_t nm, void *ud) {
    LStream *s = ud;
    size_t n = sz * nm;
    char *buf = realloc(s->partial, s->plen + n + 1);
    if (!buf) return 0;
    s->partial = buf;
    memcpy(buf + s->plen, data, n);
    s->plen += n;
    size_t start = 0;
    for (size_t i = 0; i < s->plen; i++)
        if (buf[i] == '\n') { push_line(s, buf + start, i - start); start = i + 1; }
    memmove(buf, buf + start, s->plen - start);
    s->plen -= start;
    if (s->plen > 1 << 20) s->plen = 0; /* runaway line */
    return n;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return atomic_load(&((LStream *)ud)->stop) ? 1 : 0;
}

static void *run(void *arg) {
    LStream *s = arg;
    CURL *c = curl_easy_init();
    if (!c) { snprintf(s->err, sizeof s->err, "curl init failed"); atomic_store(&s->done, 1); return NULL; }
    struct curl_slist *h = curl_slist_append(NULL, s->auth);
    h = curl_slist_append(h, "Accept: application/x-ndjson");
    curl_easy_setopt(c, CURLOPT_URL, s->url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    /* streams send a keep-alive newline every few seconds; silence for a
     * minute means the connection is dead */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    if (s->body[0]) curl_easy_setopt(c, CURLOPT_POSTFIELDS, s->body);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &s->http);
    if (s->plen) { push_line(s, s->partial, s->plen); s->plen = 0; }
    if (rc != CURLE_OK && !atomic_load(&s->stop)) snprintf(s->err, sizeof s->err, "%s", curl_easy_strerror(rc));
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    atomic_store(&s->done, 1);
    return NULL;
}

LStream *ls_open(const char *path, const char *body, const char *token) {
    pthread_once(&once, init_curl);
    LStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->url, sizeof s->url, "%s%s", base_url(), path);
    snprintf(s->body, sizeof s->body, "%s", body ? body : "");
    snprintf(s->auth, sizeof s->auth, "Authorization: Bearer %s", token);
    pthread_mutex_init(&s->mu, NULL);
    if (pthread_create(&s->thread, NULL, run, s) != 0) { free(s); return NULL; }
    return s;
}

char *ls_next_line(LStream *s) {
    if (!s) return NULL;
    char *line = NULL;
    pthread_mutex_lock(&s->mu);
    if (s->qhead != s->qtail) { line = s->queue[s->qhead]; s->qhead = (s->qhead + 1) % QMAX; }
    pthread_mutex_unlock(&s->mu);
    return line;
}

bool ls_finished(LStream *s, long *http, char *err, size_t errlen) {
    if (!s || !atomic_load(&s->done)) return false;
    /* only finished once every line has been read */
    pthread_mutex_lock(&s->mu);
    bool empty = s->qhead == s->qtail;
    pthread_mutex_unlock(&s->mu);
    if (!empty) return false;
    if (http) *http = s->http;
    if (err && errlen) snprintf(err, errlen, "%s", s->err);
    return true;
}

void ls_close(LStream *s) {
    if (!s) return;
    atomic_store(&s->stop, 1);
    pthread_join(s->thread, NULL);
    char *l;
    while ((l = ls_next_line(s))) free(l);
    free(s->partial);
    pthread_mutex_destroy(&s->mu);
    free(s);
}

/* ------------------------------------------------------- one-off posts */

static pthread_mutex_t err_mu = PTHREAD_MUTEX_INITIALIZER;
static char last_err[300];

typedef struct { char url[512], body[256], auth[256]; } Post;

static size_t collect(char *data, size_t sz, size_t nm, void *ud) {
    char *buf = ud;
    size_t have = strlen(buf), n = sz * nm, room = 299 - have;
    memcpy(buf + have, data, n < room ? n : room);
    buf[have + (n < room ? n : room)] = 0;
    return n;
}

static void *post_run(void *arg) {
    Post *p = arg;
    char resp[300] = "";
    CURL *c = curl_easy_init();
    if (c) {
        struct curl_slist *h = curl_slist_append(NULL, p->auth);
        curl_easy_setopt(c, CURLOPT_URL, p->url);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, p->body);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        CURLcode rc = curl_easy_perform(c);
        long http = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http);
        if (rc != CURLE_OK || http >= 400) {
            pthread_mutex_lock(&err_mu);
            if (rc != CURLE_OK) snprintf(last_err, sizeof last_err, "%s", curl_easy_strerror(rc));
            else snprintf(last_err, sizeof last_err, "lichess said %ld: %s", http, resp);
            pthread_mutex_unlock(&err_mu);
        }
        curl_slist_free_all(h);
        curl_easy_cleanup(c);
    }
    free(p);
    return NULL;
}

void lichess_post(const char *path, const char *body, const char *token) {
    pthread_once(&once, init_curl);
    Post *p = calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->url, sizeof p->url, "%s%s", base_url(), path);
    snprintf(p->body, sizeof p->body, "%s", body ? body : "");
    snprintf(p->auth, sizeof p->auth, "Authorization: Bearer %s", token);
    pthread_t t;
    if (pthread_create(&t, NULL, post_run, p) == 0) pthread_detach(t);
    else free(p);
}

bool lichess_error(char *out, size_t n) {
    pthread_mutex_lock(&err_mu);
    bool have = last_err[0] != 0;
    if (have) { snprintf(out, n, "%s", last_err); last_err[0] = 0; }
    pthread_mutex_unlock(&err_mu);
    return have;
}

typedef struct { char *buf; size_t len; } Buf;

static size_t grow(char *data, size_t sz, size_t nm, void *ud) {
    Buf *b = ud;
    size_t n = sz * nm;
    char *t = realloc(b->buf, b->len + n + 1);
    if (!t) return 0;
    b->buf = t;
    memcpy(t + b->len, data, n);
    b->len += n;
    t[b->len] = 0;
    return n;
}

char *lichess_get(const char *path, const char *token, long *http, int timeout_s) {
    pthread_once(&once, init_curl);
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    char url[512], auth[256];
    snprintf(url, sizeof url, "%s%s", base_url(), path);
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    struct curl_slist *h = curl_slist_append(NULL, auth);
    Buf b = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, UA);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, grow);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    *http = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { free(b.buf); return NULL; }
    return b.buf ? b.buf : calloc(1, 1);
}
