#ifndef SQLITE_HISTORY_H
#define SQLITE_HISTORY_H

#include <time.h>

typedef struct {
    int count;
    float temp;
    time_t timestamp;
} history_record_t;

int history_db_init(const char *path);
void history_db_set_max_records(int limit);
int history_db_add(int count, float temp);
int history_db_get_last(history_record_t *out, int max_records);
long history_db_total(void);
void history_db_close(void);

#endif