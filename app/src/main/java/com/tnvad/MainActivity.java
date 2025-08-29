package com.tnvad;

import android.Manifest;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.Handler;
import android.os.Message;


import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.RequiresPermission;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;

import com.wzc.vad.VadUtils;

public class MainActivity extends AppCompatActivity implements View.OnClickListener, Handler.Callback {
    private static final String TAG = "MainActivity";
    private VadUtils mVad;
    private boolean isRecording;
    private TextView mStatusView;
    private Handler mHandler = new Handler(this);
    private static final int PERMISSION_REQUEST_RECORD_AUDIO = 100; // 权限请求码

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        mVad = new VadUtils();
        mVad.create();
        mVad.init(0.5f);
        //mVad.setMode(3);
        initView();
        checkAndRequestPermissions(); // 单独抽取权限检查方法
    }

    // 检查并请求录音权限
    private void checkAndRequestPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) { // 仅在6.0+需要动态请求
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                // 请求权限
                ActivityCompat.requestPermissions(
                        this,
                        new String[]{Manifest.permission.RECORD_AUDIO},
                        PERMISSION_REQUEST_RECORD_AUDIO
                );
            }
        }
    }

    // 处理权限请求结果
    @Override
    public void onRequestPermissionsResult(
            int requestCode,
            @NonNull String[] permissions,
            @NonNull int[] grantResults
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == PERMISSION_REQUEST_RECORD_AUDIO) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "录音权限已授予", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "请授予录音权限，否则无法使用录音功能", Toast.LENGTH_LONG).show();
            }
        }
    }

    private void initView() {
        mStatusView = findViewById(R.id.tv_status);
        Button btn_create = findViewById(R.id.btn_create);
        Button btn_free = findViewById(R.id.btn_free);
        Button btn_init = findViewById(R.id.btn_init);
        Button btn_setMode = findViewById(R.id.btn_setMode);
        Button btn_process = findViewById(R.id.btn_process);
        Button btn_stop = findViewById(R.id.btn_stopRecord);

        btn_create.setOnClickListener(this);
        btn_free.setOnClickListener(this);
        btn_init.setOnClickListener(this);
        btn_setMode.setOnClickListener(this);
        btn_process.setOnClickListener(this);
        btn_stop.setOnClickListener(this);
    }

    @Override
    public void onClick(View v) {
        int viewId = v.getId();

        if (viewId == R.id.btn_create) {
            mVad.create();
        } else if (viewId == R.id.btn_free) {
            mVad.free();
        } else if (viewId == R.id.btn_init) {
            int initStatus = mVad.init(0.5f);
            Log.d(TAG, "init status: " + initStatus);
        } else if (viewId == R.id.btn_setMode) {
            int setModeStatus =0;// mVad.setMode(3);
            Log.d(TAG, "set mode status: " + setModeStatus);
        } else if (viewId == R.id.btn_process) {
            // 检查录音权限是否已授予
            if (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                Toast.makeText(this, "请先授予录音权限", Toast.LENGTH_SHORT).show();
                return;
            }
            isRecording = true;
            new Thread(new Runnable() {
                @RequiresPermission(Manifest.permission.RECORD_AUDIO)
                @Override
                public void run() {
                    startRecord();
                }
            }).start();
        } else if (viewId == R.id.btn_stopRecord) {
            try {
                Thread.sleep(1000);
            } catch (InterruptedException e) {
                e.printStackTrace();
            }
            isRecording = false;
        }
    }

    @RequiresPermission(Manifest.permission.RECORD_AUDIO)
    private void startRecord() {
        final  int frequency = 16000; // 采样率
        int channelConfiguration = AudioFormat.CHANNEL_IN_MONO;
        int audioEncoding = AudioFormat.ENCODING_PCM_16BIT;

        final int frm_len = frequency/1000*20;

        // 1. 计算最小缓冲区大小
        int bufferSize = AudioRecord.getMinBufferSize(frequency, channelConfiguration, audioEncoding);
        if (bufferSize == AudioRecord.ERROR || bufferSize == AudioRecord.ERROR_BAD_VALUE) {
            Log.e(TAG, "不支持的音频参数，计算缓冲区大小失败");
            return;
        }
        Log.d(TAG,"bufferSize:"+bufferSize);
        // 2. 创建AudioRecord并检查初始化状态
        AudioRecord audioRecord = new AudioRecord(
                MediaRecorder.AudioSource.MIC,
                frequency,
                channelConfiguration,
                audioEncoding,
                bufferSize
        );

        // 关键：检查AudioRecord是否初始化成功
        if (audioRecord.getState() != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord初始化失败，状态：" + audioRecord.getState());
            audioRecord.release(); // 释放资源
            return;
        }

        // 3. 开始录音
        try {
            audioRecord.startRecording();
        } catch (IllegalStateException e) {
            Log.e(TAG, "startRecording失败：" + e.getMessage());
            audioRecord.release();
            return;
        }

        short[] buffer = new short[frm_len];
        while (isRecording) {
            // 读取音频数据（注意：read可能返回错误码，需处理）
            int readSize = audioRecord.read(buffer, 0, buffer.length);
            if (readSize == AudioRecord.ERROR_INVALID_OPERATION || readSize == AudioRecord.ERROR_BAD_VALUE) {
                Log.e(TAG, "读取音频数据失败");
                break;
            }
            int isDetect = mVad.process(frequency, buffer, 256);
            Log.d(TAG, "VAD检测结果：" + isDetect);
            Message message = mHandler.obtainMessage();
            message.what = 0x01;
            message.obj = isDetect == 1 ? "Active Voice..." : "Non-active Voice...";
            mHandler.sendMessage(message);
        }

        // 4. 停止并释放资源
        try {
            audioRecord.stop();
        } catch (IllegalStateException e) {
            Log.e(TAG, "stop失败：" + e.getMessage());
        }
        audioRecord.release();
    }

    @Override
    public boolean handleMessage(Message message) {
        if (message.what == 0x01) {
            String obj = (String) message.obj;
            mStatusView.setText(obj);
        }
        return true;
    }
}