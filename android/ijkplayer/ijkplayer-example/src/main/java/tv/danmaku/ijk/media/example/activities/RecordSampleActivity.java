/*
 * Copyright (C) 2024 Bilibili
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package tv.danmaku.ijk.media.example.activities;

import android.os.Bundle;
import android.os.Environment;
import android.support.v7.app.AppCompatActivity;
import android.view.View;
import android.widget.Button;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

import tv.danmaku.ijk.media.example.R;
import tv.danmaku.ijk.media.example.widget.media.IjkVideoView;
import tv.danmaku.ijk.media.player.IjkMediaPlayer;

public class RecordSampleActivity extends AppCompatActivity {

    private IjkVideoView mVideoView;
    private Button mStartRecordButton;
    private Button mStopRecordButton;
    private TextView mRecordStatusText;
    private TextView mRecordPathText;
    private RadioGroup mRecordModeGroup;
    private RadioButton mDirectRecordRadio;
    private RadioButton mTranscodeRecordRadio;

    private boolean isRecording = false;
    private String mRecordPath;
    private boolean mUseTranscode = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_record_sample);

        mVideoView = findViewById(R.id.video_view);
        mStartRecordButton = findViewById(R.id.btn_start_record);
        mStopRecordButton = findViewById(R.id.btn_stop_record);
        mRecordStatusText = findViewById(R.id.text_record_status);
        mRecordPathText = findViewById(R.id.text_record_path);
        mRecordModeGroup = findViewById(R.id.radio_group_record_mode);
        mDirectRecordRadio = findViewById(R.id.radio_direct_record);
        mTranscodeRecordRadio = findViewById(R.id.radio_transcode_record);

        // 设置直播URL
        String url = getIntent().getStringExtra("videoPath");
        if (url == null) {
            // 默认使用RTSP测试流
            url = "rtsp://wowzaec2demo.streamlock.net/vod/mp4:BigBuckBunny_115k.mp4";
        }
        mVideoView.setVideoPath(url);
        mVideoView.start();

        // 设置录制模式选择
        mRecordModeGroup.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(RadioGroup group, int checkedId) {
                mUseTranscode = (checkedId == R.id.radio_transcode_record);
            }
        });

        // 开始录制按钮
        mStartRecordButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startRecord();
            }
        });

        // 停止录制按钮
        mStopRecordButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopRecord();
            }
        });

        updateUI();
    }

    private void startRecord() {
        if (isRecording) {
            Toast.makeText(this, "已经在录制中", Toast.LENGTH_SHORT).show();
            return;
        }

        // 创建录制文件路径
        File recordDir = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_MOVIES), "IjkRecords");
        if (!recordDir.exists()) {
            recordDir.mkdirs();
        }

        SimpleDateFormat sdf = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault());
        String fileName = "record_" + sdf.format(new Date()) + ".mp4";
        mRecordPath = new File(recordDir, fileName).getAbsolutePath();

        // 获取IjkMediaPlayer实例
        IjkMediaPlayer player = mVideoView.getIjkMediaPlayer();
        if (player != null) {
            int result;
            if (mUseTranscode) {
                // 使用转码录制
                result = player.startRecordTranscode(mRecordPath);
            } else {
                // 直接录制
                result = player.startRecord(mRecordPath);
            }
            
            if (result == 0) {
                isRecording = true;
                Toast.makeText(this, "开始录制", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "录制失败: " + result, Toast.LENGTH_SHORT).show();
            }
        } else {
            Toast.makeText(this, "播放器未就绪", Toast.LENGTH_SHORT).show();
        }

        updateUI();
    }

    private void stopRecord() {
        if (!isRecording) {
            Toast.makeText(this, "没有正在进行的录制", Toast.LENGTH_SHORT).show();
            return;
        }

        IjkMediaPlayer player = mVideoView.getIjkMediaPlayer();
        if (player != null) {
            int result = player.stopRecord();
            if (result == 0) {
                isRecording = false;
                Toast.makeText(this, "录制已停止，文件保存在: " + mRecordPath, Toast.LENGTH_LONG).show();
            } else {
                Toast.makeText(this, "停止录制失败: " + result, Toast.LENGTH_SHORT).show();
            }
        }

        updateUI();
    }

    private void updateUI() {
        mStartRecordButton.setEnabled(!isRecording);
        mStopRecordButton.setEnabled(isRecording);
        mRecordStatusText.setText(isRecording ? "录制中..." : "未录制");
        mRecordPathText.setText(isRecording ? "保存路径: " + mRecordPath : "");
        mRecordModeGroup.setEnabled(!isRecording);
    }

    @Override
    protected void onDestroy() {
        if (isRecording) {
            stopRecord();
        }
        if (mVideoView != null) {
            mVideoView.stopPlayback();
            mVideoView.release(true);
        }
        super.onDestroy();
    }
} 