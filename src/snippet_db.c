/*
 * snippet_db.c - SQLite + FTS5 persistence layer.
 *
 * Design notes
 * ------------
 *  - `snippets` is the source of truth.
 *  - `snippets_fts` is an external-content FTS5 table.  Triggers mirror
 *    inserts/updates/deletes into it, so no special code path can forget the
 *    index.
 *  - At startup the index is rebuilt once.  This repairs databases that were
 *    modified by older builds or by external tools and costs very little for
 *    a local snippet collection.
 *  - The UI list searches name + tags with instr() substring matching rather
 *    than FTS5 MATCH: unicode61 treats a complete Chinese phrase such as
 *    "通用哈希表" as one token, so searching "哈希" would otherwise miss it.
 *  - Every user supplied string is bound as a parameter.
 */
#include "snippet_db.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHEMA_SQL                                                              \
    "CREATE TABLE IF NOT EXISTS snippets ("                                     \
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"                           \
    "  name       TEXT NOT NULL,"                                               \
    "  tags       TEXT NOT NULL DEFAULT '',"                                    \
    "  description TEXT NOT NULL DEFAULT '',"                                   \
    "  body       TEXT NOT NULL,"                                               \
    "  created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"\
    "  updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))" \
    ");"                                                                        \
    "CREATE VIRTUAL TABLE IF NOT EXISTS snippets_fts USING fts5("               \
    "  name, tags, body,"                                                       \
    "  content='snippets',"                                                     \
    "  content_rowid='id',"                                                     \
    "  tokenize='unicode61 remove_diacritics 2'"                                 \
    ");"                                                                        \
    "CREATE TRIGGER IF NOT EXISTS snippets_fts_ai AFTER INSERT ON snippets BEGIN"\
    "  INSERT INTO snippets_fts(rowid, name, tags, body)"                       \
    "  VALUES (new.id, new.name, new.tags, new.body);"                          \
    "END;"                                                                      \
    "CREATE TRIGGER IF NOT EXISTS snippets_fts_ad AFTER DELETE ON snippets BEGIN"\
    "  INSERT INTO snippets_fts(snippets_fts, rowid, name, tags, body)"         \
    "  VALUES ('delete', old.id, old.name, old.tags, old.body);"                \
    "END;"                                                                      \
    "CREATE TRIGGER IF NOT EXISTS snippets_fts_au AFTER UPDATE ON snippets BEGIN"\
    "  INSERT INTO snippets_fts(snippets_fts, rowid, name, tags, body)"         \
    "  VALUES ('delete', old.id, old.name, old.tags, old.body);"                \
    "  INSERT INTO snippets_fts(rowid, name, tags, body)"                       \
    "  VALUES (new.id, new.name, new.tags, new.body);"                          \
    "END;"

static char *dup_text(const unsigned char *text)
{
    const char *src = text ? (const char *)text : "";
    size_t len = strlen(src) + 1;
    char *copy = (char *)malloc(len);
    if (copy) {
        memcpy(copy, src, len);
    }
    return copy;
}

static void report_error(SnippetDB *db, char *err, size_t err_size,
                         const char *what, const char *sqlite_message)
{
    char message[1024];
    const char *detail = sqlite_message;

    if (!detail || !*detail) {
        detail = (db && db->handle) ? sqlite3_errmsg(db->handle)
                                    : "unknown database error";
    }

    snprintf(message, sizeof(message), "%s: %s", what ? what : "SQLite error",
             detail);
    if (err && err_size > 0) {
        snprintf(err, err_size, "%s", message);
    }
    fprintf(stderr, "[snippet-db] %s\n", message);
}

static bool exec_sql(SnippetDB *db, const char *sql, const char *what,
                     char *err, size_t err_size)
{
    char *sqlite_message = NULL;
    int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &sqlite_message);

    if (rc != SQLITE_OK) {
        report_error(db, err, err_size, what,
                     sqlite_message ? sqlite_message : sqlite3_errmsg(db->handle));
        if (sqlite_message &&
            (strstr(sqlite_message, "fts5") || strstr(sqlite_message, "FTS5") ||
             strstr(sqlite_message, "no such module"))) {
            fprintf(stderr,
                    "[snippet-db] hint: this SQLite build has no FTS5 module. "
                    "Build it with -DSQLITE_ENABLE_FTS5 (the bundled CMake "
                    "build does this automatically).\n");
        }
        sqlite3_free(sqlite_message);
        return false;
    }

    sqlite3_free(sqlite_message);
    return true;
}

static bool prepare_stmt(SnippetDB *db, const char *sql, sqlite3_stmt **stmt,
                         const char *what, char *err, size_t err_size)
{
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        report_error(db, err, err_size, what, sqlite3_errmsg(db->handle));
        *stmt = NULL;
        return false;
    }
    return true;
}

void snippet_list_init(SnippetList *list)
{
    if (!list) {
        return;
    }
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void snippet_list_free(SnippetList *list)
{
    size_t i;

    if (!list) {
        return;
    }
    for (i = 0; i < list->count; ++i) {
        free(list->items[i].name);
        free(list->items[i].tags);
    }
    free(list->items);
    snippet_list_init(list);
}

bool snippet_list_contains(const SnippetList *list, sqlite3_int64 id)
{
    size_t i;

    if (!list) {
        return false;
    }
    for (i = 0; i < list->count; ++i) {
        if (list->items[i].id == id) {
            return true;
        }
    }
    return false;
}

void snippet_free(Snippet *snippet)
{
    if (!snippet) {
        return;
    }
    free(snippet->name);
    free(snippet->tags);
    free(snippet->description);
    free(snippet->body);
    memset(snippet, 0, sizeof(*snippet));
}

static bool list_push(SnippetList *list, sqlite3_int64 id, const char *name,
                      const char *tags, char *err, size_t err_size)
{
    SnippetSummary *item;

    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity ? list->capacity * 2 : 32;
        SnippetSummary *new_items = (SnippetSummary *)realloc(
            list->items, new_capacity * sizeof(*new_items));
        if (!new_items) {
            report_error(NULL, err, err_size, "out of memory",
                         "could not grow the result list");
            return false;
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }

    item = &list->items[list->count];
    item->id = id;
    item->name = dup_text((const unsigned char *)(name ? name : ""));
    item->tags = dup_text((const unsigned char *)(tags ? tags : ""));
    if (!item->name || !item->tags) {
        free(item->name);
        free(item->tags);
        item->name = NULL;
        item->tags = NULL;
        report_error(NULL, err, err_size, "out of memory",
                     "could not copy a result row");
        return false;
    }

    ++list->count;
    return true;
}

/*
 * Split the search box into whitespace-separated terms and build a small SQL
 * query whose every term must occur as a substring of the name or the tags.
 *
 * We intentionally do not use FTS5 MATCH for this list:
 * FTS5's unicode61 tokenizer indexes "通用哈希表" as a single token, so a
 * search for "哈希" (a term in the middle) would never match.  `instr()` gives
 * the expected substring behaviour and the local dataset is tiny.  The FTS5
 * table is still maintained for full-text use and compatibility.
 */
#define SEARCH_MAX_TOKENS 64
#define SEARCH_SQL_CAPACITY 16384

static bool build_search_sql(const char *input, char *sql, size_t sql_capacity,
                             char **tokens, int *token_count, char **storage,
                             char *err, size_t err_size)
{
    char *copy;
    char *p;
    int count = 0;
    int written;
    int i;

    *token_count = 0;
    *storage = NULL;
    if (sql_capacity > 0) {
        sql[0] = '\0';
    }
    if (!input || !*input) {
        return true;
    }

    copy = (char *)malloc(strlen(input) + 1);
    if (!copy) {
        report_error(NULL, err, err_size, "out of memory",
                     "could not copy the search text");
        return false;
    }
    strcpy(copy, input);

    p = copy;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (count == SEARCH_MAX_TOKENS) {
            free(copy);
            report_error(NULL, err, err_size, "query too complex",
                         "too many search terms");
            return false;
        }
        tokens[count++] = p;
        while (*p && !isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p) {
            *p++ = '\0';
        }
    }

    if (count == 0) {
        free(copy);
        return true;
    }

    written = snprintf(sql, sql_capacity,
                       "SELECT id, name, tags FROM snippets WHERE ");
    for (i = 0; i < count; ++i) {
        int n = snprintf(
            sql + written, sql_capacity - (size_t)written,
            "%s(instr(lower(name), lower(?%d)) > 0 "
            "OR instr(lower(tags), lower(?%d)) > 0)",
            i == 0 ? "" : " AND ", i + 1, i + 1);
        if (n < 0 || (size_t)n >= sql_capacity - (size_t)written) {
            free(copy);
            report_error(NULL, err, err_size, "query too long",
                         "the search text is too large");
            return false;
        }
        written += n;
    }

    if ((size_t)written + 64 >= sql_capacity) {
        free(copy);
        report_error(NULL, err, err_size, "query too long",
                     "the search text is too large");
        return false;
    }
    snprintf(sql + written, sql_capacity - (size_t)written,
             " ORDER BY updated_at DESC, id DESC;");

    *token_count = count;
    *storage = copy;
    return true;
}

static bool column_exists(SnippetDB *db, const char *table,
                          const char *column, bool *exists, char *err,
                          size_t err_size)
{
    char sql[128];
    sqlite3_stmt *stmt = NULL;
    int rc;

    *exists = false;
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    if (!prepare_stmt(db, sql, &stmt, "could not inspect the table schema", err,
                      err_size)) {
        return false;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && sqlite3_stricmp((const char *)name, column) == 0) {
            *exists = true;
            break;
        }
    }

    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        report_error(db, err, err_size, "could not inspect the table schema",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool snippet_db_open(SnippetDB *db, const char *path, char *err,
                     size_t err_size)
{
    int rc;
    char *sqlite_message = NULL;

    if (!db) {
        return false;
    }
    memset(db, 0, sizeof(*db));

    if (!path || !*path) {
        path = "snippets.db";
    }

    rc = sqlite3_open_v2(path, &db->handle,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[snippet-db] cannot open database '%s': %s\n", path,
                db->handle ? sqlite3_errmsg(db->handle) : sqlite3_errstr(rc));
        if (err && err_size > 0) {
            snprintf(err, err_size, "cannot open database '%s': %s", path,
                     db->handle ? sqlite3_errmsg(db->handle)
                                : sqlite3_errstr(rc));
        }
        if (db->handle) {
            sqlite3_close(db->handle);
            db->handle = NULL;
        }
        return false;
    }

    sqlite3_busy_timeout(db->handle, 5000);
    sqlite3_extended_result_codes(db->handle, 1);

    /* WAL is a nice-to-have; a read-only location may refuse it, and that is
     * not a fatal condition for this single-process application. */
    rc = sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL,
                      &sqlite_message);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[snippet-db] warning: could not enable WAL: %s\n",
                sqlite_message ? sqlite_message : sqlite3_errmsg(db->handle));
    }
    sqlite3_free(sqlite_message);
    sqlite_message = NULL;

    if (!exec_sql(db, "PRAGMA foreign_keys=ON;", "could not enable foreign keys",
                  err, err_size) ||
        !exec_sql(db, SCHEMA_SQL, "could not create the database schema", err,
                  err_size)) {
        snippet_db_close(db);
        return false;
    }

    {
        bool has_description = false;
        if (!column_exists(db, "snippets", "description", &has_description, err,
                           err_size)) {
            snippet_db_close(db);
            return false;
        }
        if (!has_description &&
            !exec_sql(db,
                      "ALTER TABLE snippets ADD COLUMN description TEXT NOT "
                      "NULL DEFAULT '';",
                      "could not add the description column", err, err_size)) {
            snippet_db_close(db);
            return false;
        }
    }

    if (!exec_sql(db, "INSERT INTO snippets_fts(snippets_fts) VALUES('rebuild');",
                  "could not rebuild the FTS5 index", err, err_size)) {
        snippet_db_close(db);
        return false;
    }

    fprintf(stderr, "[snippet-db] using database: %s\n", path);
    return true;
}

void snippet_db_close(SnippetDB *db)
{
    if (db && db->handle) {
        int rc = sqlite3_close_v2(db->handle);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "[snippet-db] warning: sqlite3_close failed: %s\n",
                    sqlite3_errmsg(db->handle));
        }
        db->handle = NULL;
    }
}

bool snippet_db_list(SnippetDB *db, const char *search, SnippetList *out,
                     char *err, size_t err_size)
{
    static const char *LIST_SQL =
        "SELECT id, name, tags FROM snippets ORDER BY updated_at DESC, id DESC;";

    sqlite3_stmt *stmt = NULL;
    char sql[SEARCH_SQL_CAPACITY];
    char *tokens[SEARCH_MAX_TOKENS];
    char *storage = NULL;
    int token_count = 0;
    const char *sql_to_use;
    int rc;
    int i;

    if (!db || !db->handle || !out) {
        return false;
    }

    snippet_list_free(out);

    if (!build_search_sql(search, sql, sizeof(sql), tokens, &token_count,
                          &storage, err, err_size)) {
        return false;
    }

    sql_to_use = token_count > 0 ? sql : LIST_SQL;
    if (!prepare_stmt(db, sql_to_use, &stmt,
                      token_count > 0 ? "could not prepare the search query"
                                      : "could not prepare the list query",
                      err, err_size)) {
        free(storage);
        return false;
    }

    for (i = 0; i < token_count; ++i) {
        if (sqlite3_bind_text(stmt, i + 1, tokens[i], -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
            report_error(db, err, err_size, "could not bind a search term",
                         sqlite3_errmsg(db->handle));
            free(storage);
            sqlite3_finalize(stmt);
            return false;
        }
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *tags = sqlite3_column_text(stmt, 2);

        if (!list_push(out, id,
                       name ? (const char *)name : "",
                       tags ? (const char *)tags : "", err, err_size)) {
            free(storage);
            sqlite3_finalize(stmt);
            snippet_list_free(out);
            return false;
        }
    }

    if (rc != SQLITE_DONE) {
        report_error(db, err, err_size,
                     token_count > 0 ? "search query failed"
                                     : "listing snippets failed",
                     sqlite3_errmsg(db->handle));
        free(storage);
        sqlite3_finalize(stmt);
        snippet_list_free(out);
        return false;
    }

    free(storage);
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        report_error(db, err, err_size, "could not finalize the query",
                     sqlite3_errmsg(db->handle));
        snippet_list_free(out);
        return false;
    }

    return true;
}

bool snippet_db_get(SnippetDB *db, sqlite3_int64 id, Snippet *out, char *err,
                    size_t err_size)
{
    static const char *SQL =
        "SELECT id, name, tags, description, body FROM snippets WHERE id = ?1;";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->id = id;

    if (!db || !db->handle) {
        return false;
    }

    if (!prepare_stmt(db, SQL, &stmt, "could not prepare the get query", err,
                      err_size)) {
        return false;
    }

    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        report_error(db, err, err_size, "could not bind the snippet id",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *tags = sqlite3_column_text(stmt, 2);
        const unsigned char *description = sqlite3_column_text(stmt, 3);
        const unsigned char *body = sqlite3_column_text(stmt, 4);

        out->name = dup_text(name);
        out->tags = dup_text(tags);
        out->description = dup_text(description);
        out->body = dup_text(body);
        if (!out->name || !out->tags || !out->description || !out->body) {
            report_error(NULL, err, err_size, "out of memory",
                         "could not copy the snippet");
            snippet_free(out);
            sqlite3_finalize(stmt);
            return false;
        }
        out->id = sqlite3_column_int64(stmt, 0);
        out->valid = true;
    } else if (rc == SQLITE_DONE) {
        report_error(NULL, err, err_size, "snippet not found",
                     "the selected row no longer exists");
    } else {
        report_error(db, err, err_size, "could not load the snippet",
                     sqlite3_errmsg(db->handle));
    }

    sqlite3_finalize(stmt);
    return out->valid;
}

bool snippet_db_insert(SnippetDB *db, const char *name, const char *tags,
                       const char *description, const char *body,
                       sqlite3_int64 *out_id, char *err, size_t err_size)
{
    static const char *SQL =
        "INSERT INTO snippets(name, tags, description, body) "
        "VALUES (?1, ?2, ?3, ?4);";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !db->handle || !name || !tags || !description || !body) {
        report_error(NULL, err, err_size, "invalid insert",
                     "a required string was NULL");
        return false;
    }

    if (!prepare_stmt(db, SQL, &stmt, "could not prepare the insert", err,
                      err_size)) {
        return false;
    }

    if (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, tags, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, description, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(stmt, 4, body, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        report_error(db, err, err_size, "could not bind the new snippet",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        report_error(db, err, err_size, "could not insert the snippet",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    if (out_id) {
        *out_id = sqlite3_last_insert_rowid(db->handle);
    }

    sqlite3_finalize(stmt);
    return true;
}

bool snippet_db_update(SnippetDB *db, sqlite3_int64 id, const char *name,
                       const char *tags, const char *description,
                       const char *body, char *err, size_t err_size)
{
    static const char *SQL =
        "UPDATE snippets SET name = ?1, tags = ?2, description = ?3, body = ?4, "
        "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE id = ?5;";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !db->handle || !name || !tags || !description || !body) {
        report_error(NULL, err, err_size, "invalid update",
                     "a required string was NULL");
        return false;
    }

    if (!prepare_stmt(db, SQL, &stmt, "could not prepare the update", err,
                      err_size)) {
        return false;
    }

    if (sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, tags, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, description, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(stmt, 4, body, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, id) != SQLITE_OK) {
        report_error(db, err, err_size, "could not bind the updated snippet",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        report_error(db, err, err_size, "could not update the snippet",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    if (sqlite3_changes(db->handle) == 0) {
        report_error(NULL, err, err_size, "snippet not found",
                     "the row to update no longer exists");
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool snippet_db_delete(SnippetDB *db, sqlite3_int64 id, char *err,
                       size_t err_size)
{
    static const char *SQL = "DELETE FROM snippets WHERE id = ?1;";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!db || !db->handle) {
        return false;
    }

    if (!prepare_stmt(db, SQL, &stmt, "could not prepare the delete", err,
                      err_size)) {
        return false;
    }

    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        report_error(db, err, err_size, "could not bind the snippet id",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        report_error(db, err, err_size, "could not delete the snippet",
                     sqlite3_errmsg(db->handle));
        sqlite3_finalize(stmt);
        return false;
    }

    if (sqlite3_changes(db->handle) == 0) {
        report_error(NULL, err, err_size, "snippet not found",
                     "the row to delete no longer exists");
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}
