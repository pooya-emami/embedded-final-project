#include "sqlite_history.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db = NULL;

int history_db_init(const char *path)
{
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "SQLite open failed: %s\n", sqlite3_errmsg(db));
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
        return -1;
    }

    return 0;
}

int history_db_add(int count, float temp)
{
    if (!db) {
        fprintf(stderr, "SQLite database not initialized\n");
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
    
    if (rc == SQLITE_DONE) {
        // Keep only last 50 records (circular buffer)
        const char *cleanup = 
            "DELETE FROM history WHERE id NOT IN "
            "(SELECT id FROM history ORDER BY id DESC LIMIT 50);";
        char *err = NULL;
        sqlite3_exec(db, cleanup, NULL, NULL, &err);
        if (err) {
            sqlite3_free(err);
        }
        return 0;
    }
    return -1;
}

int history_db_get_last(history_record_t *out, int max_records)
{
    if (!db) {
        fprintf(stderr, "SQLite database not initialized\n");
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
    }

    sqlite3_finalize(stmt);
    return idx;
}

long history_db_total(void)
{
    if (!db) {
        fprintf(stderr, "SQLite database not initialized\n");
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

void history_db_close(void)
{
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}