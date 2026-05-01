#include "dvr_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

struct dvr_engine {
    dvr_config_t     config;
    dvr_state_t      state;
    volatile int     running;
    camera_ctx_t    *camera;
    display_ctx_t   *display;
    ring_buffer_t   *ring_buf;
    trigger_ctx_t   *trigger;
    int              target_present;
    time_t           save_trigger_time;
    int              save_pending;
    int              save_event;
};

static void on_trigger(const trigger_data_t *data, void *user_data);

dvr_engine_t *dvr_engine_create(const dvr_config_t *config)
{
    dvr_engine_t *eng = calloc(1, sizeof(dvr_engine_t));
    if (!eng) return NULL;

    memcpy(&eng->config, config, sizeof(dvr_config_t));
    eng->state   = DVR_STATE_IDLE;
    eng->running = 1;

    eng->camera = camera_open(config->camera_device, config->width,
                              config->height, config->fps);
    if (!eng->camera) {
        fprintf(stderr, "[DVR] Camera init failed\n");
        free(eng);
        return NULL;
    }

    eng->ring_buf = ring_buffer_create(config->buffer_seconds, config->fps,
                                       config->width, config->height,
                                       config->sd_card_path);
    if (!eng->ring_buf) {
        fprintf(stderr, "[DVR] Ring buffer init failed\n");
        camera_close(eng->camera);
        free(eng);
        return NULL;
    }

    if (config->enable_display && config->display_mode == DISPLAY_MODE_LCD) {
        eng->display = display_open(config->width, config->height);
        if (!eng->display) {
            printf("[DVR] LCD not available, running headless\n");
        }
    }

    eng->trigger = trigger_receiver_create("/tmp/dvr_trigger_pipe");
    if (!eng->trigger) {
        printf("[DVR] Trigger pipe not available, manual mode only\n");
    }

    printf("[DVR] Engine created, state=IDLE\n");
    return eng;
}

void dvr_engine_destroy(dvr_engine_t *eng)
{
    if (!eng) return;
    dvr_engine_stop(eng);
    if (eng->trigger) trigger_receiver_destroy(eng->trigger);
    if (eng->display) display_close(eng->display);
    if (eng->ring_buf) ring_buffer_destroy(eng->ring_buf);
    if (eng->camera) camera_close(eng->camera);
    free(eng);
}

static int save_clip_to_mp4(dvr_engine_t *eng, time_t start_time, time_t end_time,
                            const char *filename)
{
    ring_buffer_t *rb = eng->ring_buf;
    int max_frames = rb->capacity;
    off_t *offsets = malloc((size_t)max_frames * sizeof(off_t));
    int *timestamps = malloc((size_t)max_frames * sizeof(int));
    if (!offsets || !timestamps) {
        free(offsets);
        free(timestamps);
        return -1;
    }

    int frame_count = 0;
    pthread_mutex_lock(&rb->lock);
    for (int i = 0; i < rb->count; i++) {
        int pos = (rb->tail + i) % rb->capacity;
        if (rb->index[pos].timestamp >= start_time &&
            rb->index[pos].timestamp <= end_time) {
            offsets[frame_count]    = rb->index[pos].offset;
            timestamps[frame_count] = (int)rb->index[pos].timestamp;
            frame_count++;
        }
    }
    char filepath[256];
    strncpy(filepath, rb->filepath, sizeof(filepath) - 1);
    filepath[sizeof(filepath) - 1] = '\0';
    int frame_size = rb->frame_size;
    pthread_mutex_unlock(&rb->lock);

    if (frame_count == 0) {
        printf("[DVR] No frames in range [%ld, %ld]\n", (long)start_time, (long)end_time);
        free(offsets);
        free(timestamps);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[DVR] fork failed: %s\n", strerror(errno));
        free(offsets);
        free(timestamps);
        return -1;
    }

    if (pid == 0) {
        int fd = open(filepath, O_RDONLY);
        if (fd < 0) _exit(1);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r %d -i pipe:0 "
                 "-c:v mpeg4 -q:v 5 -pix_fmt yuv420p %s 2>/dev/null",
                 eng->config.width, eng->config.height, eng->config.fps, filename);

        FILE *ffmpeg = popen(cmd, "w");
        if (!ffmpeg) { close(fd); _exit(1); }

        uint8_t *buf = malloc((size_t)frame_size);
        if (!buf) { close(fd); pclose(ffmpeg); _exit(1); }

        int sent = 0;
        for (int i = 0; i < frame_count; i++) {
            ssize_t n = pread(fd, buf, (size_t)frame_size, offsets[i]);
            if (n <= 0) continue;
            size_t written = fwrite(buf, 1, (size_t)n, ffmpeg);
            if (written != (size_t)n) break;
            sent++;
        }

        free(buf);
        close(fd);
        int ret = pclose(ffmpeg);

        if (ret != 0) {
            fprintf(stderr, "[DVR] ffmpeg failed with code %d\n", ret);
        } else {
            printf("[DVR] Saved %d frames: %s\n", sent, filename);
        }
        _exit(0);
    }

    free(offsets);
    free(timestamps);
    printf("[DVR] Async encoding started (pid=%d): %s, %d frames\n", pid, filename, frame_count);
    return 0;
}

static void on_trigger(const trigger_data_t *data, void *user_data)
{
    dvr_engine_t *eng = (dvr_engine_t *)user_data;

    switch (data->event) {

    case TRIGGER_TARGET_ON:
        if (eng->state == DVR_STATE_IDLE) {
            eng->state = DVR_STATE_BUFFERING;
            eng->target_present = 1;
            printf("[DVR] >>> STATE: IDLE -> BUFFERING (target detected)\n");
        }
        break;

    case TRIGGER_TARGET_OFF:
        if (eng->state == DVR_STATE_BUFFERING) {
            eng->state = DVR_STATE_IDLE;
            eng->target_present = 0;
            ring_buffer_clear(eng->ring_buf);
            printf("[DVR] >>> STATE: BUFFERING -> IDLE (target lost)\n");
        }
        break;

    case TRIGGER_WARNING:
    case TRIGGER_FALL:
    case TRIGGER_COLLISION:
        if (eng->state == DVR_STATE_BUFFERING || eng->state == DVR_STATE_IDLE) {
            const char *evt_name = data->event == TRIGGER_WARNING ? "WARNING" :
                                   data->event == TRIGGER_FALL ? "FALL" : "COLLISION";
            printf("[DVR] >>> EMERGENCY triggered: %s, will save after %ds buffer\n",
                   evt_name, eng->config.save_after_seconds);

            eng->save_pending      = 1;
            eng->save_event        = data->event;
            eng->save_trigger_time = data->timestamp;

            if (eng->state == DVR_STATE_IDLE) {
                eng->state = DVR_STATE_BUFFERING;
            }
        }
        break;
    }
}

int dvr_engine_run(dvr_engine_t *eng)
{
    if (!eng) return -1;

    uint8_t *frame_buf = malloc((size_t)(eng->config.width * eng->config.height * 3));
    if (!frame_buf) return -1;

    printf("[DVR] Main loop started\n");

    while (eng->running) {
        fd_set fds;
        FD_ZERO(&fds);

        int cam_fd = camera_get_fd(eng->camera);
        FD_SET(cam_fd, &fds);
        int max_fd = cam_fd;

        int trig_fd = -1;
        if (eng->trigger) {
            trig_fd = trigger_receiver_get_fd(eng->trigger);
            FD_SET(trig_fd, &fds);
            if (trig_fd > max_fd) max_fd = trig_fd;
        }

        struct timeval tv = {0, 100000};
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        while (waitpid(-1, NULL, WNOHANG) > 0);

        if (FD_ISSET(cam_fd, &fds)) {
            time_t ts;
            int size = camera_grab_frame(eng->camera, frame_buf,
                                         eng->config.width * eng->config.height * 3, &ts);
            if (size > 0) {
                if (eng->state == DVR_STATE_BUFFERING) {
                    ring_buffer_push(eng->ring_buf, frame_buf, size, ts);
                }

                if (eng->display) {
                    display_show_frame(eng->display, frame_buf,
                                       eng->config.width, eng->config.height);
                }
            }
        }

        if (trig_fd >= 0 && FD_ISSET(trig_fd, &fds)) {
            trigger_receiver_process(eng->trigger, on_trigger, eng);
        }

        if (eng->save_pending) {
            time_t now = time(NULL);
            time_t elapsed = now - eng->save_trigger_time;

            if (elapsed >= eng->config.save_after_seconds) {
                eng->save_pending = 0;

                const char *evt_name = eng->save_event == TRIGGER_WARNING ? "WARNING" :
                                       eng->save_event == TRIGGER_FALL ? "FALL" : "COLLISION";

                time_t start = eng->save_trigger_time - eng->config.save_before_seconds;
                time_t end   = eng->save_trigger_time + eng->config.save_after_seconds;

                char filename[DVR_MAX_PATH + 64];
                snprintf(filename, sizeof(filename),
                         "%s/emergency_%ld_%s.mp4",
                         eng->config.sd_card_path,
                         (long)eng->save_trigger_time, evt_name);

                printf("[DVR] Saving clip: [%ld, %ld] -> %s\n",
                       (long)start, (long)end, filename);

                save_clip_to_mp4(eng, start, end, filename);

                if (!eng->target_present) {
                    eng->state = DVR_STATE_IDLE;
                    ring_buffer_clear(eng->ring_buf);
                    printf("[DVR] >>> STATE: -> IDLE (no target)\n");
                }
            }
        }
    }

    free(frame_buf);
    printf("[DVR] Main loop exited\n");
    return 0;
}

void dvr_engine_stop(dvr_engine_t *eng)
{
    if (!eng) return;
    eng->running = 0;
}
