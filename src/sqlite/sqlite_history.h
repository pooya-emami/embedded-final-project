#ifndef SQLITE_HISTORY_H
#define SQLITE_HISTORY_H

#include <time.h>

typedef struct {
    int count;
    float temp;
    time_t timestamp;
} history_record_t;

int history_db_init(const char *path);
int history_db_add(int count, float temp);
long history_db_total(void);
int history_db_get_last(history_record_t *out, int max_records);
void history_db_close(void);

#endif