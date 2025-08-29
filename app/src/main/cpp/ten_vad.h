//
// Copyright © 2025
// TEN VAD public API (optimized header, backward compatible)
//
#ifndef TEN_VAD_H
#define TEN_VAD_H

/* ================= Visibility ================ */
#if defined(__APPLE__) || defined(__ANDROID__) || defined(__linux__)
#  define TENVAD_API __attribute__((visibility("default")))
#elif defined(_WIN32) || defined(__CYGWIN__)
#  ifdef TENVAD_EXPORTS
#    define TENVAD_API __declspec(dllexport)
#  else
#    define TENVAD_API __declspec(dllimport)
#  endif
#else
#  define TENVAD_API
#endif

#include <stddef.h>  /* size_t */
#include <stdint.h>  /* int16_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Version & Defaults ================ */
#define TEN_VAD_VERSION_MAJOR 1
#define TEN_VAD_VERSION_MINOR 1
#define TEN_VAD_VERSION_PATCH 0

#define TEN_VAD_DEFAULT_SAMPLE_RATE   16000
#define TEN_VAD_DEFAULT_HOP_SIZE       256     /* 16ms @16k */
#define TEN_VAD_DEFAULT_THRESHOLD     0.25f    /* 概率阈值，越高越不敏感 */
#define TEN_VAD_DEFAULT_MIN_RMS     1200.0f    /* 能量门限，拦截远处小声 */
#define TEN_VAD_DEFAULT_ATTACK_FRAMES   3      /* 起声去抖 */
#define TEN_VAD_DEFAULT_RELEASE_FRAMES  8      /* 释声去抖 */

/* ================= Result / Error Codes ================ */
typedef enum ten_vad_result_e {
    TENVAD_OK                 = 0,
    TENVAD_ERR_GENERIC        = -1,
    TENVAD_ERR_BAD_ARG        = -2,
    TENVAD_ERR_BAD_STATE      = -3,
    TENVAD_ERR_UNSUPPORTED    = -4
} ten_vad_result_t;

/* ================= Opaque handle ================ */
typedef void* ten_vad_handle_t;

/* =============== Tunable parameters =============== */
typedef struct ten_vad_params_s {
    float threshold;      /* [0..1]  概率阈值：高→更不敏感 */
    float min_rms;        /* >=0     最低能量门限（短整型 RMS） */
    int   attack_frames;  /* >=1     连续语音帧数达到才认定“起声” */
    int   release_frames; /* >=1     连续静音帧数达到才认定“释声” */
} ten_vad_params_t;

/* =============== Runtime state (read-only) =============== */
typedef struct ten_vad_state_s {
    int  decision;   /* 0: 静音, 1: 语音（含迟滞后的稳定状态） */
    float avg_prob;  /* 最近一次 process 的平均概率（便于调参观察） */
} ten_vad_state_t;

/* ================= Basic API (kept for backward-compat) ================ */
/**
 * @brief Create and initialize a ten_vad instance (legacy signature).
 * @param[out] handle    returned handle
 * @param[in]  hop_size  recommended 256
 * @param[in]  threshold probability threshold [0..1]
 * @return TENVAD_OK(0) on success, negative on error
 */
TENVAD_API int ten_vad_create(ten_vad_handle_t *handle, size_t hop_size, float threshold);

/**
 * @brief Process one or multiple frames (length must be N*hop_size).
 * @param[in]  handle
 * @param[in]  audio_data         int16 mono buffer
 * @param[in]  audio_data_length  equals hop_size or k*hop_size
 * @param[out] out_probability    average probability over frames (can be NULL)
 * @param[out] out_flag           last frame’s binary decision (can be NULL)
 * @return TENVAD_OK(0) on success, negative on error
 */
TENVAD_API int ten_vad_process(ten_vad_handle_t handle,
                               const int16_t *audio_data,
                               size_t audio_data_length,
                               float *out_probability,
                               int   *out_flag);

/** @brief Destroy the instance; *handle set to NULL on return. */
TENVAD_API int ten_vad_destroy(ten_vad_handle_t *handle);

/** @brief Get version string like "1.1.0". */
TENVAD_API const char* ten_vad_get_version(void);

/* ================= Enhanced API (optional but recommended) ================ */
/**
 * @brief Create with full parameters (energy gate + hysteresis).
 *        If not implemented by your .so yet, you can forward to ten_vad_create()
 *        and keep internal defaults.
 */
TENVAD_API int ten_vad_create_with_params(ten_vad_handle_t *handle,
                                          size_t hop_size,
                                          const ten_vad_params_t *params);

/** @brief Change parameters at runtime (restarts internal state if needed). */
TENVAD_API int ten_vad_set_params(ten_vad_handle_t handle,
                                  const ten_vad_params_t *params);

/** @brief Read back current parameters (for UI/debug). */
TENVAD_API int ten_vad_get_params(ten_vad_handle_t handle,
                                  ten_vad_params_t *out_params);

/** @brief Reset internal hysteresis counters/state to “silence”. */
TENVAD_API int ten_vad_reset_state(ten_vad_handle_t handle);

/** @brief Get last stable decision & avg prob from the most recent process(). */
TENVAD_API int ten_vad_get_state(ten_vad_handle_t handle,
                                 ten_vad_state_t *out_state);

/**
 * @brief Validate IO assumptions to fail fast at call site.
 * @param[in] sample_rate must be 16000 for current implementation
 * @param[in] hop_size    frame hop in samples (recommended 256)
 * @param[in] length      input buffer length
 * @return TENVAD_OK if valid; error otherwise
 */
TENVAD_API int ten_vad_validate_io(int sample_rate, size_t hop_size, size_t length);

/* =============== Helper to fill defaults =============== */
static inline void ten_vad_params_fill_defaults(ten_vad_params_t *p) {
    if (!p) return;
    p->threshold      = TEN_VAD_DEFAULT_THRESHOLD;
    p->min_rms        = TEN_VAD_DEFAULT_MIN_RMS;
    p->attack_frames  = TEN_VAD_DEFAULT_ATTACK_FRAMES;
    p->release_frames = TEN_VAD_DEFAULT_RELEASE_FRAMES;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* TEN_VAD_H */