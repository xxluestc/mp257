/**
 * nav_tts.c - HUD 导航接收 + OLED 显示 + 骨传导语音播报
 *
 * 数据流：
 *   [手机APP UDP 8888] -> nav_recv_thread() -> 解析 JSON
 *        |
 *        +-- type=navi      -> nav_update_oled() + nav_tts_speak_navdata()
 *        |                     (<=50m 转向/到达时播放预录模板)
 *        +-- type=navi_tts  -> nav_tts_speak()
 *        |                     (缓存命中直接播；未命中尝试 edge-tts 在线生成)
 *        +-- type=danger_tts-> parse_danger_tts_json()
 *        |                     preload 只记录
 *        |                     trigger -> dispatch_danger_text() -> nav_tts_speak_danger()
 *        |                       优先完整 TTS -> 无网络/失败时按关键词播放 danger_*.wav
 *        +-- type=alert     -> nav_tts_alert() 播放固定预警音
 *
 * 公共接口：
 *   nav_tts_start()                 启动 UDP 接收线程 + OLED 看门狗
 *   nav_tts_stop()                  停止线程并关闭 OLED
 *   nav_tts_speak(text)             直接播报任意文本
 *   nav_tts_speak_danger(text)      异常路况播报（含本地兜底）
 *   nav_tts_set_danger_text_handler 注册 danger_tts trigger 的处理函数
 *
 * APP 发送格式：
 *   {"type":"navi","turn":2,"distance":100}
 *   {"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
 *   {"type":"danger_tts","phase":"preload","alert_type":"其他异常","text":"前方停车场出口，请减速观察","distance":67}
 *   {"type":"danger_tts","phase":"trigger","alert_type":"其他异常","text":"前方14米停车场出口，请减速观察","distance":14}
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

#include "nav_tts.h"
#include "common.h" /* hud: NavData */
#include "oled.h"   /* hud: OLED 显示 */
#include "cJSON.h"

#define NAV_CACHE_DIR "/xxl/camera_detect/nav_tts_cache"
#define NAV_GEN_SCRIPT "/xxl/camera_detect/scripts/gen_nav_tts.sh"
#define NAV_TMP_DIR "/tmp"
#define NAV_APLAY_CMD "aplay"
#define NAV_SIGNAL_TIMEOUT 5 /* 导航信号超时时间(s) */

/*
 * 0 = 开发板无网络，禁用所有 edge-tts 在线生成尝试，避免失败日志刷屏
 * 1 = 允许联网时使用 edge-tts 生成完整语音（需要提前在板上安装 edge-tts/ffmpeg）
 */
#define NAV_ONLINE_TTS_ENABLE 0

static atomic_int g_nav_running = 0;
static pthread_t g_nav_tid;
static pthread_t g_nav_watchdog_tid;
static pthread_mutex_t g_nav_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_has_nav = 0; /* guarded by g_nav_mutex */
static time_t g_last_nav_time = 0;
static atomic_int g_oled_inited = 0;
static int g_watchdog_started = 0;

static time_t monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec;
}

#define AUDIO_QUEUE_CAPACITY 8

typedef struct {
    void *(*function)(void *);
    void *argument;
} audio_job_t;

static audio_job_t g_audio_jobs[AUDIO_QUEUE_CAPACITY];
static unsigned int g_audio_count = 0;
static int g_audio_closed = 1;
static pthread_t g_audio_tid;
static pthread_mutex_t g_audio_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_ready = PTHREAD_COND_INITIALIZER;

/* Ownership of argument transfers on success. Priority alerts displace the
 * oldest queued navigation item; active playback is never duplicated. */
static int audio_submit(void *(*function)(void *), void *argument, int priority) {
    pthread_mutex_lock(&g_audio_queue_mutex);
    if (g_audio_closed || (g_audio_count == AUDIO_QUEUE_CAPACITY && !priority)) {
        pthread_mutex_unlock(&g_audio_queue_mutex);
        fprintf(stderr, "[AUDIO] Queue closed/full; notification dropped\n");
        return -1;
    }
    if (g_audio_count == AUDIO_QUEUE_CAPACITY) {
        free(g_audio_jobs[--g_audio_count].argument);
    }
    unsigned int index = g_audio_count++;
    if (priority) {
        memmove(g_audio_jobs + 1, g_audio_jobs, index * sizeof(audio_job_t));
        index = 0;
    }
    g_audio_jobs[index] = (audio_job_t){function, argument};
    pthread_cond_signal(&g_audio_ready);
    pthread_mutex_unlock(&g_audio_queue_mutex);
    return 0;
}

static void *audio_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_audio_queue_mutex);
        while (!g_audio_count && !g_audio_closed)
            pthread_cond_wait(&g_audio_ready, &g_audio_queue_mutex);
        if (g_audio_closed) {
            while (g_audio_count)
                free(g_audio_jobs[--g_audio_count].argument);
            pthread_mutex_unlock(&g_audio_queue_mutex);
            break;
        }
        audio_job_t job = g_audio_jobs[0];
        --g_audio_count;
        memmove(g_audio_jobs, g_audio_jobs + 1, g_audio_count * sizeof(audio_job_t));
        pthread_mutex_unlock(&g_audio_queue_mutex);
        job.function(job.argument);
    }
    return NULL;
}

static void audio_stop(void) {
    pthread_mutex_lock(&g_audio_queue_mutex);
    g_audio_closed = 1;
    pthread_cond_broadcast(&g_audio_ready);
    pthread_mutex_unlock(&g_audio_queue_mutex);
    pthread_join(g_audio_tid, NULL);
}

/*
 * danger_tts 最终文本接口。
 * 默认不播放；队友在主程序中注册骨传导处理函数后，
 * trigger 阶段才会把完整 text 交给该函数。
 */
static pthread_mutex_t g_danger_handler_mutex = PTHREAD_MUTEX_INITIALIZER;
static DangerTextHandler g_danger_text_handler = NULL;

/* ---------------- 工具函数 ---------------- */

/**
 * @brief DJB2 字符串哈希，用于生成缓存文件名
 * @param str 输入字符串（UTF-8 导航文本）
 * @return 32 位无符号哈希值
 */
static unsigned int djb_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++) != 0) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * @brief 将导航文本转换为本地缓存 WAV 文件路径
 * @param text      原始播报文本
 * @param path      输出缓冲区
 * @param path_size 输出缓冲区大小
 *
 * 路径格式：NAV_CACHE_DIR/nav_<hash>_<safe_text>.wav。
 * safe_text 会替换掉文件系统特殊字符并截断到 64 字节，避免文件名过长。
 */
static void text_to_cache_path(const char *text, char *path, size_t path_size) {
    char safe[512];
    int j = 0;
    for (int i = 0; text[i] != '\0' && j < (int)sizeof(safe) - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\'' || c == '"' || c == ';' || c == ':' || c == ',' || c == '.' ||
            c == '!' || c == '?') {
            safe[j++] = '_';
        } else if (isalnum(c) || c >= 0x80) {
            safe[j++] = c;
        } else {
            safe[j++] = '_';
        }
    }
    safe[j] = '\0';

    if (j > 64) {
        safe[64] = '\0';
    }

    unsigned int h = djb_hash(text);
    snprintf(path, path_size, "%s/nav_%08x_%s.wav", NAV_CACHE_DIR, h, safe);
}

/**
 * @brief 确保导航 TTS 缓存目录存在
 * @return 0 成功，-1 失败
 */
static int ensure_cache_dir(void) {
    struct stat st;
    if (stat(NAV_CACHE_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    if (mkdir(NAV_CACHE_DIR, 0777) == 0) {
        return 0;
    }
    fprintf(stderr, "[系统] [NAV] mkdir %s failed: %s\n", NAV_CACHE_DIR, strerror(errno));
    return -1;
}

/* ---------------- danger_tts 采样收集 ---------------- */

#define DANGER_SAMPLE_LOG "/xxl/camera_detect/danger_tts_samples.log"
#define DANGER_SAMPLE_MAX 64

static pthread_mutex_t g_sample_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_sample_history[DANGER_SAMPLE_MAX][NAV_TTS_TEXT_MAX];
static int g_sample_count = 0;

/**
 * @brief 记录手机端发送的 danger_tts 样本（去重），用于确认实际需要哪些 WAV
 * @param phase       preload / trigger
 * @param alert_type  手机端上报的异常大类
 * @param text        完整播报文案
 * @param distance    距离（米）
 */
static void collect_danger_sample(const char *phase, const char *alert_type, const char *text,
                                  int distance) {
    if (!text || !text[0]) {
        return;
    }

    pthread_mutex_lock(&g_sample_mutex);

    /* 简单去重：最近 64 条内重复则跳过 */
    for (int i = 0; i < g_sample_count; i++) {
        if (strcmp(g_sample_history[i], text) == 0) {
            pthread_mutex_unlock(&g_sample_mutex);
            return;
        }
    }

    /* 保存到内存环形缓冲 */
    int idx = g_sample_count % DANGER_SAMPLE_MAX;
    snprintf(g_sample_history[idx], sizeof(g_sample_history[idx]), "%s", text);
    if (g_sample_count < DANGER_SAMPLE_MAX) {
        g_sample_count++;
    }

    /* 追加到日志文件 */
    FILE *fp = fopen(DANGER_SAMPLE_LOG, "a");
    if (fp) {
        time_t now = time(NULL);
        struct tm date;
        struct tm *tm_info = localtime_r(&now, &date);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
        fprintf(fp, "[%s] phase=%s alert_type=%s distance=%d text=%s\n", ts, phase ? phase : "-",
                alert_type ? alert_type : "-", distance, text);
        fclose(fp);
    }

    pthread_mutex_unlock(&g_sample_mutex);
}

/* 音频播放互斥锁：确保多个导航/异常提示音串行播放，避免 aplay 抢设备 */
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 同步执行 aplay，并捕获错误输出
 * @param wav_path 待播放的 WAV 文件路径
 * @param errbuf   错误输出缓冲区
 * @param err_size 错误缓冲区大小
 * @return aplay 退出码，0 表示成功
 */
static int play_wav_sync(const char *wav_path, char *errbuf, size_t err_size) {
    int descriptor = open("/tmp/nav_aplay_err.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(descriptor);
        return -1;
    }
    int setup = posix_spawn_file_actions_adddup2(&actions, descriptor, STDERR_FILENO);
    if (descriptor != STDERR_FILENO)
        setup |= posix_spawn_file_actions_addclose(&actions, descriptor);
    char *arguments[] = {NAV_APLAY_CMD, "-q", (char *)wav_path, NULL};
    pid_t pid = -1;
    int rc = setup ? -1 : posix_spawnp(&pid, NAV_APLAY_CMD, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(descriptor);
    if (rc == 0) {
        time_t deadline = monotonic_seconds() + 15;
        int status = 0;
        for (;;) {
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                break;
            }
            if (result < 0 && errno != EINTR) {
                rc = -1;
                break;
            }
            if (!g_nav_running || monotonic_seconds() >= deadline) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
                rc = -1;
                break;
            }
            usleep(20000);
        }
    }

    FILE *fp = fopen("/tmp/nav_aplay_err.log", "r");
    if (fp) {
        size_t n = fread(errbuf, 1, err_size - 1, fp);
        errbuf[n] = '\0';
        fclose(fp);
    } else {
        errbuf[0] = '\0';
    }
    return rc;
}

/**
 * @brief aplay 播放线程入口
 * @param arg 指向 strdup 复制的 WAV 路径，线程结束时释放
 *
 * 在独立线程中串行播放 WAV，避免阻塞 UDP 接收线程。
 */
static void *aplay_thread(void *arg) {
    char *wav_path = (char *)arg;

    /* 与自身/其他导航语音串行播放，避免多个 aplay 抢设备 */
    pthread_mutex_lock(&g_audio_mutex);

    char errbuf[512] = {0};
    int rc = play_wav_sync(wav_path, errbuf, sizeof(errbuf));
    if (rc != 0) {
        fprintf(stderr, "[系统] [NAV] aplay 播放失败 (rc=%d): %s | %s\n", rc, wav_path,
                errbuf[0] ? errbuf : "no error output");
    }

    pthread_mutex_unlock(&g_audio_mutex);
    free(wav_path);
    return NULL;
}

/**
 * @brief 异步播放 WAV 文件
 * @param wav_path 待播放的 WAV 文件路径
 *
 * 将任务投递给有界音频队列，调用者立即返回继续处理 UDP 消息。
 */
static void play_wav_async(const char *wav_path) {
    char *path_copy = strdup(wav_path);
    if (!path_copy) {
        fprintf(stderr, "[系统] [NAV] strdup failed\n");
        return;
    }

    if (audio_submit(aplay_thread, path_copy, 0) != 0) {
        free(path_copy);
    }
}

int nav_tts_play_file(const char *path) {
    if (!path || !path[0])
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    if (audio_submit(aplay_thread, copy, 1) != 0) {
        free(copy);
        return -1;
    }
    return 0;
}

static int generate_tts_wav(const char *text, const char *output_path) {
    char cmd[2048];

    /* 开发板无网络时直接返回，不尝试在线生成 */
    if (!NAV_ONLINE_TTS_ENABLE) {
        return -1;
    }

    /* 提前检查 edge-tts 是否可用，避免每次失败都重试 */
    static int edge_tts_checked = 0;
    static int edge_tts_available = 0;
    if (!edge_tts_checked) {
        edge_tts_available = (system("command -v edge-tts >/dev/null 2>&1") == 0);
        edge_tts_checked = 1;
        if (!edge_tts_available) {
            fprintf(stderr, "[系统] [NAV] edge-tts 未安装，将使用本地兜底提示音\n");
        }
    }
    if (!edge_tts_available) {
        return -1;
    }

    /* 优先使用项目脚本 */
    if (access(NAV_GEN_SCRIPT, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd), "%s '%s' '%s' >/dev/null 2>&1", NAV_GEN_SCRIPT, text,
                 output_path);
        if (system(cmd) == 0) {
            return 0;
        }
    }

    /* 备用：直接用 edge-tts 命令 */
    char tmp_mp3[256];
    snprintf(tmp_mp3, sizeof(tmp_mp3), "%s/nav_tts_%d.mp3", NAV_TMP_DIR, (int)getpid());

    snprintf(
        cmd, sizeof(cmd),
        "edge-tts --voice zh-CN-XiaoxiaoNeural --text '%s' --write-media %s >/dev/null 2>&1 && "
        "ffmpeg -y -i %s -ar 48000 -ac 2 -sample_fmt s16 %s >/dev/null 2>&1 && "
        "rm -f %s",
        text, tmp_mp3, tmp_mp3, output_path, tmp_mp3);

    int rc = system(cmd);
    if (rc != 0) {
        /* 无网络时失败是预期行为，不再打印失败日志 */
        unlink(tmp_mp3);
        return -1;
    }
    return 0;
}

/* 本地兜底提示音：在线 TTS 不可用时，按关键词播放基础提示 */
static const struct {
    const char *keyword;
    const char *wav;
} g_base_hints[] = {
    {"掉头", "base_掉头.wav"},
    {"左转", "base_左转.wav"},
    {"右转", "base_右转.wav"},
    {"直行", "base_直行.wav"},
    {"到达目的地", "base_到达目的地.wav"},
    {"到达", "base_到达目的地.wav"},
    {"测速", "base_前方有测速.wav"},
};

/**
 * @brief 在线 TTS 不可用时，按关键词播放本地兜底导航提示
 * @param text 完整导航文案
 *
 * 根据文本中的关键词（左转/右转/掉头等）匹配预录 WAV；
 * 无匹配时播放默认提示 "请查看屏幕导航"。
 */
static void play_base_hint(const char *text) {
    if (!text || !text[0]) {
        return;
    }

    char path[1024];
    for (size_t i = 0; i < sizeof(g_base_hints) / sizeof(g_base_hints[0]); i++) {
        if (strstr(text, g_base_hints[i].keyword)) {
            snprintf(path, sizeof(path), "%s/%s", NAV_CACHE_DIR, g_base_hints[i].wav);
            if (access(path, F_OK) == 0) {
                printf("[系统] [NAV] 在线 TTS 不可用，播放本地兜底: %s -> %s\n",
                       g_base_hints[i].keyword, path);
                play_wav_async(path);
                return;
            }
        }
    }

    snprintf(path, sizeof(path), "%s/base_请查看屏幕导航.wav", NAV_CACHE_DIR);
    if (access(path, F_OK) == 0) {
        printf("[系统] [NAV] 在线 TTS 不可用，播放默认兜底提示\n");
        play_wav_async(path);
    }
}

/* 异常路况本地固定提示音：按关键词直接播放预存 WAV。
 * 当前手机端只会上报两种异常：
 *   - 前方路面有障碍物，请注意避让  -> danger_障碍物.wav
 *   - 前方施工路段，请减速慢行      -> danger_施工.wav
 * 后续如果手机端新增异常类型，只需在此表和 nav_tts_cache/ 中新增对应文件即可。 */
static const struct {
    const char *keyword;
    const char *wav;
    const char *display; /* 仅用于日志打印 */
} g_danger_hints[] = {
    {"障碍物", "danger_障碍物.wav", "前方路面有障碍物，请注意避让"},
    {"施工", "danger_施工.wav", "前方施工路段，请减速慢行"},
};

#define DANGER_COOLDOWN 8

/* 每个关键词独立冷却，不同异常类型互不阻塞 */
static time_t s_last_danger_time_by_hint[sizeof(g_danger_hints) / sizeof(g_danger_hints[0])] = {0};

/**
 * @brief 判断文本是否能匹配到本地异常路况固定提示音
 * @return 1 表示有对应本地文件，0 表示无
 */
static int danger_hint_exists(const char *text) {
    if (!text || !text[0]) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(g_danger_hints) / sizeof(g_danger_hints[0]); i++) {
        if (strstr(text, g_danger_hints[i].keyword)) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", NAV_CACHE_DIR, g_danger_hints[i].wav);
            if (access(path, F_OK) == 0) {
                return 1;
            }
        }
    }

    return 0;
}

/**
 * @brief 按关键词匹配并播放本地异常路况固定提示音
 * @return 1 表示成功触发播放，0 表示未播放（冷却中或无匹配文件）
 */
static int play_danger_hint(const char *text) {
    if (!text || !text[0]) {
        return 0;
    }

    time_t now = monotonic_seconds();
    char path[1024];

    /* 按关键词匹配具体类型，每个类型独立冷却 */
    for (size_t i = 0; i < sizeof(g_danger_hints) / sizeof(g_danger_hints[0]); i++) {
        if (strstr(text, g_danger_hints[i].keyword)) {
            snprintf(path, sizeof(path), "%s/%s", NAV_CACHE_DIR, g_danger_hints[i].wav);
            if (access(path, F_OK) == 0) {
                if ((now - s_last_danger_time_by_hint[i]) < DANGER_COOLDOWN) {
                    printf("[系统] [DANGER_TTS] 同类提示冷却中 (%ds)，跳过: %s\n", DANGER_COOLDOWN,
                           g_danger_hints[i].keyword);
                    return 0;
                }
                printf("[系统] [DANGER_TTS] 播放本地固定提示: %s -> %s\n",
                       g_danger_hints[i].display, path);
                play_wav_async(path);
                s_last_danger_time_by_hint[i] = now;
                return 1;
            }
        }
    }

    /* 当前只有两种已知异常，未匹配时不播放，避免乱播 */
    printf("[系统] [DANGER_TTS] 未匹配已知关键词，跳过: %s\n", text);
    return 0;
}

/* 导航语音播报冷却控制：避免同一转向在近距离重复播报 */
#define NAV_SPEAK_COOLDOWN 15

static int is_turn_action(int turn) {
    return (turn == TURN_LEFT || turn == TURN_RIGHT || turn == TURN_SLIGHT_LEFT ||
            turn == TURN_SLIGHT_RIGHT || turn == TURN_UTURN_LEFT || turn == TURN_UTURN_RIGHT ||
            turn == TURN_DESTINATION);
}

/* 结构化导航数据本地播报：根据 turn + distance 直接播放预录模板，无需网络。
 * 为了和 OLED 显示对齐并避免乱播，只在距离 <= 50m 且需要转向/到达时播报一次。 */
static void nav_tts_speak_navdata(const NavData *nav) {
    if (!nav) {
        return;
    }

    /* 只播报需要动作的关键节点，且距离已经很近（<= 50m） */
    if (!is_turn_action(nav->turn)) {
        return;
    }
    if (nav->distance > 50) {
        return;
    }

    static time_t s_last_speak_time = 0;
    static int s_last_speak_turn = -1;
    time_t now = monotonic_seconds();

    /* 同一转向在冷却时间内不重复播报 */
    if (nav->turn == s_last_speak_turn && (now - s_last_speak_time) < NAV_SPEAK_COOLDOWN) {
        return;
    }

    const char *dir = "直行";
    switch (nav->turn) {
    case TURN_LEFT:
        dir = "左转";
        break;
    case TURN_RIGHT:
        dir = "右转";
        break;
    case TURN_SLIGHT_LEFT:
        dir = "左转";
        break;
    case TURN_SLIGHT_RIGHT:
        dir = "右转";
        break;
    case TURN_UTURN_LEFT:
    case TURN_UTURN_RIGHT:
        dir = "掉头";
        break;
    case TURN_DESTINATION:
        dir = "到达目的地";
        break;
    default:
        dir = "直行";
        break;
    }

    const char *dist_label;
    int d = nav->distance;
    if (d < 75)
        dist_label = "50米";
    else if (d < 150)
        dist_label = "100米";
    else if (d < 250)
        dist_label = "200米";
    else if (d < 400)
        dist_label = "300米";
    else if (d < 650)
        dist_label = "500米";
    else if (d < 900)
        dist_label = "800米";
    else if (d < 1500)
        dist_label = "1公里";
    else if (d < 2500)
        dist_label = "2公里";
    else if (d < 4000)
        dist_label = "3公里";
    else
        dist_label = "5公里";

    char text[128];
    char path[1024];
    if (nav->turn == TURN_DESTINATION) {
        snprintf(text, sizeof(text), "前方%s到达目的地", dist_label);
    } else {
        snprintf(text, sizeof(text), "前方%s%s", dist_label, dir);
    }
    snprintf(path, sizeof(path), "%s/nav_%s.wav", NAV_CACHE_DIR, text);

    if (access(path, F_OK) == 0) {
        printf("[系统] [NAV] 结构化播报: %s (turn=%d distance=%dm)\n", text, nav->turn,
               nav->distance);
        play_wav_async(path);
        s_last_speak_time = now;
        s_last_speak_turn = nav->turn;
    } else {
        fprintf(stderr, "[系统] [NAV] 缺少模板 %s，回退到方向提示\n", path);
        play_base_hint(dir);
        s_last_speak_time = now;
        s_last_speak_turn = nav->turn;
    }
}

typedef struct {
    char text[NAV_TTS_TEXT_MAX];
    char cache_path[1024];
    int is_danger; /* 1=异常路况，生成失败时回退到本地固定提示 */
} nav_tts_task_t;

static void *nav_tts_generate_thread(void *arg) {
    nav_tts_task_t *task = (nav_tts_task_t *)arg;

    /* 在线生成已禁用，直接失败，不再打印无网络错误日志 */
    if (!NAV_ONLINE_TTS_ENABLE) {
        free(task);
        return NULL;
    }

    printf("[系统] [%s] 正在生成 TTS: %s\n", task->is_danger ? "DANGER_TTS" : "NAV", task->text);

    if (generate_tts_wav(task->text, task->cache_path) == 0) {
        play_wav_async(task->cache_path);
    } else {
        if (task->is_danger) {
            /* 异常路况：在线生成失败时回退本地固定提示 */
            play_danger_hint(task->text);
        }
        /* navi_tts 失败时静默，避免无网络时刷屏 */
    }

    free(task);
    return NULL;
}

/* ---------------- 公共接口 ---------------- */

void nav_tts_set_danger_text_handler(DangerTextHandler handler) {
    pthread_mutex_lock(&g_danger_handler_mutex);
    g_danger_text_handler = handler;
    pthread_mutex_unlock(&g_danger_handler_mutex);

    printf("[系统] [DANGER_TTS][接口] %s\n",
           handler ? "骨传导文本接口已注册" : "骨传导文本接口已取消");
}

/*
 * 只在 trigger 阶段调用。
 * 复制函数指针后立即释放锁，避免上层处理阻塞注册操作。
 */
static void dispatch_danger_text(const char *text) {
    DangerTextHandler handler = NULL;

    if (text == NULL || text[0] == '\0') {
        return;
    }

    pthread_mutex_lock(&g_danger_handler_mutex);
    handler = g_danger_text_handler;
    pthread_mutex_unlock(&g_danger_handler_mutex);

    if (handler != NULL) {
        printf("[系统] [DANGER_TTS][接口] 提交最终文本: %s\n", text);
        handler(text);
    } else {
        printf("[系统] [DANGER_TTS][接口] 文本已解析，等待队友接入骨传导: %s\n", text);
    }
}

void nav_tts_speak(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }

    char clean[NAV_TTS_TEXT_MAX];
    int j = 0;
    for (int i = 0; text[i] != '\0' && j < (int)sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        clean[j++] = c;
    }
    clean[j] = '\0';

    if (clean[0] == '\0') {
        return;
    }

    printf("[系统] [NAV] 收到语音播报: %s\n", clean);

    if (ensure_cache_dir() != 0) {
        return;
    }

    char cache_path[1024];
    text_to_cache_path(clean, cache_path, sizeof(cache_path));

    if (access(cache_path, F_OK) == 0) {
        printf("[系统] [NAV] 命中缓存: %s\n", cache_path);
        play_wav_async(cache_path);
        return;
    }

    /* 无网络时不尝试在线生成，避免失败日志 */
    if (!NAV_ONLINE_TTS_ENABLE) {
        return;
    }

    printf("[系统] [NAV] 缓存未命中，后台生成: %s\n", cache_path);

    nav_tts_task_t *task = (nav_tts_task_t *)calloc(1, sizeof(nav_tts_task_t));
    if (!task) {
        fprintf(stderr, "[系统] [NAV] 内存不足\n");
        return;
    }
    snprintf(task->text, sizeof(task->text), "%s", clean);
    snprintf(task->cache_path, sizeof(task->cache_path), "%s", cache_path);

    if (audio_submit(nav_tts_generate_thread, task, 0) != 0)
        free(task);
}

/*
 * 异常路况播报入口。
 * 开发板无网络，优先按关键词播放本地固定提示音；
 * 只有本地未命中（无关键词、无默认文件）时，才尝试完整 TTS 缓存或在线生成。
 */
void nav_tts_speak_danger(const char *text) {
    if (text == NULL || text[0] == '\0') {
        return;
    }

    char clean[NAV_TTS_TEXT_MAX];
    int j = 0;
    for (int i = 0; text[i] != '\0' && j < (int)sizeof(clean) - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        clean[j++] = c;
    }
    clean[j] = '\0';

    if (clean[0] == '\0') {
        return;
    }

    printf("[系统] [DANGER_TTS] 收到异常路况播报: %s\n", clean);

    /* 1. 本地有固定提示：直接播放（含冷却控制），绝不再走网络 */
    if (danger_hint_exists(clean)) {
        play_danger_hint(clean);
        return;
    }

    /* 2. 本地未命中：尝试完整 TTS 缓存 */
    if (ensure_cache_dir() != 0) {
        return;
    }

    char cache_path[1024];
    text_to_cache_path(clean, cache_path, sizeof(cache_path));

    if (access(cache_path, F_OK) == 0) {
        printf("[系统] [DANGER_TTS] 命中完整缓存: %s\n", cache_path);
        play_wav_async(cache_path);
        return;
    }

    /* 3. 仍无缓存且允许联网时才尝试在线生成 */
    if (!NAV_ONLINE_TTS_ENABLE) {
        return;
    }

    printf("[系统] [DANGER_TTS] 缓存未命中，后台生成完整语音: %s\n", cache_path);

    nav_tts_task_t *task = (nav_tts_task_t *)calloc(1, sizeof(nav_tts_task_t));
    if (!task) {
        fprintf(stderr, "[系统] [DANGER_TTS] 内存不足\n");
        return;
    }
    snprintf(task->text, sizeof(task->text), "%s", clean);
    snprintf(task->cache_path, sizeof(task->cache_path), "%s", cache_path);
    task->is_danger = 1;

    if (audio_submit(nav_tts_generate_thread, task, 0) != 0)
        free(task);
}

/* ---------------- OLED 导航显示 ---------------- */

static void nav_update_oled(const NavData *nav) {
    if (!g_oled_inited) {
        return;
    }
    pthread_mutex_lock(&g_nav_mutex);
    if (nav) {
        g_has_nav = 1;
        g_last_nav_time = monotonic_seconds();
    } else {
        g_has_nav = 0;
    }
    if (oled_show_nav(nav, nav ? 1 : 0) != 0)
        fprintf(stderr, "[NAV] OLED update failed: %s\n", strerror(errno));
    pthread_mutex_unlock(&g_nav_mutex);
}

static void *nav_watchdog_thread(void *arg) {
    (void)arg;
    while (g_nav_running) {
        sleep(1);
        pthread_mutex_lock(&g_nav_mutex);
        if (g_has_nav && (monotonic_seconds() - g_last_nav_time) > NAV_SIGNAL_TIMEOUT) {
            printf("[系统] [NAV] 导航信号超时，OLED 显示 NO DATA\n");
            g_has_nav = 0;
            if (g_oled_inited) {
                if (oled_show_nav(NULL, 0) != 0)
                    fprintf(stderr, "[NAV] OLED timeout display failed: %s\n", strerror(errno));
            }
        }
        pthread_mutex_unlock(&g_nav_mutex);
    }
    return NULL;
}

/* ---------------- JSON 解析 ---------------- */

/**
 * @brief 解析 type=navi 结构化导航 JSON
 * @param json_str JSON 字符串
 * @param nav      输出解析后的转向/距离数据
 * @return 0 成功，-1 失败
 */
static int parse_navi_json(const char *json_str, NavData *nav) {
    cJSON *root = cJSON_ParseWithOpts(json_str, NULL, 1);
    if (root == NULL) {
        return -1;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    cJSON *turn_obj = cJSON_GetObjectItem(root, "turn");
    cJSON *dist_obj = cJSON_GetObjectItem(root, "distance");

    if (!cJSON_IsString(type_obj) || type_obj->valuestring == NULL ||
        strcmp(type_obj->valuestring, "navi") != 0 || !cJSON_IsNumber(turn_obj) ||
        !cJSON_IsNumber(dist_obj)) {
        cJSON_Delete(root);
        return -1;
    }

    nav->turn = turn_obj->valueint;
    nav->distance = dist_obj->valueint;
    cJSON_Delete(root);
    return 0;
}

/**
 * @brief 解析 type=navi_tts 完整导航语音 JSON
 * @param json_str  JSON 字符串
 * @param text_out  输出播报文本缓冲区
 * @param text_size 输出缓冲区大小
 * @return 0 成功，-1 失败
 */
static int parse_navi_tts_json(const char *json_str, char *text_out, size_t text_size) {
    cJSON *root = cJSON_ParseWithOpts(json_str, NULL, 1);
    if (root == NULL) {
        return -1;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    cJSON *text_obj = cJSON_GetObjectItem(root, "text");

    if (!cJSON_IsString(type_obj) || type_obj->valuestring == NULL ||
        strcmp(type_obj->valuestring, "navi_tts") != 0 || !cJSON_IsString(text_obj) ||
        text_obj->valuestring == NULL || text_obj->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    snprintf(text_out, text_size, "%s", text_obj->valuestring);
    cJSON_Delete(root);
    return 0;
}

/**
 * @brief 解析 type=danger_tts 异常路况 JSON
 * @param json_str       JSON 字符串
 * @param phase_out      输出阶段字符串（preload/trigger）
 * @param phase_size     phase_out 缓冲区大小
 * @param alert_type_out 输出异常大类
 * @param alert_type_size alert_type_out 缓冲区大小
 * @param text_out       输出完整播报文案
 * @param text_size      text_out 缓冲区大小
 * @param distance_out   输出距离（米），可为 NULL
 * @return 0 成功，-1 失败
 *
 * 兼容旧版数据：若缺少 phase 字段，则按 trigger 处理。
 */
static int parse_danger_tts_json(const char *json_str, char *phase_out, size_t phase_size,
                                 char *alert_type_out, size_t alert_type_size, char *text_out,
                                 size_t text_size, int *distance_out) {
    cJSON *root = cJSON_ParseWithOpts(json_str, NULL, 1);
    if (root == NULL) {
        return -1;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    cJSON *phase_obj = cJSON_GetObjectItem(root, "phase");
    cJSON *alert_type_obj = cJSON_GetObjectItem(root, "alert_type");
    cJSON *text_obj = cJSON_GetObjectItem(root, "text");
    cJSON *distance_obj = cJSON_GetObjectItem(root, "distance");

    if (!cJSON_IsString(type_obj) || type_obj->valuestring == NULL ||
        strcmp(type_obj->valuestring, "danger_tts") != 0 || !cJSON_IsString(text_obj) ||
        text_obj->valuestring == NULL || text_obj->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return -1;
    }

    /* 兼容旧版数据：缺少 phase 时按照正式触发处理。 */
    if (cJSON_IsString(phase_obj) && phase_obj->valuestring != NULL &&
        phase_obj->valuestring[0] != '\0') {
        snprintf(phase_out, phase_size, "%s", phase_obj->valuestring);
    } else {
        snprintf(phase_out, phase_size, "%s", DANGER_TTS_PHASE_TRIGGER);
    }

    if (cJSON_IsString(alert_type_obj) && alert_type_obj->valuestring != NULL &&
        alert_type_obj->valuestring[0] != '\0') {
        snprintf(alert_type_out, alert_type_size, "%s", alert_type_obj->valuestring);
    } else {
        snprintf(alert_type_out, alert_type_size, "%s", "unknown");
    }

    /*
     * text 是手机端已经生成好的最终播报文案。
     * 接收端只复制，不根据 distance 再拼接，避免两端距离不一致。
     */
    snprintf(text_out, text_size, "%s", text_obj->valuestring);

    if (distance_out != NULL) {
        *distance_out = cJSON_IsNumber(distance_obj) ? distance_obj->valueint : -1;
    }

    cJSON_Delete(root);
    return 0;
}

/**
 * @brief 解析 type=alert/warning/front_alert/collision 预警 JSON
 * @param json_str  JSON 字符串
 * @param alert_type 输出预警类型/消息
 * @param type_size 输出缓冲区大小
 * @return 0 成功，-1 失败
 */
static int parse_alert_json(const char *json_str, char *alert_type, size_t type_size) {
    cJSON *root = cJSON_ParseWithOpts(json_str, NULL, 1);
    if (root == NULL) {
        return -1;
    }

    cJSON *type_obj = cJSON_GetObjectItem(root, "type");
    cJSON *msg_obj = cJSON_GetObjectItem(root, "message");

    if (!cJSON_IsString(type_obj) || type_obj->valuestring == NULL) {
        cJSON_Delete(root);
        return -1;
    }

    const char *t = type_obj->valuestring;
    if (strcmp(t, "alert") != 0 && strcmp(t, "warning") != 0 && strcmp(t, "front_alert") != 0 &&
        strcmp(t, "collision") != 0) {
        cJSON_Delete(root);
        return -1;
    }

    if (msg_obj && cJSON_IsString(msg_obj) && msg_obj->valuestring) {
        snprintf(alert_type, type_size, "%s", msg_obj->valuestring);
    } else {
        snprintf(alert_type, type_size, "%s", t);
    }

    cJSON_Delete(root);
    return 0;
}

/* 预警播报：固定音频，冷却 10s 避免连播 */
static void nav_tts_alert(const char *alert_type) {
    (void)alert_type;

    static time_t s_last_alert_time = 0;
    time_t now = monotonic_seconds();
    if ((now - s_last_alert_time) < 10) {
        return;
    }

    const char *candidate_paths[] = {
        NAV_CACHE_DIR "/alert_前方车辆靠近.wav",
        NAV_CACHE_DIR "/alert_请注意.wav",
    };
    for (size_t i = 0; i < sizeof(candidate_paths) / sizeof(candidate_paths[0]); i++) {
        if (access(candidate_paths[i], F_OK) == 0) {
            printf("[系统] [NAV] 前方预警播报: %s\n", candidate_paths[i]);
            play_wav_async(candidate_paths[i]);
            s_last_alert_time = now;
            return;
        }
    }
}

/* ---------------- UDP 接收线程 ---------------- */

/**
 * @brief 导航 UDP 接收线程入口
 * @param arg 未使用
 *
 * 绑定 UDP 8888 端口，循环接收 APP 下发的 JSON 消息，
 * 按 type 分发到 navi / navi_tts / danger_tts / alert 处理逻辑。
 */
static void *nav_recv_thread(void *arg) {
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        perror("[系统] [NAV] socket");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(NAV_UDP_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[系统] [NAV] bind");
        close(sock);
        return NULL;
    }

    printf("[系统] [NAV] UDP receiver ready on port %d\n", NAV_UDP_PORT);

    char buffer[2048];
    struct pollfd fds = {.fd = sock, .events = POLLIN};

    while (g_nav_running) {
        int ret = poll(&fds, 1, 100);
        if (ret <= 0) {
            continue;
        }

        int n = (int)recvfrom(sock, buffer, sizeof(buffer) - 1, 0, NULL, NULL);
        if (n > 0) {
            buffer[n] = '\0';
            printf("[系统] [NAV] 收到数据: %s\n", buffer);

            NavData nav;
            char text[NAV_TTS_TEXT_MAX];
            char danger_phase[16];
            char danger_alert_type[64];
            int danger_distance = -1;

            if (parse_navi_json(buffer, &nav) == 0) {
                printf("[系统] [NAV] OLED 导航: turn=%d distance=%dm\n", nav.turn, nav.distance);
                nav_update_oled(&nav);
                nav_tts_speak_navdata(&nav);
            } else if (parse_navi_tts_json(buffer, text, sizeof(text)) == 0) {
                nav_tts_speak(text);
            } else if (parse_danger_tts_json(buffer, danger_phase, sizeof(danger_phase),
                                             danger_alert_type, sizeof(danger_alert_type), text,
                                             sizeof(text), &danger_distance) == 0) {
                printf("[系统] [DANGER_TTS][解析成功] "
                       "phase=%s alert_type=%s distance=%d text=%s\n",
                       danger_phase, danger_alert_type, danger_distance, text);

                /* 记录手机实际发送的异常文案，用于确认需要哪些 WAV */
                collect_danger_sample(danger_phase, danger_alert_type, text, danger_distance);

                if (strcmp(danger_phase, DANGER_TTS_PHASE_PRELOAD) == 0) {
                    /* 80米预加载阶段：确认收到即可，不交给骨传导。 */
                    printf("[系统] [DANGER_TTS][PRELOAD] 已接收，不播放\n");
                } else if (strcmp(danger_phase, DANGER_TTS_PHASE_TRIGGER) == 0) {
                    /* 30米正式触发：把手机生成的最终文本交给队友接口。 */
                    printf("[系统] [DANGER_TTS][TRIGGER] 最终文本已就绪\n");
                    dispatch_danger_text(text);
                } else {
                    printf("[系统] [DANGER_TTS][忽略] 未知 phase=%s\n", danger_phase);
                }
            } else if (parse_alert_json(buffer, text, sizeof(text)) == 0) {
                printf("[系统] [NAV] 收到预警: %s\n", text);
                nav_tts_alert(text);
            } else {
                printf("[系统] [NAV] 未知 JSON 类型，忽略\n");
            }
        }
    }

    close(sock);
    printf("[系统] [NAV] UDP receiver stopped\n");
    return NULL;
}

/**
 * @brief 启动导航 UDP 接收线程和 OLED 看门狗线程
 * @return 0 成功，-1 失败
 *
 * 同时初始化 OLED 与有界音频队列。
 */
int nav_tts_start(void) {
    if (g_nav_running) {
        return 0;
    }
    g_nav_running = 1;
    g_audio_closed = 0;
    if (pthread_create(&g_audio_tid, NULL, audio_worker, NULL) != 0) {
        g_nav_running = 0;
        g_audio_closed = 1;
        return -1;
    }

    if (oled_init() == 0) {
        g_oled_inited = 1;
        printf("[系统] [NAV] OLED 初始化成功\n");
    } else {
        fprintf(stderr, "[系统] [NAV] OLED 初始化失败，将继续运行\n");
    }

    if (pthread_create(&g_nav_tid, NULL, nav_recv_thread, NULL) != 0) {
        g_nav_running = 0;
        audio_stop();
        if (g_oled_inited)
            oled_close();
        g_oled_inited = 0;
        return -1;
    }

    if (pthread_create(&g_nav_watchdog_tid, NULL, nav_watchdog_thread, NULL) != 0) {
        fprintf(stderr, "[系统] [NAV] watchdog 线程创建失败\n");
    } else
        g_watchdog_started = 1;

    return 0;
}

/**
 * @brief 停止导航接收线程并关闭 OLED
 */
void nav_tts_stop(void) {
    if (!g_nav_running) {
        return;
    }
    g_nav_running = 0;
    pthread_join(g_nav_tid, NULL);
    if (g_watchdog_started)
        pthread_join(g_nav_watchdog_tid, NULL);
    g_watchdog_started = 0;
    audio_stop();
    if (g_oled_inited) {
        oled_close();
        g_oled_inited = 0;
    }
}
