package com.wzc.vad;

public class VadUtils {
    static {
        System.loadLibrary("ten_vad_jni"); // 对应C库名称
    }

    // 创建VAD实例（内部初始化ten_vad_handle）
    public native void create();

    // 销毁VAD实例（释放资源）
    public native void free();

    // 初始化VAD（可设置参数，如置信度阈值）
    public native int init(float voiceThreshold);

    // 核心处理方法：输入PCM数据，返回VAD结果
    // fs：采样率（ten_vad支持16000Hz，需明确限制）
    // buffer：16位PCM数据（单声道）
    // length：数据长度（单位：样本数，需为hop_size的整数倍，即256的倍数）
    public native int process(int fs, short[] buffer, int length);

    // 检查采样率和帧长是否合法（ten_vad要求16000Hz，每帧256样本）
    public native boolean validRateAndFrameLength(int fs, int length);
}