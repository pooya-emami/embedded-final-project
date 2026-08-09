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

    const char *sql =
        "CREATE TABLE IF NOT EXISTS history ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " count INTEGER NOT NULL,"
        " temp REAL NOT NULL,"
        " timestamp INTEGER NOT NULL"
        ");";

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

int history_db_add(int count, float temp)
{
    if (!db) {
        fprintf(stderr, "SQLite: Database not initialized\n");
        return -1;
    }

    const char *sql =
        "INSERT INTO history (count, temp, timestamp) VALUES (?, ?, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, count);
    sqlite3_bind_double(stmt, 2, temp);
    sqlite3_bind_int(stmt, 3, (int)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "SQLite insert failed: %s\n", sqlite3_errmsg(db));
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

    const char *sql = "SELECT COUNT(*) FROM history;";
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

int history_db_get_last(history_record_t *out, int max_records)
{
    if (!db) {
        fprintf(stderr, "SQLite: Database not initialized\n");
        return -1;
    }

    const char *sql =
        "SELECT count, temp, timestamp "
        "FROM history ORDER BY id DESC LIMIT ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_records);

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out[idx].count = sqlite3_column_int(stmt, 0);
        out[idx].temp = (float)sqlite3_column_double(stmt, 1);
        out[idx].timestamp = sqlite3_column_int(stmt, 2);
        idx++;
        if (idx >= max_records) break;
    }

    sqlite3_finalize(stmt);
    return idx;
}

void history_db_close(void)
{
    if (db) {
        sqlite3_close(db);
        db = NULL;
        printf("[SQLITE] Database closed\n");
    }
}