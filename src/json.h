/* json.h - a small JSON reader, enough for the lichess API. */
#ifndef JSON_H
#define JSON_H

#include <stdbool.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode {
    JType t;
    char *key;            /* member name inside an object */
    char *str;            /* J_STR */
    double num;           /* J_NUM */
    bool b;               /* J_BOOL */
    struct JNode *child;  /* first element/member */
    struct JNode *next;
} JNode;

JNode *json_parse(const char *text); /* NULL on a syntax error */
void json_free(JNode *n);
/* dotted path: json_get(n, "game.white.name") */
JNode *json_get(JNode *n, const char *path);
const char *json_str(JNode *n, const char *path, const char *def);
double json_num(JNode *n, const char *path, double def);
bool json_bool(JNode *n, const char *path, bool def);

#endif
