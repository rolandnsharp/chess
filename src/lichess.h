/* lichess.h - lichess Board API over libcurl. Streams run on their own
 * threads and hand complete NDJSON lines to the window loop. */
#ifndef LICHESS_H
#define LICHESS_H

#include <stdbool.h>
#include <stddef.h>

#define LICHESS_API "https://lichess.org"

typedef struct LStream LStream;

/* GET (body NULL) or POST (form body); runs until closed or the server ends it */
LStream *ls_open(const char *path, const char *body, const char *token);
char *ls_next_line(LStream *s); /* malloc'd line without newline, or NULL */
/* true once the request has ended; then *http is the status and err holds
 * any error body (for 4xx) or transport error */
bool ls_finished(LStream *s, long *http, char *err, size_t errlen);
void ls_close(LStream *s);

/* a short POST in the background; failures are kept for lichess_error() */
void lichess_post(const char *path, const char *body, const char *token);
bool lichess_error(char *out, size_t n);

/* blocking GET with a timeout, for checking the token; malloc'd body or NULL */
char *lichess_get(const char *path, const char *token, long *http, int timeout_s);

#endif
