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
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    printf("[DETECTION] Starting Detection Service...\n");
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

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

    int frame_count = 0;
    
    int target_width = 320;
    int target_height = 240;
    int throttle_active = 0;
    float current_temp = -1;
    int last_fps_update = 0;
    int target_fps = 30;
    int sleep_us = 33000;
    
    while (running) {
        if (!g_frame || !g_frame->relay_enabled || g_frame->stream_mode != 2) {
            usleep(100000);
            continue;
        }

        unsigned char buf[SHM_FRAME_BUF_SIZE];
        size_t len = shared_frame_read(g_frame, buf, SHM_FRAME_BUF_SIZE);

        if (len > 0 && buf[0] == 0xFF && buf[1] == 0xD8) {
            if (g_processed) {
                processed_frame_t *pf = (processed_frame_t*)g_processed;
                
                current_temp = pf->current_temp;
                
                if (pf->thermal_throttle_active != throttle_active) {
                    throttle_active = pf->thermal_throttle_active;
                    target_width = pf->target_width;
                    target_height = pf->target_height;
                    
                    if (throttle_active) {
                        printf("[DETECTION] Thermal throttling: %dx%d (FPS: %d)\n", 
                               target_width, target_height, pf->target_fps);
                    } else {
                        printf("[DETECTION] Thermal restored: %dx%d (FPS: %d)\n", 
                               target_width, target_height, pf->target_fps);
                    }
                }
                
                if (pf->target_fps != last_fps_update && pf->target_fps > 0) {
                    last_fps_update = pf->target_fps;
                    target_fps = pf->target_fps;
                    if (target_fps > 0) {
                        sleep_us = 1000000 / target_fps;
                    } else {
                        sleep_us = 33000;
                    }
                    printf("[DETECTION] Target FPS updated: %d (sleep: %d us)\n", 
                           target_fps, sleep_us);
                }
            }

            DetectionResult res = process_frame(buf, len, target_width, target_height);
            
            if (res.jpeg_output && res.jpeg_length > 0) {
                processed_frame_write(g_processed, res.jpeg_output, res.jpeg_length, 
                                      res.person_count, current_temp);
            }
            
            free_detection_result(&res);
            
            frame_count++;
            if (frame_count % 60 == 0) {
                printf("[DETECTION] Frames: %d, Resolution: %dx%d, FPS: %d, Temp: %.1f C\n", 
                       frame_count, target_width, target_height, target_fps, current_temp);
            }
        }

        usleep(sleep_us);
    }

    printf("[DETECTION] Shutting down...\n");
    return 0;
}