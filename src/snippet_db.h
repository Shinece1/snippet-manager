/*
 * snippet_db.h - SQLite/FTS5 storage for the snippet manager.
 *
 * The database is deliberately self-contained: a normal `snippets` table keeps
 * the authoritative data and an external-content FTS5 table mirrors it for
 * full-text search.  Triggers keep the two in sync.
 */
#ifndef SNIPPET_DB_H
#define SNIPPET_DB_H

#include <stdbool.h>
#include <stddef.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnippetDB {
    sqlite3 *handle;
} SnippetDB;

/* One row of the left-hand list (no body, to keep filtering cheap). */
typedef struct SnippetSummary {
    sqlite3_int64 id;
    char *name;
    char *tags;
} SnippetSummary;

typedef struct SnippetList {
    SnippetSummary *items;
    size_t count;
    size_t capacity;
} SnippetList;

/* A fully loaded snippet (right-hand panel / editor). */
typedef struct Snippet {
    bool valid;
    sqlite3_int64 id;
    char *name;
    char *tags;
    char *description;
    char *body;
} Snippet;

void snippet_list_init(SnippetList *list);
void snippet_list_free(SnippetList *list);
bool snippet_list_contains(const SnippetList *list, sqlite3_int64 id);

void snippet_free(Snippet *snippet);

/*
 * Open (creating if needed) the database and make sure the schema and FTS5
 * index exist.  On failure a human-readable message is written to `err` and
 * to stderr, and the handle is closed again.
 */
bool snippet_db_open(SnippetDB *db, const char *path, char *err, size_t err_size);
void snippet_db_close(SnippetDB *db);

/*
 * Load the list.  When `search` is NULL/empty every snippet is returned.
 * Otherwise every whitespace-separated term must occur somewhere in the name
 * or tags (case-insensitive ASCII substring match).  This is deliberate:
 * FTS5's unicode61 tokenizer treats a whole Chinese phrase such as
 * "通用哈希表" as one token, so searching "哈希" could never match it.  The
 * FTS5 table is still maintained for full-text use and compatibility.
 *
 * `out` must have been initialised with snippet_list_init().  On failure the
 * old contents of `out` are released and it is left empty.
 */
bool snippet_db_list(SnippetDB *db, const char *search, SnippetList *out,
                     char *err, size_t err_size);

bool snippet_db_get(SnippetDB *db, sqlite3_int64 id, Snippet *out,
                    char *err, size_t err_size);

bool snippet_db_insert(SnippetDB *db, const char *name, const char *tags,
                       const char *description, const char *body,
                       sqlite3_int64 *out_id, char *err, size_t err_size);

bool snippet_db_update(SnippetDB *db, sqlite3_int64 id, const char *name,
                       const char *tags, const char *description,
                       const char *body, char *err, size_t err_size);

bool snippet_db_delete(SnippetDB *db, sqlite3_int64 id,
                       char *err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif /* SNIPPET_DB_H */
