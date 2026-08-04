#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "shared_frame.h"

static sem_t *g_lock = NULL;

shared_frame_t *shared_frame_open(void) {
    g_lock = sem_open(SEM_FRAME_NAME, O_CREAT, 0666, 1);
    if (g_lock == SEM_FAILED) {
        perror("sem_open");
        return NULL;
    }

    int fd = shm_open(SHM_FRAME_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (ftruncate(fd, sizeof(shared_frame_t)) < 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    shared_frame_t *sf = mmap(NULL, sizeof(shared_frame_t),
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* fd not needed once mapped */

    if (sf == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    return sf;
}

void shared_frame_write(shared_frame_t *sf, const unsigned char *data, size_t len) {
    if (len > SHM_FRAME_BUF_SIZE) len = SHM_FRAME_BUF_SIZE;

    sem_wait(g_lock);
    memcpy(sf->frame_buffer, data, len);
    sf->frame_size = len;
    sem_post(g_lock);
}

size_t shared_frame_read(shared_frame_t *sf, unsigned char *buf, size_t max) {
    size_t n;

    sem_wait(g_lock);
    n = sf->frame_size;
    if (n > max) n = max;
    if (n > 0) memcpy(buf, sf->frame_buffer, n);
    sem_post(g_lock);

    return n;
}
