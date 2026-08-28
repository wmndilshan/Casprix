/*
 * document.c — in-memory document store keyed by URI.
 * Each document caches its parse + semantic-analysis result.
 */
#define _POSIX_C_SOURCE 200809L
#include "lsp.h"

#include <stdlib.h>
#include <string.h>

static Document *g_docs = NULL;

void docstore_init(void)     { g_docs = NULL; }

void docstore_shutdown(void) {
    Document *d = g_docs;
    while (d) {
        Document *next = d->next;
        analysis_free(d);
        free(d->uri);
        free(d->text);
        free(d);
        d = next;
    }
    g_docs = NULL;
}

Document *doc_get(const char *uri) {
    for (Document *d = g_docs; d; d = d->next)
        if (strcmp(d->uri, uri) == 0) return d;
    return NULL;
}

Document *doc_put(const char *uri, const char *text, int version) {
    Document *d = doc_get(uri);
    if (d) {
        free(d->text);
        d->text = strdup(text ? text : "");
        d->version = version;
        analysis_free(d); /* invalidate cached analysis */
        return d;
    }
    d = (Document *)calloc(1, sizeof(Document));
    d->uri = strdup(uri);
    d->text = strdup(text ? text : "");
    d->version = version;
    d->next = g_docs;
    g_docs = d;
    return d;
}

void doc_remove(const char *uri) {
    Document **pp = &g_docs;
    while (*pp) {
        if (strcmp((*pp)->uri, uri) == 0) {
            Document *dead = *pp;
            *pp = dead->next;
            analysis_free(dead);
            free(dead->uri);
            free(dead->text);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}
