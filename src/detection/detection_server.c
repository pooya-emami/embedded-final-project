#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <semaphore.h>
#include <errno.h>

#include "../common/shared_frame.h"
#include "human_detector.hpp"

#define SHM_DETECTION_NAME "/guard_detection_result"
#define SEM_DETECTION_NAME "/guard_detection_lock"

typedef struct {
    int person_count;
    float cpu_temp;
    time_t timestamp;
    unsigned char frame_buffer[SHM_FRAME_BUF_SIZE];
    size_t frame_size;
    int valid;
} detection_result_t;

static shared_frame_t *g_frame = NULL;
static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    printf("[DETECTION] Starting Detection Service...\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open shared frame
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "[DETECTION] Failed to open shared frame\n");
        return 1;
    }
    printf("[DETECTION] Shared frame opened\n");

    // Open shared memory for detection results
    sem_t *result_sem = sem_open(SEM_DETECTION_NAME, O_CREAT, 0666, 1);
    if (result_sem == SEM_FAILED) {
        perror("[DETECTION] sem_open");
        return 1;
    }

    int shm_fd = shm_open(SHM_DETECTION_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("[DETECTION] shm_open");
        return 1;
    }
    
    if (ftruncate(shm_fd, sizeof(detection_result_t)) < 0) {
        perror("[DETECTION] ftruncate");
        return 1;
    }

    detection_result_t *result = mmap(NULL, sizeof(detection_result_t),
                                      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (result == MAP_FAILED) {
        perror("[DETECTION] mmap");
        return 1;
    }

    // Temperature file
    FILE *temp_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    float cpu_temp = -1;
    
    while (running) {
        unsigned char buf[SHM_FRAME_BUF_SIZE];
        size_t len = shared_frame_read(g_frame, buf, SHM_FRAME_BUF_SIZE);

        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            // Read temperature
            if (temp_file) {
                rewind(temp_file);
                int t = 0;
                if (fscanf(temp_file, "%d", &t) == 1) {
                    cpu_temp = t / 1000.0f;
                }
            }

            // Run detection
            DetectionResult res = process_frame(buf, len, 320, 240);
            
            // Write result to shared memory
            sem_wait(result_sem);
            result->person_count = res.person_count;
            result->cpu_temp = cpu_temp;
            result->timestamp = time(NULL);
            result->valid = 1;
            
            if (res.jpeg_output && res.jpeg_length > 0) {
                size_t copy_len = res.jpeg_length;
                if (copy_len > SHM_FRAME_BUF_SIZE) copy_len = SHM_FRAME_BUF_SIZE;
                memcpy(result->frame_buffer, res.jpeg_output, copy_len);
                result->frame_size = copy_len;
            }
            sem_post(result_sem);
            
            free_detection_result(&res);
        }

        usleep(33000);  // ~30 FPS
    }

    printf("[DETECTION] Shutting down...\n");
    
    sem_close(result_sem);
    sem_unlink(SEM_DETECTION_NAME);
    shm_unlink(SHM_DETECTION_NAME);
    if (temp_file) fclose(temp_file);
    
    return 0;
}