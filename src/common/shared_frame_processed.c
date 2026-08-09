#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "shared_frame_processed.h"

static sem_t *g_lock = NULL;

processed_frame_t *processed_frame_open(void) {
    g_lock = sem_open(SEM_PROCESSED_NAME, O_CREAT, 0666, 1);
    if (g_lock == SEM_FAILED) {
        perror("sem_open processed");
        return NULL;
    }

    int fd = shm_open(SHM_PROCESSED_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open processed");
        return NULL;
    }

    if (ftruncate(fd, sizeof(processed_frame_t)) < 0) {
        perror("ftruncate processed");
        close(fd);
        return NULL;
    }

    processed_frame_t *pf = mmap(NULL, sizeof(processed_frame_t),
                                  PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (pf == MAP_FAILED) {
        perror("mmap processed");
        return NULL;
    }

    return pf;
}

void processed_frame_write(processed_frame_t *pf, const unsigned char *data, size_t len, int count, float temp) {
    if (len > SHM_PROCESSED_BUF_SIZE) len = SHM_PROCESSED_BUF_SIZE;

    sem_wait(g_lock);
    memcpy(pf->frame_buffer, data, len);
    pf->frame_size = len;
    pf->person_count = count;
    pf->cpu_temp = temp;
    pf->timestamp = time(NULL);
    sem_post(g_lock);
}

size_t processed_frame_read(processed_frame_t *pf, unsigned char *buf, size_t max, int *count, float *temp) {
    size_t n;

    sem_wait(g_lock);
    n = pf->frame_size;
    if (n > max) n = max;
    if (n > 0) memcpy(buf, pf->frame_buffer, n);
    *count = pf->person_count;
    *temp = pf->cpu_temp;
    sem_post(g_lock);

    return n;
}