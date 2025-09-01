// TenVadJni.c
#include <jni.h>
#include <stdlib.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include "ten_vad.h"

#define LOG_TAG "TenVadJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const int HOP_SIZE = 256;   // 与 Java 侧保持一致

/* ========== 配置与状态 ========== */
typedef struct {
    float threshold;      // ten_vad 概率阈值 [0..1]
    float min_rms;        // 最低能量门限（短整型 RMS），过低直接判非语音（如 600~1500）
    int   attack_frames;  // 连续多少帧=1 才“起声”（去抖，2~5）
    int   release_frames; // 连续多少帧=0 才“释声”（去抖，4~12）
} vad_cfg_t;

typedef struct {
    int speech_run;       // 连续语音帧计数
    int silence_run;      // 连续静音帧计数
    int state;            // 0=静音，1=语音（迟滞后的稳定状态）
} vad_state_t;

/* ========== 全局对象（单实例） ========== */
static ten_vad_handle_t g_handle = NULL;
static vad_cfg_t  g_cfg  = { .threshold = 0.20f, .min_rms = 1200.f, .attack_frames = 3, .release_frames = 8 };
static vad_state_t g_st  = { 0, 0, 0 };

/* 动态噪声底（用于能量门限自适应） */
static float g_noise_ewma  = 300.f;  // 初始噪声底（按设备调）
static float g_noise_alpha = 0.02f;  // 噪声底跟踪速率（0.01~0.05）
static float g_noise_mul   = 1.60f;  // 动态门限倍数（1.3~2.0）

/* 互斥锁：保护 g_handle/g_cfg/g_st/g_noise_* */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static inline void mu_lock()   { pthread_mutex_lock(&g_mu); }
static inline void mu_unlock() { pthread_mutex_unlock(&g_mu); }

/* ========== 工具函数 ========== */
static inline float frame_rms(const int16_t *buf, int len) {
    long long acc = 0;
    for (int i = 0; i < len; ++i) {
        int s = buf[i];
        acc += (long long)s * s;
    }
    if (len <= 0) return 0.f;
    return (float)sqrt((double)acc / (double)len);
}

/* 对“送入 VAD 的帧”做预加重（不改原 pcm） */
static inline void pre_emphasis_0_97(int16_t* dst, const int16_t* src, int len) {
    float prev = 0.f;
    for (int i = 0; i < len; ++i) {
        float x = (float)src[i];
        float y = x - 0.97f * prev;
        prev = x;
        if (y > 32767.f) y = 32767.f;
        if (y < -32768.f) y = -32768.f;
        dst[i] = (int16_t)y;
    }
}

/* ========== JNI 导出 ========== */

JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_create(JNIEnv *env, jobject thiz) {
    mu_lock();
    if (g_handle) {
        ten_vad_destroy(&g_handle);
    }
    g_handle = NULL;
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    // 可选：重置噪声底
    // g_noise_ewma = 300.f;
    LOGD("create(): lazy handle; cfg(th=%.2f, minRms=%.1f, att=%d, rel=%d)",
         g_cfg.threshold, g_cfg.min_rms, g_cfg.attack_frames, g_cfg.release_frames);
    mu_unlock();
}

JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_free(JNIEnv *env, jobject thiz) {
    mu_lock();
    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
    }
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    LOGD("free(): VAD freed");
    mu_unlock();
}

JNIEXPORT jint JNICALL
Java_com_wzc_vad_VadUtils_init(JNIEnv *env, jobject thiz, jfloat threshold) {
    mu_lock();
    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
    }
    g_cfg.threshold = threshold; // 兼容旧接口
    int ret = ten_vad_create(&g_handle, HOP_SIZE, g_cfg.threshold);
    if (ret != 0 || !g_handle) {
        LOGE("init(): ten_vad_create failed ret=%d", ret);
        mu_unlock();
        return -1;
    }
    g_st.speech_run = g_st.silence_run = g_st.state = 0;
    LOGD("init(): ok, threshold=%.2f", g_cfg.threshold);
    mu_unlock();
    return 0;
}

/* 旧接口：一次性设置粗参数（并重建底层阈值） */
JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_setParams(JNIEnv *env, jobject thiz,
                                    jfloat threshold,
                                    jfloat minRms,
                                    jint attackFrames,
                                    jint releaseFrames) {
    mu_lock();
    g_cfg.threshold      = (threshold < 0.f) ? 0.f : (threshold > 1.f ? 1.f : threshold);
    g_cfg.min_rms        = (minRms < 0.f) ? 0.f : minRms;
    g_cfg.attack_frames  = (attackFrames  < 1) ? 1 : attackFrames;
    g_cfg.release_frames = (releaseFrames < 1) ? 1 : releaseFrames;
    LOGD("setParams(): th=%.2f, minRms=%.1f, attack=%d, release=%d",
         g_cfg.threshold, g_cfg.min_rms, g_cfg.attack_frames, g_cfg.release_frames);

    if (g_handle) {
        ten_vad_destroy(&g_handle);
        g_handle = NULL;
        int ret = ten_vad_create(&g_handle, HOP_SIZE, g_cfg.threshold);
        if (ret != 0 || !g_handle) {
            LOGE("setParams(): re-create ten_vad failed ret=%d", ret);
        }
        g_st.speech_run = g_st.silence_run = g_st.state = 0;
    }
    mu_unlock();
}

/* 新增：设置噪声底自适应参数（可选） */
JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_setNoiseParams(JNIEnv* env, jobject thiz,
                                         jfloat ewmaInit, jfloat alpha, jfloat mul) {
    mu_lock();
    if (ewmaInit > 0) g_noise_ewma = ewmaInit;
    if (alpha > 0 && alpha < 1) g_noise_alpha = alpha;
    if (mul > 0) g_noise_mul = mul;
    LOGD("setNoiseParams(): noise_ewma=%.1f alpha=%.3f mul=%.2f",
         g_noise_ewma, g_noise_alpha, g_noise_mul);
    mu_unlock();
}

/* 新增：仅清零迟滞状态（会话切换/开始外放时可调用） */
JNIEXPORT void JNICALL
Java_com_wzc_vad_VadUtils_resetState(JNIEnv* env, jobject thiz) {
    mu_lock();
    g_st.speech_run = g_st.silence_run = 0;
    g_st.state = 0;
    // 可选：把噪声底恢复初值，避免被上一轮讲话拉高
    // g_noise_ewma = 300.f;
    LOGD("resetState(): cleared hysteresis state");
    mu_unlock();
}

JNIEXPORT jint JNICALL
Java_com_wzc_vad_VadUtils_process(JNIEnv *env, jobject thiz, jint fs,
                                  jshortArray buffer, jint length) {
    if (fs != 16000) {
        LOGE("process(): only support 16k, got=%d", fs);
        return -1;
    }
    if (length < HOP_SIZE || (length % HOP_SIZE) != 0) {
        LOGE("process(): invalid len=%d (must be multiple of %d)", length, HOP_SIZE);
        return -1;
    }

    mu_lock();
    if (!g_handle) {
        LOGE("process(): not initialized");
        mu_unlock();
        return -1;
    }

    jboolean isCopy = JNI_FALSE;
    jshort *pcm = (*env)->GetPrimitiveArrayCritical(env, buffer, &isCopy);
    if (!pcm) {
        LOGE("process(): GetPrimitiveArrayCritical failed");
        mu_unlock();
        return -1;
    }

    int frames = length / HOP_SIZE;
    int speech_frames = 0;
    float prob_sum = 0.f;

    for (int i = 0; i < frames; ++i) {
        const int16_t *frm = pcm + i * HOP_SIZE;

        // 1) 对送入 VAD 的数据做预加重
        int16_t tmp[HOP_SIZE];
        pre_emphasis_0_97(tmp, frm, HOP_SIZE);

        // 2) 跑 ten_vad
        float prob = 0.f;
        int   flag = 0;
        int ret = ten_vad_process(g_handle, tmp, HOP_SIZE, &prob, &flag);
        if (ret != 0) {
            (*env)->ReleasePrimitiveArrayCritical(env, buffer, pcm, JNI_ABORT);
            LOGE("process(): ten_vad_process failed at i=%d", i);
            mu_unlock();
            return -1;
        }

        // 3) 能量（原始帧 RMS）
        float frms = frame_rms(frm, HOP_SIZE);

        // 4) 动态能量门限：max(固定下限, 噪声底×倍数)
        float dyn_gate = fmaxf(g_cfg.min_rms, g_noise_ewma * g_noise_mul);

        // 5) “概率或 flag” + “能量达标”的双条件
        int vad_ok    = (flag != 0) || (prob >= (g_cfg.threshold + 0.10f)); // 稍放松
        int is_speech = (vad_ok && frms >= dyn_gate) ? 1 : 0;

        // 6) 只在“非语音”时更新噪声底，避免把讲话混入
        if (!is_speech) {
            g_noise_ewma = (1.f - g_noise_alpha) * g_noise_ewma + g_noise_alpha * frms;
        }

        // 7) 迟滞去抖
        if (is_speech) {
            g_st.speech_run++;
            g_st.silence_run = 0;
        } else {
            g_st.silence_run++;
            g_st.speech_run = 0;
        }

        if (g_st.state == 0) {
            if (g_st.speech_run >= g_cfg.attack_frames) g_st.state = 1;
        } else {
            if (g_st.silence_run >= g_cfg.release_frames) g_st.state = 0;
        }

        if (is_speech) speech_frames++;
        prob_sum += prob;
    }

    (*env)->ReleasePrimitiveArrayCritical(env, buffer, pcm, JNI_ABORT);

    float avgProb = prob_sum / (float)frames;
    LOGD("process(): frames=%d speech=%d avgProb=%.2f state=%d noise=%.1f gate=%.1f(min=%.1f×%.2f)",
         frames, speech_frames, avgProb, g_st.state,
         g_noise_ewma, fmaxf(g_cfg.min_rms, g_noise_ewma * g_noise_mul),
         g_noise_ewma, g_noise_mul);

    int out_state = g_st.state; // 0/1：迟滞后的整体状态
    mu_unlock();
    return out_state;
}

JNIEXPORT jboolean JNICALL
Java_com_wzc_vad_VadUtils_validRateAndFrameLength(JNIEnv *env, jobject thiz,
                                                  jint fs, jint length) {
    return (fs == 16000 && length >= HOP_SIZE && (length % HOP_SIZE) == 0)
           ? JNI_TRUE : JNI_FALSE;
}