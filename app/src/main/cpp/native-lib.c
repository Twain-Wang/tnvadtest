#include <jni.h>
#include <stdlib.h>
#include <android/log.h>
#include <math.h>
#include "ten_vad.h"

#define LOG_TAG "TenVadJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const int HOP_SIZE = 256;

/* ------- 可调参数 + 状态 ------- */
typedef struct {
    float threshold;     // ten_vad 概率阈值 [0..1]
    float min_rms;       // 最低能量门限（短整型幅度），低于此视为非语音，如 800~2000
    int   attack_frames; // 连续多少帧=1 才“起声”（去抖：2~5）
    int   release_frames;// 连续多少帧=0 才“释声”（去抖：4~12）
} vad_cfg_t;

typedef struct {
    int speech_run;      // 连续语音帧计数
    int silence_run;     // 连续静音帧计数
    int state;           // 0=静音，1=语音（迟滞后的状态）
} vad_state_t;

/* ------- 全局句柄（单实例） ------- */
static ten_vad_handle_t g_handle = NULL;
static vad_cfg_t  g_cfg  = { .threshold = 0.20f, .min_rms = 1200.f, .attack_frames = 3, .release_frames = 8 };
static vad_state_t g_st  = { 0, 0, 0 };

static inline float frame_rms(const int16_t *buf, int len) {
    long long acc = 0;
    for (int i = 0; i < len; ++i) {
        int s = buf[i];
        acc += (long long)s * s;
    }
    if (len <= 0) return 0.f;
    return (float)sqrt((double)acc / (double)len);
}

/* ========== JNI ========== */

JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_create(JNIEnv *env, jobject thiz) {
    if (g_handle) {
        ten_vad_destroy(&g_handle);
    }
    g_handle = NULL;
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    LOGD("create(): VAD lazy handle; cfg(th=%.2f, minRms=%.1f, att=%d, rel=%d)",
         g_cfg.threshold, g_cfg.min_rms, g_cfg.attack_frames, g_cfg.release_frames);
}

JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_free(JNIEnv *env, jobject thiz) {
    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
    }
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    LOGD("free(): VAD freed");
}

JNIEXPORT jint JNICALL
Java_com_wzc_vad_VadUtils_init(JNIEnv *env, jobject thiz, jfloat threshold) {
    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
    }
    g_cfg.threshold = threshold; // 仍然传入以兼容旧调用
    int ret = ten_vad_create(&g_handle, HOP_SIZE, g_cfg.threshold);
    if (ret != 0 || !g_handle) {
        LOGE("init(): ten_vad_create failed ret=%d", ret);
        return -1;
    }
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    LOGD("init(): ok, threshold=%.2f", g_cfg.threshold);
    return 0;
}

/* 一次性设置更细的参数 */
JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_setParams(JNIEnv *env, jobject thiz,
                                    jfloat threshold,
                                    jfloat minRms,
                                    jint attackFrames,
                                    jint releaseFrames) {
    g_cfg.threshold      = (threshold < 0.f) ? 0.f : (threshold > 1.f ? 1.f : threshold);
    g_cfg.min_rms        = (minRms < 0.f) ? 0.f : minRms;
    g_cfg.attack_frames  = (attackFrames < 1) ? 1 : attackFrames;
    g_cfg.release_frames = (releaseFrames < 1) ? 1 : releaseFrames;
    LOGD("setParams(): th=%.2f, minRms=%.1f, attack=%d, release=%d",
         g_cfg.threshold, g_cfg.min_rms, g_cfg.attack_frames, g_cfg.release_frames);
    /* 注意：ten_vad 的阈值在 create 时生效；如需同时更新阈值到底层，可重建句柄 */
    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
        int ret = ten_vad_create(&g_handle, HOP_SIZE, g_cfg.threshold);
        if (ret != 0 || !g_handle) {
            LOGE("setParams(): re-create ten_vad failed ret=%d", ret);
        }
        g_st.speech_run = g_st.silence_run = g_st.state = 0;
    }
}

JNIEXPORT jint JNICALL
Java_com_wzc_vad_VadUtils_process(JNIEnv *env, jobject thiz, jint fs,
                                  jshortArray buffer, jint length) {
    if (!g_handle) {
        LOGE("process(): not initialized");
        return -1;
    }
    if (fs != 16000) {
        LOGE("process(): only support 16k, got=%d", fs);
        return -1;
    }
    if (length < HOP_SIZE) {
        LOGE("process(): too small, len=%d", length);
        return -1;
    }

    jboolean isCopy = JNI_FALSE;
    jshort *pcm = (*env)->GetPrimitiveArrayCritical(env, buffer, &isCopy);
    if (!pcm) {
        LOGE("process(): GetPrimitiveArrayCritical failed");
        return -1;
    }

    int frames = length / HOP_SIZE;
    int speech_frames = 0;
    float prob_sum = 0.f;

    for (int i = 0; i < frames; ++i) {
        const int16_t *frm = pcm + i * HOP_SIZE;

        float prob = 0.f;
        int   flag = 0;
        int ret = ten_vad_process(g_handle, frm, HOP_SIZE, &prob, &flag);
        if (ret != 0) {
            (*env)->ReleasePrimitiveArrayCritical(env, buffer, pcm, JNI_ABORT);
            LOGE("process(): ten_vad_process failed at i=%d", i);
            return -1;
        }

        /* 能量门限：太小直接判非语音（拦截远处小声/环境声） */
        float frms = frame_rms(frm, HOP_SIZE);
        if (frms < g_cfg.min_rms) {
            flag = 0;
        }

        /* 统计 + 迟滞 */
        if (flag) {
            g_st.speech_run++;
            g_st.silence_run = 0;
        } else {
            g_st.silence_run++;
            g_st.speech_run = 0;
        }

        if (g_st.state == 0) { // 当前静音 → 只有达到 attack 才切到“语音”
            if (g_st.speech_run >= g_cfg.attack_frames) {
                g_st.state = 1;
            }
        } else {               // 当前语音 → 只有达到 release 才切回“静音”
            if (g_st.silence_run >= g_cfg.release_frames) {
                g_st.state = 0;
            }
        }

        if (flag) speech_frames++;
        prob_sum += prob;
    }

    (*env)->ReleasePrimitiveArrayCritical(env, buffer, pcm, JNI_ABORT);

    float avgProb = prob_sum / (float)frames;
    LOGD("process(): frames=%d speech=%d avgProb=%.2f state=%d",
         frames, speech_frames, avgProb, g_st.state);

    /* 返回迟滞后的整体状态：更稳定 */
    return g_st.state;
}

JNIEXPORT jboolean JNICALL
Java_com_wzc_vad_VadUtils_validRateAndFrameLength(JNIEnv *env, jobject thiz,
                                                  jint fs, jint length) {
    return (fs == 16000 && length >= HOP_SIZE && length % HOP_SIZE == 0) ? JNI_TRUE : JNI_FALSE;
}