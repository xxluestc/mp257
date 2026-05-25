#include "dvr_engine.h"
#include "rpmsg_channel.h"
#include "camera_v4l2.h"
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
#include <dirent.h>

struct dvr_engine {
    dvr_config_t     config;
    dvr_state_t      state;
    volatile int     running;
    camera_ctx_t    *camera;
    display_ctx_t   *display;
    ring_buffer_t   *ring_buf;
    trigger_ctx_t   *trigger;
    rpmsg_ctx_t     *rpmsg;
    clip_manager_t   clips;
    int              target_present;
    time_t           buffer_start_time;
    time_t           last_cycle_time;
    int              save_pending;
    int              save_event;
    time_t           save_trigger_time;
    volatile int     sd_card_ok;
    volatile pid_t   encoder_pid;
    volatile int     encoder_running;
};

static void on_trigger(const trigger_data_t *data, void *user_data);

static int check_sd_card(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return access(path, W_OK) == 0;
}

clip_manager_t *clip_manager_create(const char *sd_path)
{
    clip_manager_t *cm = calloc(1, sizeof(clip_manager_t));
    if (!cm) return NULL;
    cm->sd_path = sd_path;
    cm->count = 0;
    cm->next_index = 0;
    memset(cm->clips, 0, sizeof(cm->clips));
    return cm;
}

void clip_manager_destroy(clip_manager_t *cm)
{
    if (!cm) return;
    free(cm);
}

const char *clip_type_name(clip_type_t t)
{
    switch (t) {
    case CLIP_TYPE_WARNING:  return "WARNING";
    case CLIP_TYPE_FALL:     return "FALL";
    case CLIP_TYPE_COLLISION: return "COLLISION";
    default: return "UNKNOWN";
    }
}

int clip_is_protected(clip_type_t t)
{
    return t == CLIP_TYPE_FALL || t == CLIP_TYPE_COLLISION;
}

int clip_manager_add(clip_manager_t *cm, const char *filename, clip_type_t type)
{
    if (!cm) return -1;

    if (!clip_is_protected(type)) {
        if (cm->clips[cm->next_index].filename[0]) {
            unlink(cm->clips[cm->next_index].filename);
            printf("[CLIP] Overwritten old: %s\n", cm->clips[cm->next_index].filename);
        }
        strncpy(cm->clips[cm->next_index].filename, filename, DVR_MAX_PATH - 1);
        cm->clips[cm->next_index].filename[DVR_MAX_PATH - 1] = '\0';
        cm->clips[cm->next_index].type       = type;
        cm->clips[cm->next_index].save_time  = time(NULL);
        cm->clips[cm->next_index].protected_ = 0;
        printf("[CLIP] Saved normal [%d/%d]: %s (%s)\n",
               cm->next_index + 1, DVR_MAX_NORMAL_CLIPS, filename, clip_type_name(type));
        cm->next_index = (cm->next_index + 1) % DVR_MAX_NORMAL_CLIPS;
        if (cm->count < DVR_MAX_NORMAL_CLIPS) cm->count++;
    } else {
        printf("[CLIP] Saved PROTECTED: %s (%s) - will NOT be auto-overwritten\n",
               filename, clip_type_name(type));
    }
    return 0;
}

void clip_manager_list(clip_manager_t *cm)
{
    if (!cm) return;
    printf("[CLIP] === Clip list ===\n");
    for (int i = 0; i < DVR_MAX_NORMAL_CLIPS; i++) {
        if (cm->clips[i].filename[0]) {
            printf("  [%d] %s (%s)%s\n", i,
                   cm->clips[i].filename,
                   clip_type_name(cm->clips[i].type),
                   cm->clips[i].protected_ ? " [PROTECTED]" : "");
        }
    }
}

dvr_engine_t *dvr_engine_create(const dvr_config_t *config)
{
    dvr_engine_t *eng = calloc(1, sizeof(dvr_engine_t));
    if (!eng) return NULL;

    memcpy(&eng->config, config, sizeof(dvr_config_t));
    eng->state      = DVR_STATE_IDLE;
    eng->running    = 1;
    eng->sd_card_ok = 1;

    eng->camera = camera_open(config->camera_device, config->width,
                              config->height, config->fps);
    if (!eng->camera) {
        fprintf(stderr, "[DVR] Camera init failed\n");
        free(eng);
        return NULL;
    }

    eng->ring_buf = ring_buffer_create(config->buffer_seconds, config->fps,
                                       camera_get_width(eng->camera),
                                       camera_get_height(eng->camera),
                                       config->sd_card_path,
                                       config->width * config->height * 2);
    if (!eng->ring_buf) {
        fprintf(stderr, "[DVR] Ring buffer init failed\n");
        camera_close(eng->camera);
        free(eng);
        return NULL;
    }

    if (config->enable_display && config->display_mode == DISPLAY_MODE_LCD) {
        eng->display = display_open(config->width, config->height);
        if (eng->display) {
            display_set_source_format(eng->display,
                                      camera_get_pixelformat(eng->camera),
                                      camera_get_bpp(eng->camera));
        } else {
            printf("[DVR] LCD not available, running headless\n");
        }
    }

    eng->trigger = trigger_receiver_create("/tmp/dvr_trigger_pipe");
    if (!eng->trigger) {
        printf("[DVR] Trigger pipe not available, manual mode only\n");
    }

    eng->rpmsg = rpmsg_channel_open("/dev/ttyRPMSG0");
    if (!eng->rpmsg) {
        eng->rpmsg = rpmsg_channel_open("/dev/ttyRPMSG1");
    }
    if (!eng->rpmsg) {
        printf("[DVR] RPMSG channel not available (will retry every 5s)\n");
    }

    eng->clips = *(clip_manager_create(config->sd_card_path));

    printf("[DVR] Engine created, state=IDLE, max_normal_clips=%d\n", DVR_MAX_NORMAL_CLIPS);
    return eng;
}

void dvr_engine_destroy(dvr_engine_t *eng)
{
    if (!eng) return;
    dvr_engine_stop(eng);
    if (eng->trigger) trigger_receiver_destroy(eng->trigger);
    if (eng->rpmsg) rpmsg_channel_close(eng->rpmsg);
    if (eng->display) display_close(eng->display);
    if (eng->ring_buf) ring_buffer_destroy(eng->ring_buf);
    if (eng->camera) camera_close(eng->camera);
    clip_manager_destroy(&eng->clips);
    free(eng);
}

typedef struct {
    off_t   offset;
    int64_t timestamp_us;
} frame_info_t;

static int frame_cmp_by_ts(const void *a, const void *b)
{
    int64_t diff = ((const frame_info_t *)a)->timestamp_us - ((const frame_info_t *)b)->timestamp_us;
    return (diff > 0) - (diff < 0);
}

static int save_clip_to_mp4(dvr_engine_t *eng, time_t start_time, time_t end_time,
                            const char *filename, clip_type_t ctype)
{
    ring_buffer_t *rb = eng->ring_buf;
    int max_frames = rb->capacity;
    frame_info_t *frames = malloc((size_t)max_frames * sizeof(frame_info_t));
    if (!frames) return -1;

    int64_t start_us = (int64_t)start_time * 1000000;
    int64_t end_us   = (int64_t)end_time * 1000000 + 999999;

    pthread_mutex_lock(&rb->lock);
    int frame_count = 0;
    for (int i = 0; i < rb->count; i++) {
        int pos = (rb->tail + i) % rb->capacity;
        if (rb->index[pos].timestamp >= start_us &&
            rb->index[pos].timestamp <= end_us) {
            frames[frame_count].offset       = rb->index[pos].offset;
            frames[frame_count].timestamp_us = rb->index[pos].timestamp;
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
        free(frames);
        return 0;
    }

    double window_sec = difftime(end_time, start_time);
    if (window_sec <= 0.0) window_sec = 30.0;
    int fps_for_ffmpeg = (int)((double)frame_count / window_sec + 0.5);
    if (fps_for_ffmpeg < 1) fps_for_ffmpeg = 1;
    if (fps_for_ffmpeg > 60) fps_for_ffmpeg = 60;

    pthread_mutex_lock(&rb->lock);
    rb->paused = 1;
    pthread_cond_signal(&rb->write_cond);
    int drain_cycles = 0;
    while (rb->pending_count > 0 && drain_cycles < 50) {
        pthread_mutex_unlock(&rb->lock);
        usleep(2000);
        pthread_mutex_lock(&rb->lock);
        drain_cycles++;
    }
    pthread_mutex_unlock(&rb->lock);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[DVR] fork failed: %s\n", strerror(errno));
        free(frames);
        return -1;
    }

    if (pid == 0) {
        usleep(50000);
        fsync(rb->fd);

        qsort(frames, (size_t)frame_count, sizeof(frame_info_t), frame_cmp_by_ts);

        int64_t first_ts = frames[0].timestamp_us;
        int64_t last_ts  = frames[frame_count - 1].timestamp_us;
        double actual_dur = (double)(last_ts - first_ts) / 1000000.0;

        printf("[DVR] Frames=%d, window=%.1fs, actual=%.2fs, fps=%d\n",
               frame_count, window_sec, actual_dur, fps_for_ffmpeg);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -f rawvideo -pix_fmt rgb565 -s %dx%d -r %d -i pipe:0 "
                 "-vsync cfr -c:v mpeg4 -q:v 5 -pix_fmt yuv420p %s 2>/dev/null",
                 eng->config.width, eng->config.height, fps_for_ffmpeg, filename);

        int fd = open(filepath, O_RDONLY);
        FILE *ffmpeg_pipe = popen(cmd, "w");
        if (fd < 0 || !ffmpeg_pipe) {
            fprintf(stderr, "[DVR] Child: open(%s)=%d popen=%p (%s)\n",
                    filepath, fd, (void*)ffmpeg_pipe, fd < 0 ? strerror(errno) : "popen failed");
            if (fd >= 0) close(fd);
            if (ffmpeg_pipe) pclose(ffmpeg_pipe);
            free(frames);
            exit(1);
        }

        uint8_t *buf = malloc((size_t)frame_size);
        if (!buf) { close(fd); pclose(ffmpeg_pipe); free(frames); exit(1); }

        int sent = 0;
        for (int i = 0; i < frame_count; i++) {
            ssize_t n = pread(fd, buf, (size_t)frame_size, frames[i].offset);
            if (n <= 0) continue;
            size_t written = fwrite(buf, 1, (size_t)n, ffmpeg_pipe);
            if (written != (size_t)n) break;
            sent++;
        }

        free(buf);
        free(frames);
        close(fd);
        int ret = pclose(ffmpeg_pipe);

        if (ret != 0) {
            fprintf(stderr, "[DVR] ffmpeg failed with code %d\n", ret);
        } else {
            printf("[DVR] Saved %d frames: %s\n", sent, filename);
        }

        exit(ret != 0 ? 1 : 0);
    }

    pthread_mutex_lock(&rb->lock);
    rb->paused = 1;
    pthread_cond_signal(&rb->write_cond);
    pthread_mutex_unlock(&rb->lock);

    eng->encoder_pid = pid;
    eng->encoder_running = 1;

    free(frames);

    clip_manager_add(&eng->clips, filename, ctype);

    printf("[DVR] Async encoding started (pid=%d): %s, %d frames, type=%s%s\n",
           pid, filename, frame_count, clip_type_name(ctype),
           clip_is_protected(ctype) ? " [PROTECTED]" : "");
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
            eng->buffer_start_time = time(NULL);
            eng->last_cycle_time  = time(NULL);
            printf("[DVR] >>> STATE: IDLE -> BUFFERING (target detected, circular recording)\n");
        }
        break;

    case TRIGGER_TARGET_OFF:
        if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
            eng->state = DVR_STATE_IDLE;
            eng->target_present = 0;
            ring_buffer_clear(eng->ring_buf);
            printf("[DVR] >>> STATE: BUFFERING -> IDLE (target lost, stopped)\n");
        } else if (eng->state == DVR_STATE_BUFFERING && eng->save_pending) {
            eng->target_present = 0;
            printf("[DVR] Target off but save pending, keeping buffer\n");
        }
        break;

    case TRIGGER_WARNING:
    case TRIGGER_FALL:
    case TRIGGER_COLLISION:
        if (eng->state == DVR_STATE_BUFFERING || eng->state == DVR_STATE_IDLE) {
            eng->save_pending      = 1;
            eng->save_event        = data->event;
            eng->save_trigger_time = data->timestamp;

            clip_type_t ctype = (clip_type_t)(eng->save_event - TRIGGER_WARNING);
            const char *evt_name = clip_type_name(ctype);

            if (eng->state == DVR_STATE_IDLE) {
                eng->state = DVR_STATE_BUFFERING;
                eng->buffer_start_time = data->timestamp;
                eng->last_cycle_time  = data->timestamp;
            }

            printf("[DVR] >>> EMERGENCY triggered: %s (protected=%d), buffering...\n",
                   evt_name, clip_is_protected(ctype));
        }
        break;
    }
}

int dvr_engine_run(dvr_engine_t *eng)
{
    if (!eng) return -1;

    int frame_size = camera_get_frame_size(eng->camera);
    if (frame_size <= 0) frame_size = eng->config.width * eng->config.height * 2;
    uint8_t *frame_buf = malloc((size_t)frame_size);
    if (!frame_buf) { return -1; }

    printf("[DVR] Main loop started\n");
    printf("[DVR] Rules:\n");
    printf("[DVR]   - TARGET_ON: start circular 30s buffer (no SD save)\n");
    printf("[DVR]   - Every 30s: cycle buffer if target present + no emergency\n");
    printf("[DVR]   - WARNING: save 30s clip (before+after 15s), max %d clips (overwrite oldest)\n", DVR_MAX_NORMAL_CLIPS);
    printf("[DVR]   - FALL/COLLISION: save 30s clip (PROTECTED, never overwritten)\n");
    printf("[DVR]   - TARGET_OFF: stop recording\n");

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

        int rpmsg_fd = -1;
        if (eng->rpmsg) {
            rpmsg_fd = rpmsg_channel_get_fd(eng->rpmsg);
            FD_SET(rpmsg_fd, &fds);
            if (rpmsg_fd > max_fd) max_fd = rpmsg_fd;
        }

        struct timeval tv = {0, 100000};
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        {
            int status;
            pid_t reaped;
            while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
                if (eng->encoder_running && reaped == eng->encoder_pid) {
                    eng->encoder_running = 0;
                    pthread_mutex_lock(&eng->ring_buf->lock);
                    eng->ring_buf->paused = 0;
                    pthread_cond_signal(&eng->ring_buf->write_cond);
                    pthread_mutex_unlock(&eng->ring_buf->lock);
                    printf("[DVR] Encoder finished, write thread resumed\n");
                }
            }
        }

        if (FD_ISSET(cam_fd, &fds)) {
            int64_t ts;
            int size = camera_grab_frame(eng->camera, frame_buf, frame_size, &ts);
            if (size > 0) {
                if (eng->display) {
                    display_show_frame(eng->display, frame_buf,
                                       eng->config.width, eng->config.height);
                }

                if (eng->state == DVR_STATE_BUFFERING) {
                    if (eng->sd_card_ok) {
                        ring_buffer_push(eng->ring_buf, frame_buf, size, ts);
                    }
                }
            }
        }

        if (trig_fd >= 0 && FD_ISSET(trig_fd, &fds)) {
            trigger_receiver_process(eng->trigger, on_trigger, eng);
        }

        if (rpmsg_fd >= 0 && FD_ISSET(rpmsg_fd, &fds)) {
            int ret = rpmsg_channel_process(eng->rpmsg, on_trigger, eng);
            if (ret < 0) {
                printf("[DVR] RPMSG connection lost, will reconnect...\n");
                rpmsg_channel_close(eng->rpmsg);
                eng->rpmsg = NULL;
            }
        }

        eng->sd_card_ok = check_sd_card(eng->config.sd_card_path);
        if (!eng->sd_card_ok && eng->state == DVR_STATE_BUFFERING) {
            printf("[DVR] WARNING: SD card removed! Buffering continues but cannot save.\n");
        }

        {
            static time_t last_rpmsg_retry = 0;
            time_t now = time(NULL);
            if (!eng->rpmsg && (now - last_rpmsg_retry >= 5)) {
                last_rpmsg_retry = now;
                eng->rpmsg = rpmsg_channel_open("/dev/ttyRPMSG0");
                if (!eng->rpmsg) eng->rpmsg = rpmsg_channel_open("/dev/ttyRPMSG1");
                if (eng->rpmsg) printf("[DVR] RPMSG channel connected! (fd=%d)\n", rpmsg_channel_get_fd(eng->rpmsg));
            }
        }

        if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
            time_t now = time(NULL);
            time_t elapsed = now - eng->last_cycle_time;

            if (elapsed >= eng->config.buffer_seconds) {
                printf("[DVR] Circular 30s cycle complete, rotating buffer (target still present)\n");
                ring_buffer_clear(eng->ring_buf);
                eng->buffer_start_time = now;
                eng->last_cycle_time  = now;
            }
        }

        /* save_pending期间禁止循环清空，避免丢失触发前的帧数据 */

        if (eng->save_pending) {
            time_t now = time(NULL);

            int available_before = (int)(eng->save_trigger_time - eng->buffer_start_time);
            if (available_before < 0) available_before = 0;
            if (available_before > eng->config.save_before_seconds)
                available_before = eng->config.save_before_seconds;

            int needed_after = eng->config.save_before_seconds + eng->config.save_after_seconds - available_before;
            if (needed_after < eng->config.save_after_seconds)
                needed_after = eng->config.save_after_seconds;

            time_t elapsed_save = now - eng->save_trigger_time;

            if (elapsed_save >= needed_after) {
                eng->save_pending = 0;

                clip_type_t ctype = (clip_type_t)(eng->save_event - TRIGGER_WARNING);
                const char *evt_name = clip_type_name(ctype);

                time_t start = eng->save_trigger_time - available_before;
                time_t end   = eng->save_trigger_time + needed_after;

                struct tm *bt = localtime(&eng->save_trigger_time);
                char filename[DVR_MAX_PATH + 64];
                snprintf(filename, sizeof(filename),
                         "%s/emergency_%04d%02d%02d_%02d%02d%02d_%s.mp4",
                         eng->config.sd_card_path,
                         bt->tm_year + 1900, bt->tm_mon + 1, bt->tm_mday,
                         bt->tm_hour, bt->tm_min, bt->tm_sec,
                         evt_name);

                printf("[DVR] Saving clip: [%ld, %ld] -> %s (type=%s, protected=%d)\n",
                       (long)start, (long)end, filename, evt_name, clip_is_protected(ctype));

                if (eng->sd_card_ok) {
                    save_clip_to_mp4(eng, start, end, filename, ctype);
                } else {
                    printf("[DVR] SD card not available, clip SKIPPED: %s\n", filename);
                }

                if (!eng->target_present) {
                    eng->state = DVR_STATE_IDLE;
                    ring_buffer_clear(eng->ring_buf);
                    printf("[DVR] >>> STATE: -> IDLE (no target, save done)\n");
                } else {
                    ring_buffer_clear(eng->ring_buf);
                    eng->buffer_start_time = time(NULL);
                    eng->last_cycle_time  = time(NULL);
                    printf("[DVR] >>> Save done, target still present -> continuing BUFFERING\n");
                }

                clip_manager_list(&eng->clips);
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
