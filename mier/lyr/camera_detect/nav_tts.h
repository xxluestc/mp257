/**
 * nav_tts.h - HUD 导航语音播报接口
 *
 * 功能：
 *   1. 在 UDP 8888 端口接收 APP 发送的导航 JSON
 *   2. 保留原有 navi / navi_tts / alert 处理逻辑
 *   3. 新增 danger_tts 接收与解析
 *   4. preload 阶段只记录，trigger 阶段把最终 text 交给上层接口
 *
 * APP 发送格式：
 *   {"type":"navi_tts","tts_type":1,"seq":12,"text":"前方100米右转进入人民路"}
 *   {"type":"danger_tts","phase":"preload","alert_type":"其他异常","text":"前方停车场出口，请减速观察","distance":67}
 *   {"type":"danger_tts","phase":"trigger","alert_type":"其他异常","text":"前方14米停车场出口，请减速观察","distance":14}
 */

#ifndef NAV_TTS_H
#define NAV_TTS_H

#ifdef __cplusplus
extern "C" {
#endif

#define NAV_UDP_PORT        8888
#define NAV_TTS_TEXT_MAX    1024

/**
 * @brief 启动导航 UDP 接收线程
 * @return 0 成功，-1 失败
 */
int nav_tts_start(void);

/**
 * @brief 停止导航 UDP 接收线程
 */
void nav_tts_stop(void);

/**
 * @brief 直接播报一段导航文字（也可从主程序调用）
 * @param text UTF-8 编码的中文导航文案
 */
void nav_tts_speak(const char *text);

/**
 * @brief 播报异常路况文字，优先完整 TTS，无网络时按关键词播放本地固定提示音
 * @param text UTF-8 编码的中文异常路况文案
 */
void nav_tts_speak_danger(const char *text);


#define DANGER_TTS_PHASE_PRELOAD  "preload"
#define DANGER_TTS_PHASE_TRIGGER  "trigger"

/**
 * @brief 异常路况最终文本处理函数。
 *
 * nav_tts.c 已经完成 phase 判断：
 * - preload：只解析和打印日志，不调用处理函数；
 * - trigger：把手机端生成好的完整 text 交给处理函数。
 *
 * text 指针只在本次调用期间有效。处理函数需要立即复制文本，
 * 或把文本提交到会自行复制内容的异步音频队列。
 */
typedef void (*DangerTextHandler)(const char *text);

/**
 * @brief 注册或取消异常路况文本处理函数。
 * @param handler 骨传导文字输出函数；传 NULL 表示取消注册。
 *
 * 建议在 nav_tts_start() 之前调用一次。
 */
void nav_tts_set_danger_text_handler(DangerTextHandler handler);

#ifdef __cplusplus
}
#endif

#endif /* NAV_TTS_H */