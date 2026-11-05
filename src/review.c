/* review.c - loading games for self-review. */
#include "review.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

double review_wp(int cp) {
    if (cp > 1500) cp = 1500;
    if (cp < -1500) cp = -1500;
    return 1.0 / (1.0 + exp(-0.00368208 * cp));
}

double review_score(const ReviewGame *g) {
    if (!strcmp(g->result, "1/2-1/2")) return 0.5;
    bool white_won = !strcmp(g->result, "1-0");
    return white_won == g->me_white ? 1.0 : 0.0;
}

static char *next_field(char **s) {
    char *start = *s;
    if (!start) return "";
    char *tab = strchr(start, '\t');
    if (tab) { *tab = 0; *s = tab + 1; }
    else { *s = NULL; char *nl = strchr(start, '\n'); if (nl) *nl = 0; }
    return start;
}

bool reviews_load(ReviewSet *s, const char *base_dir, char *err, size_t errlen) {
    memset(s, 0, sizeof *s);
    char path[1100];
    snprintf(path, sizeof path, "%sdata/analysis/drills/games.tsv", base_dir);
    snprintf(s->log_path, sizeof s->log_path, "%sdata/review_log.tsv", base_dir);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errlen, "No analysed games: run make update (or analysis/report.py)."); return false; }
    char *line = NULL;
    size_t cap = 0;
    int alloc = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        if (s->n == alloc) {
            alloc = alloc ? alloc * 2 : 64;
            s->g = realloc(s->g, (size_t)alloc * sizeof *s->g);
        }
        ReviewGame *g = &s->g[s->n];
        memset(g, 0, sizeof *g);
        char *p = line;
        snprintf(g->id, sizeof g->id, "%s", next_field(&p));
        snprintf(g->date, sizeof g->date, "%s", next_field(&p));
        snprintf(g->speed, sizeof g->speed, "%s", next_field(&p));
        snprintf(g->white, sizeof g->white, "%s", next_field(&p));
        snprintf(g->black, sizeof g->black, "%s", next_field(&p));
        g->me_white = atoi(next_field(&p)) != 0;
        snprintf(g->result, sizeof g->result, "%s", next_field(&p));
        snprintf(g->opening, sizeof g->opening, "%s", next_field(&p));
        snprintf(g->link, sizeof g->link, "%s", next_field(&p));
        char *sans = next_field(&p), *evals = next_field(&p), *best = next_field(&p);
        char *save = NULL;
        for (char *t = strtok_r(sans, " ", &save); t && g->n < RV_MAX_PLY; t = strtok_r(NULL, " ", &save))
            snprintf(g->san[g->n++], sizeof g->san[0], "%.9s", t);
        int k = 0;
        save = NULL;
        for (char *t = strtok_r(evals, ",", &save); t && k <= g->n; t = strtok_r(NULL, ",", &save))
            g->eval[k++] = atoi(t);
        k = 0;
        save = NULL;
        for (char *t = strtok_r(best, " ", &save); t && k < g->n; t = strtok_r(NULL, " ", &save))
            snprintf(g->best[k++], sizeof g->best[0], "%.9s", t);
        if (g->n >= 10) s->n++;
    }
    free(line);
    fclose(f);
    /* games already self-reviewed */
    FILE *lf = fopen(s->log_path, "r");
    if (lf) {
        char l[512];
        while (fgets(l, sizeof l, lf)) {
            char id[32];
            if (sscanf(l, "%*s %*s %31s", id) != 1) continue;
            for (int i = 0; i < s->n; i++)
                if (!strcmp(s->g[i].id, id)) s->g[i].reviewed = true;
        }
        fclose(lf);
    }
    if (!s->n) { snprintf(err, errlen, "No games to review."); return false; }
    return true;
}

void reviews_free(ReviewSet *s) {
    free(s->g);
    memset(s, 0, sizeof *s);
}
