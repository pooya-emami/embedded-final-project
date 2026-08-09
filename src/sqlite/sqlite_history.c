#include "sqlite_history.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static sqlite3 *db = NULL;

int history_db_init(const char *path)
{
    if (!path || strlen(path) == 0) {
        fprintf(stderr, "SQLite: No database path provided\n");
        return -1;
    }

    char *dir = strdup(path);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }
    free(dir);

    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite open failed: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);

    // Only one table for total count (persistent)
    const char *sql =
        "CREATE TABLE IF NOT EXISTS total_count ("
        " id INTEGER PRIMARY KEY CHECK (id = 1),"
        " total INTEGER NOT NULL DEFAULT 0,"
        " last_updated DATETIME DEFAULT CURRENT_TIMESTAMP"
        ");"
        "INSERT OR IGNORE INTO total_count (id, total) VALUES (1, 0);";

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQLite create table failed: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }

    printf("[SQLITE] Database initialized: %s\n", path);
    return 0;
}

int history_db_add(void)
{
    if (!db) {
        fprintf(stderr, "SQLite: Database not initialized\n");
        return -1;
    }

    // Only increment total count (grows forever)
    const char *update_total =
        "UPDATE total_count SET total = total + 1, last_updated = CURRENT_TIMESTAMP WHERE id = 1;";
    
    char *err = NULL;
    if (sqlite3_exec(db, update_total, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "SQLite update total failed: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    return 0;
}

long history_db_total(void)
{
    if (!db) {
        fprintf(stderr, "SQLite: Database not initialized\n");
        return -1;
    }

    const char *sql = "SELECT total FROM total_count WHERE id = 1;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    long total = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        total = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return total;
}

void history_db_close(void)
{
    if (db) {
        sqlite3_close(db);
        db = NULL;
        printf("[SQLITE] Database closed\n");
    }
}