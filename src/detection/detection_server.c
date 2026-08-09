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
#include "../common/shared_frame_processed.h"
#include "human_detector.hpp"

static shared_frame_t *g_frame = NULL;
static processed_frame_t *g_processed = NULL;
static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    printf("[DETECTION] Starting Detection Service...\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Open raw frame shared memory (from relay)
    g_frame = shared_frame_open();
    if (!g_frame) {
        fprintf(stderr, "[DETECTION] Failed to open shared frame\n");
        return 1;
    }
    printf("[DETECTION] Raw frame shared memory opened\n");

    g_processed = processed_frame_open();
    if (!g_processed) {
        fprintf(stderr, "[DETECTION] Failed to open processed shared memory\n");
        return 1;
    }
    printf("[DETECTION] Processed shared memory opened\n");

    // Temperature file
    FILE *temp_file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    float cpu_temp = -1;
    int frame_count = 0;
    
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

            DetectionResult res = process_frame(buf, len, 320, 240);
            
            if (res.jpeg_output && res.jpeg_length > 0) {
                processed_frame_write(g_processed, res.jpeg_output, res.jpeg_length, 
                                      res.person_count, cpu_temp);
            }
            
            free_detection_result(&res);
            
            frame_count++;
            if (frame_count % 30 == 0) {
                printf("[DETECTION] Frames: %d\n", frame_count);
            }
        }

        usleep(33000);  // ~30 FPS
    }

    printf("[DETECTION] Shutting down...\n");
    if (temp_file) fclose(temp_file);
    return 0;
}