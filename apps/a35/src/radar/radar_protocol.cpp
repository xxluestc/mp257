#include "app/services.hpp"
#include "runtime/frame_pipeline.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/select.h>
#include <cmath>
#include <cfloat>

static radar_direction_filter_t g_direction_filters[256];

const char *radar_direction_name(radar_direction_t direction) {
    switch (direction) {
    case RADAR_DIR_LEFT:
        return "LEFT";
    case RADAR_DIR_CENTER:
        return "CENTER";
    case RADAR_DIR_RIGHT:
        return "RIGHT";
    default:
        return "UNKNOWN";
    }
}

static radar_direction_t classify_radar_direction(float angle, radar_direction_t current) {
    /*
     * 在边界处加入滞回：已经处于 LEFT/RIGHT 时，目标必须明显回到 CENTER
     * 才允许切换，避免阈值附近一帧一跳。
     */
    if (current == RADAR_DIR_LEFT && angle <= g_angle_left_threshold + DIRECTION_HYSTERESIS_DEG)
        return RADAR_DIR_LEFT;
    if (current == RADAR_DIR_RIGHT && angle >= g_angle_right_threshold - DIRECTION_HYSTERESIS_DEG)
        return RADAR_DIR_RIGHT;

    if (angle <= g_angle_left_threshold)
        return RADAR_DIR_LEFT;
    if (angle >= g_angle_right_threshold)
        return RADAR_DIR_RIGHT;
    return RADAR_DIR_CENTER;
}

static radar_direction_t update_direction_filter(int obj_id, float raw_angle, uint64_t now_ms,
                                                 float *filtered_angle_out) {
    radar_direction_filter_t *filter = &g_direction_filters[(unsigned int)obj_id & 0xFFU];

    if (!filter->valid || now_ms - filter->last_seen_ms > 5000ULL) {
        memset(filter, 0, sizeof(*filter));
        filter->valid = 1;
        filter->filtered_angle = raw_angle;
        filter->stable_direction =
            classify_radar_direction(g_angle_direction_sign * raw_angle, RADAR_DIR_UNKNOWN);
        filter->candidate_direction = filter->stable_direction;
    } else {
        filter->filtered_angle = g_angle_filter_alpha * raw_angle +
                                 (1.0f - g_angle_filter_alpha) * filter->filtered_angle;

        radar_direction_t candidate = classify_radar_direction(
            g_angle_direction_sign * filter->filtered_angle, filter->stable_direction);
        if (candidate == filter->stable_direction) {
            filter->candidate_direction = candidate;
            filter->candidate_count = 0;
        } else if (candidate == filter->candidate_direction) {
            filter->candidate_count++;
        } else {
            filter->candidate_direction = candidate;
            filter->candidate_count = 1;
        }

        if (filter->candidate_count >= g_direction_stable_samples) {
            filter->stable_direction = filter->candidate_direction;
            filter->candidate_count = 0;
        }
    }

    filter->last_seen_ms = now_ms;
    if (filtered_angle_out != NULL)
        *filtered_angle_out = filter->filtered_angle;
    return filter->stable_direction;
}

/**
 * @brief 解析 BSD 报告并计算是否需要告警
 * @param bsd 解析后的 BSD 检测数据
 * @return 雷达处理结果
 *
 * 告警条件：
 *   - 任一目标靠近且 TTC < g_ttc_threshold，或
 *   - 任一目标距离 <= g_dist_threshold
 *
 * 危险目标按 min(TTC/TTC阈值, 距离/距离阈值) 选择。该选择方式保留原告警
 * 条件，同时保证距离与 TTC 始终来自同一个 objId。
 */
static radar_result_t process_bsd_report(const bsd_det_t *bsd) {
    radar_result_t result{};
    int obj_count = bsd->obj_num;
    if (obj_count > MAX_RADAR_OBJECTS)
        obj_count = MAX_RADAR_OBJECTS;
    result.obj_count = obj_count;
    result.dangerous_index = -1;
    result.dangerous_obj_id = -1;
    result.min_distance = 9999.0f;
    result.min_ttc = 9999.0f;
    float best_risk = FLT_MAX;

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms = helmet::monotonic_us() / 1000;

    for (int i = 0; i < obj_count; i++) {
        const bsd_obj_t *o = &bsd->obj[i];
        radar_target_t *target = &result.targets[i];
        target->obj_id = (int)(uint8_t)o->objId;
        target->distance = (float)o->range_val;
        target->velocity = (float)o->velo_val;
        target->angle = (float)o->angle_val;
        target->ttc = -1.0f;
        target->direction =
            update_direction_filter(target->obj_id, target->angle, now_ms, &target->filtered_angle);

        if (target->distance <= 0.0f)
            continue;

        result.has_target = 1;
        if (target->distance < result.min_distance)
            result.min_distance = target->distance;

        if (target->velocity < 0.0f) {
            target->ttc = target->distance / -target->velocity;
            if (target->ttc < result.min_ttc)
                result.min_ttc = target->ttc;
            result.approaching = 1;
        }
        if ((target->ttc >= 0.0f && target->ttc < g_ttc_threshold) ||
            target->distance <= g_dist_threshold) {
            result.should_alert = 1;
        }

        float dist_ratio = target->distance / fmaxf(g_dist_threshold, 0.1f);
        float ttc_ratio =
            (target->ttc >= 0.0f) ? target->ttc / fmaxf(g_ttc_threshold, 0.1f) : FLT_MAX;
        float risk = fminf(dist_ratio, ttc_ratio);
        if (risk < best_risk ||
            (fabsf(risk - best_risk) < 0.0001f && result.dangerous_index >= 0 &&
             target->distance < result.targets[result.dangerous_index].distance)) {
            best_risk = risk;
            result.dangerous_index = i;
            result.dangerous_obj_id = target->obj_id;
        }
    }

    return result;
}

/**
 * @brief 从雷达串口数据帧中提取 BSD 报告
 * @param frame     接收到的数据帧
 * @param frame_len 数据帧长度
 * @param out       输出解析结果
 * @return 1 成功解析到 BSD 报告，0 非 BSD 帧或无需处理，-1 数据异常
 *
 * 支持 HEAD_REPORT 报告帧和 HEAD_REPLY 回复帧，自动校验 sum8 校验和。
 */
int process_radar_frame(const uint8_t *frame, int frame_len, radar_result_t *out) {
    if (frame_len < 4)
        return -1;
    uint8_t head = frame[0];
    if (head == HEAD_REPORT) {
        uint8_t len = frame[1];
        if (len < 1 || 2 + len + 1 != frame_len)
            return -1;
        if (calc_sum8(frame, 2 + len) != frame[2 + len])
            return -1;
        const uint8_t *payload = &frame[2];
        if (payload[0] != TYPE_BSD)
            return 0;
        /* LEN 包含 1 字节 TYPE；跳过 TYPE 后剩余长度必须同步减 1。 */
        int data_len = (int)len - 1;
        const uint8_t *data = payload + 1;
        if (data_len < 4)
            return 0;
        bsd_det_t bsd;
        memset(&bsd, 0, sizeof(bsd));
        bsd.obj_num = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        int oc = bsd.obj_num;
        if (oc > MAX_RADAR_OBJECTS)
            oc = MAX_RADAR_OBJECTS;
        int expected = 4 + oc * (int)sizeof(bsd_obj_t);
        if (data_len < expected)
            oc = (data_len - 4) / (int)sizeof(bsd_obj_t);
        if (oc < 0)
            oc = 0;
        bsd.obj_num = (uint16_t)oc;
        for (int i = 0; i < oc; i++) {
            int off = 4 + i * (int)sizeof(bsd_obj_t);
            if (off + (int)sizeof(bsd_obj_t) <= data_len)
                memcpy(&bsd.obj[i], &data[off], sizeof(bsd_obj_t));
        }
        *out = process_bsd_report(&bsd);
        return 1;
    }
    return 0;
}

int radar_init(int fd) {
    printf("[RADAR] Initializing...\n");
    fflush(stdout);
    uint8_t rx_buf[512];
    auto wait_reply = [&](const char *command, uint8_t expected_command, int timeout_ms) {
        uint8_t pending[1024];
        int pending_len = 0;
        int first_rx_ms = -1;
        int total_rx = 0;
        int reply_ms = -1;
        double start = boot_time_seconds();
        while (g_running) {
            int elapsed = (int)((boot_time_seconds() - start) * 1000.0);
            if (elapsed >= timeout_ms)
                break;
            int remain = timeout_ms - elapsed;
            struct timeval tv = {remain / 1000, (remain % 1000) * 1000};
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
                break;
            ssize_t count = read(fd, rx_buf, sizeof(rx_buf));
            if (count > 0) {
                if (first_rx_ms < 0)
                    first_rx_ms = (int)((boot_time_seconds() - start) * 1000.0);
                total_rx += (int)count;
                if (count > (ssize_t)(sizeof(pending) - pending_len))
                    pending_len = 0;
                int copy_len = count < (ssize_t)(sizeof(pending) - pending_len)
                                   ? (int)count
                                   : (int)(sizeof(pending) - pending_len);
                memcpy(pending + pending_len, rx_buf, copy_len);
                pending_len += copy_len;

                int offset = 0;
                while (offset < pending_len) {
                    int available = pending_len - offset;
                    uint8_t head = pending[offset];
                    int frame_total;
                    if (head == HEAD_REPLY) {
                        if (available < 3)
                            break;
                        frame_total = 5 + pending[offset + 2];
                    } else if (head == HEAD_REPORT) {
                        if (available < 2)
                            break;
                        frame_total = 2 + pending[offset + 1] + 1;
                    } else {
                        offset++;
                        continue;
                    }
                    if (frame_total <= 0 || frame_total > (int)sizeof(pending)) {
                        offset++;
                        continue;
                    }
                    if (available < frame_total)
                        break;
                    if (head == HEAD_REPLY && pending[offset + 1] == expected_command) {
                        reply_ms = (int)((boot_time_seconds() - start) * 1000.0);
                        offset += frame_total;
                        break;
                    }
                    offset += frame_total;
                }
                if (offset > 0) {
                    memmove(pending, pending + offset, (size_t)(pending_len - offset));
                    pending_len -= offset;
                }
                if (reply_ms >= 0)
                    break;
            }
        }
        printf("[启动] [RADAR_INIT] command=%s first_rx_ms=%d reply_ms=%d total_rx=%d\n", command,
               first_rx_ms, reply_ms, total_rx);
        return reply_ms >= 0;
    };
    bool writes_ok = true;
    unsigned int replies_seen = 0;
    flush_rx(fd);
    writes_ok = (send_cmd(fd, 7, 0x1E, NULL, 0) == 0) && writes_ok;
    replies_seen += wait_reply("group7_cmd1e", (uint8_t)((7 << 5) | 0x1E), 1000);
    usleep(200000);
    flush_rx(fd);
    {
        uint8_t p = 0x01;
        writes_ok = (send_cmd(fd, 6, 0x11, &p, 1) == 0) && writes_ok;
    }
    replies_seen += wait_reply("group6_cmd11", (uint8_t)((6 << 5) | 0x11), 1000);
    usleep(200000);
    flush_rx(fd);
    {
        uint8_t p = 0x00;
        writes_ok = (send_cmd(fd, 0, 0x02, &p, 1) == 0) && writes_ok;
    }
    replies_seen += wait_reply("group0_cmd02", 0x02, 1000);
    usleep(200000);
    flush_rx(fd);
    {
        uint8_t p[2] = {0x88, 0x13};
        writes_ok = (send_cmd(fd, 6, 0x12, p, 2) == 0) && writes_ok;
    }
    replies_seen += wait_reply("group6_cmd12", (uint8_t)((6 << 5) | 0x12), 1000);
    usleep(200000);
    printf("[RADAR] Setup writes_ok=%d matched_replies=%u/4\n", writes_ok, replies_seen);
    return writes_ok && replies_seen == 4 ? 0 : -1;
}
