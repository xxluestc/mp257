/**
 * dvr_engine.c - DVR 行车记录引擎
 *
 * 状态机: IDLE → BUFFERING → SAVING
 * 触发方式: 命名管道 /tmp/dvr_trigger_pipe
 * 编码: fork() + ffmpeg 异步编码为 MP4
 */

#include "dvr_engine.h"
#include "usb_camera.h"
#include "ring_buffer.h"
#include "trigger_receiver.h"
#include "frame_decoder.h"
#include "npu_fusion.h"
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
#include <linux/videodev2.h>

/* NPU验证状态 */
enum {
    NPU_IDLE      = 0,
    NPU_VERIFYING = 1,
    NPU_CONFIRMED = 2,
    NPU_DENIED    = 3,
};

struct dvr_engine {
    dvr_config_t     config;
    dvr_state_t      state;
    volatile int     running;
    usb_camera_t    *camera;
    ring_buffer_t   *ring_buf;
    trigger_ctx_t   *trigger;
    int              target_present;
    time_t           buffer_start_time;
    time_t           last_cycle_time;
    int              save_pending;
    int              save_event;
    time_t           save_trigger_time;
    volatile int     storage_ok;
    volatile pid_t   encoder_pid;
    volatile int     encoder_running;
    volatile int     need_clear_after_encode; /* 编码完成后清除缓冲区 */
    int              pix_fmt;        /* 摄像头像素格式 */

    /* 自动触发模式 */
    time_t           engine_start_time;
    int              auto_target_sent;
    int              auto_collision_sent;
    int              auto_save_complete;  /* 自动模式下保存完成 */

    /* NPU融合验证 */
    npu_fusion_ctx_t *npu;
    int               npu_verify_state;   /* NPU_IDLE/VERIFYING/CONFIRMED/DENIED */
    int               npu_confirm_count;  /* 连续确认次数 */
    int               npu_deny_count;     /* 连续否认次数 */
    int               npu_frame_skip;     /* 每N帧跑一次NPU (减少NPU负载) */
    uint8_t          *npu_rgb_buf;        /* RGB解码缓冲区 */
    int               npu_rgb_buf_size;
};

static void on_trigger(const trigger_data_t *data, void *user_data);

static int check_storage(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return access(path, W_OK) == 0;
}

static const char *clip_type_name(clip_type_t t)
{
    switch (t) {
    case CLIP_TYPE_WARNING:   return "WARNING";
    case CLIP_TYPE_FALL:      return "FALL";
    case CLIP_TYPE_COLLISION: return "COLLISION";
    default: return "UNKNOWN";
    }
}

static int clip_is_protected(clip_type_t t)
{
    return t == CLIP_TYPE_FALL || t == CLIP_TYPE_COLLISION;
}

/* 按时间戳排序帧 */
typedef struct {
    off_t   offset;
    int64_t timestamp_us;
    int     frame_size;
} frame_info_t;

static int frame_cmp_by_ts(const void *a, const void *b)
{
    int64_t diff = ((const frame_info_t *)a)->timestamp_us -
                   ((const frame_info_t *)b)->timestamp_us;
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

    /* 收集时间范围内的帧 */
    pthread_mutex_lock(&rb->lock);
    int frame_count = 0;
    for (int i = 0; i < rb->count; i++) {
        int pos = (rb->tail + i) % rb->capacity;
        if (rb->index[pos].timestamp_us >= start_us &&
            rb->index[pos].timestamp_us <= end_us) {
            frames[frame_count].offset       = rb->index[pos].file_offset;
            frames[frame_count].timestamp_us = rb->index[pos].timestamp_us;
            frames[frame_count].frame_size   = rb->index[pos].frame_size;
            frame_count++;
        }
    }
    char filepath[256];
    strncpy(filepath, rb->filepath, sizeof(filepath) - 1);
    filepath[sizeof(filepath) - 1] = '\0';
    int max_frame_size = rb->max_pending_size;
    pthread_mutex_unlock(&rb->lock);

    if (frame_count == 0) {
        printf("[DVR] No frames in range [%ld, %ld]\n", (long)start_time, (long)end_time);
        free(frames);
        return 0;
    }

    /* 窗口时间跨度(仅用于日志和暂停写线程) */
    double window_sec = difftime(end_time, start_time);
    if (window_sec <= 0.0) window_sec = 30.0;

    /* 暂停写线程, drain pending队列 */
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

    /* fork子进程进行编码 */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[DVR] fork failed: %s\n", strerror(errno));
        free(frames);
        return -1;
    }

    if (pid == 0) {
        /* 子进程 */
        usleep(50000);
        fsync(rb->fd);

        /* 按时间戳排序 */
        qsort(frames, (size_t)frame_count, sizeof(frame_info_t), frame_cmp_by_ts);

        int64_t first_ts = frames[0].timestamp_us;
        int64_t last_ts  = frames[frame_count - 1].timestamp_us;
        double actual_dur = (double)(last_ts - first_ts) / 1000000.0;

        /* 基于实际帧时间跨度计算帧率 (关键: 解决时长不准问题) */
        int fps_for_ffmpeg;
        if (actual_dur > 0.1) {
            fps_for_ffmpeg = (int)((double)frame_count / actual_dur + 0.5);
        } else {
            fps_for_ffmpeg = eng->config.fps; /* fallback to configured fps */
        }
        if (fps_for_ffmpeg < 1)  fps_for_ffmpeg = 1;
        if (fps_for_ffmpeg > 60) fps_for_ffmpeg = 60;

        printf("[DVR] Child: Frames=%d, window=%.1fs, actual=%.2fs, fps=%d\n",
               frame_count, window_sec, actual_dur, fps_for_ffmpeg);

        int fd = open(filepath, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[DVR] Child: open(%s) failed: %s\n", filepath, strerror(errno));
            free(frames);
            exit(1);
        }

        uint8_t *buf = malloc((size_t)max_frame_size + FRAME_HEADER_SIZE);
        if (!buf) { close(fd); free(frames); exit(1); }

        if (eng->pix_fmt == V4L2_PIX_FMT_MJPEG) {
            /* MJPEG: 先写临时mjpeg文件, 避免管道解析问题 */
            char tmp_mjpg[512];
            snprintf(tmp_mjpg, sizeof(tmp_mjpg), "%s/dvr_tmp.mjpg", eng->config.storage_path);

            int tmp_fd = open(tmp_mjpg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (tmp_fd < 0) {
                fprintf(stderr, "[DVR] Child: open(%s) failed: %s\n", tmp_mjpg, strerror(errno));
                free(buf); close(fd); free(frames); exit(1);
            }

            int sent = 0;
            for (int i = 0; i < frame_count; i++) {
                uint8_t header[FRAME_HEADER_SIZE];
                ssize_t hn = pread(fd, header, FRAME_HEADER_SIZE, frames[i].offset);
                if (hn != FRAME_HEADER_SIZE) continue;

                uint32_t stored_size;
                memcpy(&stored_size, header, 4);
                int data_size = (int)stored_size;
                if (data_size <= 0 || data_size > max_frame_size) continue;

                ssize_t dn = pread(fd, buf, (size_t)data_size,
                                   frames[i].offset + FRAME_HEADER_SIZE);
                if (dn != data_size) continue;

                ssize_t wn = write(tmp_fd, buf, (size_t)dn);
                if (wn != dn) { fprintf(stderr, "[DVR] Child: write tmp failed\n"); break; }
                sent++;
            }
            close(tmp_fd);

            printf("[DVR] Child: wrote %d frames to temp mjpg file\n", sent);

            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -y -f mjpeg -r %d -i %s "
                     "-c:v mpeg4 -q:v 5 -pix_fmt yuv420p -fps_mode cfr %s "
                     "2>/tmp/ffmpeg_stderr.log",
                     fps_for_ffmpeg, tmp_mjpg, filename);

            int ret = system(cmd);
            unlink(tmp_mjpg);

            if (ret != 0) {
                fprintf(stderr, "[DVR] Child: ffmpeg failed with code %d\n", ret);
            } else {
                printf("[DVR] Child: Saved %d frames: %s\n", sent, filename);
            }

            free(buf);
            free(frames);
            close(fd);
            exit(ret != 0 ? 1 : 0);
        } else {
            /* YUYV 等原始格式: 使用管道 */
            char cmd[1024];
            const char *ff_pixfmt = "yuyv422";
            if (eng->pix_fmt == V4L2_PIX_FMT_RGB565) ff_pixfmt = "rgb565";
            else if (eng->pix_fmt == V4L2_PIX_FMT_RGB24) ff_pixfmt = "rgb24";

            snprintf(cmd, sizeof(cmd),
                     "ffmpeg -y -f rawvideo -pix_fmt %s -s %dx%d -r %d -i pipe:0 "
                     "-c:v mpeg4 -q:v 5 -pix_fmt yuv420p -fps_mode cfr %s "
                     "2>/tmp/ffmpeg_stderr.log",
                     ff_pixfmt, eng->config.width, eng->config.height,
                     fps_for_ffmpeg, filename);

            FILE *ffmpeg_pipe = popen(cmd, "w");
            if (!ffmpeg_pipe) {
                fprintf(stderr, "[DVR] Child: popen failed: %s\n", strerror(errno));
                free(buf); close(fd); free(frames); exit(1);
            }

            int sent = 0;
            for (int i = 0; i < frame_count; i++) {
                uint8_t header[FRAME_HEADER_SIZE];
                ssize_t hn = pread(fd, header, FRAME_HEADER_SIZE, frames[i].offset);
                if (hn != FRAME_HEADER_SIZE) continue;

                uint32_t stored_size;
                memcpy(&stored_size, header, 4);
                int data_size = (int)stored_size;
                if (data_size <= 0 || data_size > max_frame_size) continue;

                ssize_t dn = pread(fd, buf, (size_t)data_size,
                                   frames[i].offset + FRAME_HEADER_SIZE);
                if (dn != data_size) continue;

                size_t written = fwrite(buf, 1, (size_t)dn, ffmpeg_pipe);
                if (written != (size_t)dn) break;
                sent++;
            }

            free(buf);
            free(frames);
            close(fd);
            int ret = pclose(ffmpeg_pipe);

            if (ret != 0) {
                fprintf(stderr, "[DVR] Child: ffmpeg failed with code %d\n", ret);
            } else {
                printf("[DVR] Child: Saved %d frames: %s\n", sent, filename);
            }

            exit(ret != 0 ? 1 : 0);
        }
    }

    /* 父进程 */
    pthread_mutex_lock(&rb->lock);
    rb->paused = 1;
    pthread_cond_signal(&rb->write_cond);
    pthread_mutex_unlock(&rb->lock);

    eng->encoder_pid = pid;
    eng->encoder_running = 1;

    free(frames);

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
            printf("[DVR] >>> STATE: IDLE -> BUFFERING (target detected)\n");

            /* 启动NPU验证 */
            if (npu_fusion_available(eng->npu)) {
                eng->npu_verify_state  = NPU_VERIFYING;
                eng->npu_confirm_count = 0;
                eng->npu_deny_count    = 0;
                eng->npu_frame_skip    = 0;
                printf("[DVR] >>> NPU: verification started (need %d confirms)\n",
                       NPU_CONFIRM_NEEDED);
            } else {
                /* 无NPU，直接信任雷达 */
                eng->npu_verify_state = NPU_CONFIRMED;
                printf("[DVR] >>> NPU: not available, trusting radar by default\n");
            }
        }
        break;

    case TRIGGER_TARGET_OFF:
        if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
            eng->state = DVR_STATE_IDLE;
            eng->target_present = 0;
            ring_buffer_clear(eng->ring_buf);
            printf("[DVR] >>> STATE: BUFFERING -> IDLE (target lost)\n");
        } else if (eng->state == DVR_STATE_BUFFERING && eng->save_pending) {
            eng->target_present = 0;
            printf("[DVR] Target off but save pending, keeping buffer\n");
        }
        /* 重置NPU验证状态 */
        eng->npu_verify_state  = NPU_IDLE;
        eng->npu_confirm_count = 0;
        eng->npu_deny_count    = 0;
        break;

    case TRIGGER_WARNING:
    case TRIGGER_FALL:
    case TRIGGER_COLLISION:
        if (eng->state == DVR_STATE_BUFFERING || eng->state == DVR_STATE_IDLE) {
            /* NPU融合验证: 仅在NPU确认道路用户后才保存 */
            if (npu_fusion_available(eng->npu)) {
                if (eng->npu_verify_state == NPU_VERIFYING) {
                    /* NPU还在验证中，等待结果 */
                    printf("[DVR] >>> NPU still verifying, queueing save decision...\n");
                }
                if (eng->npu_verify_state == NPU_DENIED) {
                    /* NPU确认雷达误触发，跳过保存 */
                    printf("[DVR] *** NPU DENIED: radar false positive, SKIPPING save ***\n");
                    eng->npu_verify_state = NPU_IDLE;
                    break;
                }
                /* NPU_CONFIRMED 或 NPU_IDLE(无NPU时已设为CONFIRMED) */
            }

            eng->save_pending      = 1;
            eng->save_event        = data->event;
            eng->save_trigger_time = data->timestamp;

            if (eng->state == DVR_STATE_IDLE) {
                eng->state = DVR_STATE_BUFFERING;
                eng->buffer_start_time = data->timestamp;
                eng->last_cycle_time  = data->timestamp;
            }

            clip_type_t ctype = (clip_type_t)(eng->save_event - TRIGGER_WARNING);
            printf("[DVR] >>> EMERGENCY triggered: %s (protected=%d), NPU=%s\n",
                   clip_type_name(ctype), clip_is_protected(ctype),
                   eng->npu_verify_state == NPU_CONFIRMED ? "CONFIRMED" : "N/A");
        }
        break;
    }
}

dvr_engine_t *dvr_engine_create(const dvr_config_t *config)
{
    dvr_engine_t *eng = calloc(1, sizeof(dvr_engine_t));
    if (!eng) return NULL;

    memcpy(&eng->config, config, sizeof(dvr_config_t));
    eng->state      = DVR_STATE_IDLE;
    eng->running    = 1;
    eng->storage_ok = 1;
    eng->engine_start_time = time(NULL);

    /* 自动创建存储目录 (SD卡路径可能不存在) */
    struct stat st;
    if (stat(config->storage_path, &st) != 0) {
        if (mkdir(config->storage_path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "[DVR] Failed to create storage dir: %s\n", config->storage_path);
            free(eng);
            return NULL;
        }
    }

    /* 1. 打开摄像头 */
    eng->camera = usb_camera_open(config->camera_device,
                                  config->width, config->height, config->fps);
    if (!eng->camera) {
        fprintf(stderr, "[DVR] Camera init failed\n");
        free(eng);
        return NULL;
    }

    eng->pix_fmt = usb_camera_get_pixelformat(eng->camera);

    /* 2. 创建环形缓冲区 (可变帧大小, 最大帧大小预留) */
    int max_frame = config->width * config->height * 2; /* RGB565 worst case */
    if (eng->pix_fmt == V4L2_PIX_FMT_MJPEG) {
        /* MJPEG 1280x720 单帧可达 ~1.8MB, 预留2MB避免截断 */
        max_frame = eng->camera ? usb_camera_get_frame_size(eng->camera) : 2 * 1024 * 1024;
        if (max_frame < 512 * 1024) max_frame = 512 * 1024;
    }
    int capacity_frames = config->buffer_seconds * config->fps * 2; /* 真实帧率可能高于配置值 */
    eng->ring_buf = ring_buffer_create(capacity_frames,
                                       config->storage_path,
                                       max_frame);
    if (!eng->ring_buf) {
        fprintf(stderr, "[DVR] Ring buffer init failed\n");
        usb_camera_close(eng->camera);
        free(eng);
        return NULL;
    }

    /* 3. 创建命名管道触发接收 */
    eng->trigger = trigger_receiver_create("/tmp/dvr_trigger_pipe");
    if (!eng->trigger) {
        printf("[DVR] Trigger pipe not available, manual mode only\n");
    }

    /* 4. 初始化NPU融合验证 */
    if (config->npu_enabled && config->npu_model_path[0]) {
        eng->npu = npu_fusion_create(config->npu_model_path,
                                      config->npu_labels_path,
                                      config->npu_confidence);
        if (eng->npu) {
            /* 预分配RGB解码缓冲区 */
            eng->npu_rgb_buf_size = config->width * config->height * 3;
            eng->npu_rgb_buf = (uint8_t *)malloc((size_t)eng->npu_rgb_buf_size);
            if (!eng->npu_rgb_buf) {
                printf("[DVR] WARNING: NPU RGB buffer alloc failed, NPU disabled\n");
                npu_fusion_destroy(eng->npu);
                eng->npu = NULL;
            }
        }
    } else {
        eng->npu = NULL;
        printf("[DVR] NPU fusion not configured\n");
    }

    printf("[DVR] Engine created, state=IDLE\n");
    printf("[DVR] Rules:\n");
    printf("[DVR]   - TARGET_ON:  start circular %ds buffer\n", config->buffer_seconds);
    printf("[DVR]   - Every %ds:  cycle buffer if target present + no emergency\n", config->buffer_seconds);
    printf("[DVR]   - WARNING/FALL/COLLISION: save %ds clip (%ds before + %ds after)\n",
           config->save_before_seconds + config->save_after_seconds,
           config->save_before_seconds, config->save_after_seconds);
    printf("[DVR]   - TARGET_OFF: stop recording\n");
    if (npu_fusion_available(eng->npu)) {
        printf("[DVR]   - NPU FUSION: radar targets verified by camera AI\n");
    }

    if (config->auto_mode) {
        printf("[DVR] [AUTO] Mode: target=%ds collision=%ds event=%s\n",
               config->auto_target_delay, config->auto_collision_delay,
               config->auto_event == AUTO_EVENT_WARNING ? "WARNING" : "COLLISION");
    }
    return eng;
}

void dvr_engine_destroy(dvr_engine_t *eng)
{
    if (!eng) return;
    dvr_engine_stop(eng);
    if (eng->npu) npu_fusion_destroy(eng->npu);
    if (eng->npu_rgb_buf) free(eng->npu_rgb_buf);
    if (eng->trigger) trigger_receiver_destroy(eng->trigger);
    if (eng->ring_buf) ring_buffer_destroy(eng->ring_buf);
    if (eng->camera) usb_camera_close(eng->camera);
    free(eng);
}

int dvr_engine_run(dvr_engine_t *eng)
{
    if (!eng) return -1;

    int frame_size = usb_camera_get_frame_size(eng->camera);
    if (frame_size <= 0) frame_size = eng->config.width * eng->config.height * 2;
    uint8_t *frame_buf = malloc((size_t)frame_size);
    if (!frame_buf) { return -1; }

    printf("[DVR] Main loop started\n");

    while (eng->running) {
        fd_set fds;
        FD_ZERO(&fds);

        int cam_fd = usb_camera_get_fd(eng->camera);
        FD_SET(cam_fd, &fds);
        int max_fd = cam_fd;

        int trig_fd = -1;
        if (eng->trigger) {
            trig_fd = trigger_receiver_get_fd(eng->trigger);
            FD_SET(trig_fd, &fds);
            if (trig_fd > max_fd) max_fd = trig_fd;
        }

        struct timeval tv = {0, 100000}; /* 100ms timeout */
        int ret = select(max_fd + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* 回收已完成的编码子进程 */
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

                    /* 编码完成后执行延迟的缓冲区清除 (避免竞态条件) */
                    if (eng->need_clear_after_encode) {
                        eng->need_clear_after_encode = 0;
                        ring_buffer_clear(eng->ring_buf);
                        if (!eng->target_present) {
                            eng->state = DVR_STATE_IDLE;
                            printf("[DVR] >>> STATE: -> IDLE (no target, save done)\n");
                        } else {
                            eng->buffer_start_time = time(NULL);
                            eng->last_cycle_time  = time(NULL);
                            printf("[DVR] >>> Save done, buffer cleared, continuing BUFFERING\n");
                        }
                    }
                }
            }
        }

        /* 摄像头帧就绪 */
        if (FD_ISSET(cam_fd, &fds)) {
            int64_t ts;
            int size = usb_camera_grab_frame(eng->camera, frame_buf, frame_size, &ts);
            if (size > 0) {
                if (eng->state == DVR_STATE_BUFFERING && eng->storage_ok) {
                    ring_buffer_push(eng->ring_buf, frame_buf, size, ts);
                }

                /* NPU融合验证: 每5帧跑一次NPU推理 */
                if (eng->npu_verify_state == NPU_VERIFYING && eng->npu_rgb_buf) {
                    eng->npu_frame_skip++;
                    if (eng->npu_frame_skip >= 5) {
                        eng->npu_frame_skip = 0;

                        /* 解码帧到RGB */
                        int decode_ok = 0;
                        if (eng->pix_fmt == V4L2_PIX_FMT_MJPEG) {
                            decode_ok = (frame_decode_mjpeg_to_rgb(
                                frame_buf, size, eng->npu_rgb_buf,
                                eng->config.width, eng->config.height) == 0);
                        } else if (eng->pix_fmt == V4L2_PIX_FMT_YUYV) {
                            frame_decode_yuyv_to_rgb(frame_buf, eng->npu_rgb_buf,
                                                      eng->config.width, eng->config.height);
                            decode_ok = 1;
                        }

                        if (decode_ok) {
                            int road_user = npu_fusion_check_road_user(
                                eng->npu, eng->npu_rgb_buf,
                                eng->config.width, eng->config.height);

                            if (road_user) {
                                eng->npu_confirm_count++;
                                eng->npu_deny_count = 0;
                                printf("[DVR] NPU confirm: %d/%d\n",
                                       eng->npu_confirm_count, NPU_CONFIRM_NEEDED);
                                if (eng->npu_confirm_count >= NPU_CONFIRM_NEEDED) {
                                    eng->npu_verify_state = NPU_CONFIRMED;
                                    printf("[DVR] *** NPU CONFIRMED: road user detected ***\n");
                                }
                            } else {
                                eng->npu_deny_count++;
                                eng->npu_confirm_count = 0;
                                printf("[DVR] NPU deny: %d/%d\n",
                                       eng->npu_deny_count, NPU_DENY_NEEDED);
                                if (eng->npu_deny_count >= NPU_DENY_NEEDED) {
                                    eng->npu_verify_state = NPU_DENIED;
                                    printf("[DVR] *** NPU DENIED: radar false positive ***\n");
                                }
                            }
                        }
                    }
                }
            }
        }

        /* 命名管道触发 */
        if (trig_fd >= 0 && FD_ISSET(trig_fd, &fds)) {
            trigger_receiver_process(eng->trigger, on_trigger, eng);
        }

        /* 存储路径检测 */
        eng->storage_ok = check_storage(eng->config.storage_path);
        if (!eng->storage_ok && eng->state == DVR_STATE_BUFFERING) {
            printf("[DVR] WARNING: Storage path not writable! Buffering continues but cannot save.\n");
        }

        /* 自动触发模式: 按时间线模拟 TARGET_ON → COLLISION */
        if (eng->config.auto_mode) {
            time_t now = time(NULL);
            time_t elapsed = (time_t)difftime(now, eng->engine_start_time);

            /* 第一阶段: 等待 auto_target_delay 秒后触发 TARGET_ON */
            if (!eng->auto_target_sent && elapsed >= eng->config.auto_target_delay) {
                trigger_data_t td;
                memset(&td, 0, sizeof(td));
                td.event     = TRIGGER_TARGET_ON;
                td.timestamp = now;
                on_trigger(&td, eng);
                eng->auto_target_sent = 1;
                printf("[DVR] [AUTO] TARGET_ON sent (elapsed=%lds)\n", (long)elapsed);
            }

            /* 第二阶段: TARGET_ON 后等待 auto_collision_delay 秒触发 COLLISION */
            if (eng->auto_target_sent && !eng->auto_collision_sent) {
                time_t since_target = (time_t)difftime(now, eng->buffer_start_time);
                /* 确保缓冲区积累了足够的前置帧(至少 save_before_seconds) */
                if (since_target >= eng->config.auto_collision_delay
                    && since_target >= eng->config.save_before_seconds) {
                    trigger_data_t td;
                    memset(&td, 0, sizeof(td));
                    td.event     = (eng->config.auto_event == AUTO_EVENT_WARNING)
                                   ? TRIGGER_WARNING : TRIGGER_COLLISION;
                    td.timestamp = now;
                    on_trigger(&td, eng);
                    eng->auto_collision_sent = 1;
                    printf("[DVR] [AUTO] %s sent (since_target=%lds)\n",
                           eng->config.auto_event == AUTO_EVENT_WARNING ? "WARNING" : "COLLISION",
                           (long)since_target);
                }
            }
        }

        /* 循环缓冲区: 每 buffer_seconds 清空一次, 避免无限增长 */
        if (eng->state == DVR_STATE_BUFFERING && !eng->save_pending) {
            time_t now = time(NULL);
            time_t elapsed = now - eng->last_cycle_time;

            if (elapsed >= eng->config.buffer_seconds) {
                printf("[DVR] Circular %ds cycle complete, rotating buffer (target still present)\n",
                       eng->config.buffer_seconds);
                ring_buffer_clear(eng->ring_buf);
                eng->buffer_start_time = now;
                eng->last_cycle_time  = now;
            }
        }

        /* save_pending: 等待触发后补录足够时间 */
        if (eng->save_pending) {
            time_t now = time(NULL);

            int available_before = (int)(eng->save_trigger_time - eng->buffer_start_time);
            if (available_before < 0) available_before = 0;
            if (available_before > eng->config.save_before_seconds)
                available_before = eng->config.save_before_seconds;

            int needed_after = eng->config.save_before_seconds +
                               eng->config.save_after_seconds - available_before;
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
                         eng->config.storage_path,
                         bt->tm_year + 1900, bt->tm_mon + 1, bt->tm_mday,
                         bt->tm_hour, bt->tm_min, bt->tm_sec,
                         evt_name);

                printf("[DVR] Saving clip: [%ld, %ld] -> %s (type=%s, protected=%d)\n",
                       (long)start, (long)end, filename, evt_name, clip_is_protected(ctype));

                if (eng->storage_ok) {
                    save_clip_to_mp4(eng, start, end, filename, ctype);
                } else {
                    printf("[DVR] Storage not available, clip SKIPPED: %s\n", filename);
                }

                if (!eng->target_present) {
                    eng->state = DVR_STATE_IDLE;
                    eng->need_clear_after_encode = 1;
                    printf("[DVR] >>> STATE: -> IDLE (no target, save done)\n");
                } else {
                    eng->need_clear_after_encode = 1;
                    eng->buffer_start_time = time(NULL);
                    eng->last_cycle_time  = time(NULL);
                    printf("[DVR] >>> Save triggered, waiting for encode to clear buffer...\n");
                }
            }
        }

        /* 自动模式: 保存完成后自动退出 */
        if (eng->config.auto_mode && eng->auto_collision_sent
            && !eng->save_pending && !eng->encoder_running) {
            printf("[DVR] [AUTO] Save complete, exiting\n");
            eng->running = 0;
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