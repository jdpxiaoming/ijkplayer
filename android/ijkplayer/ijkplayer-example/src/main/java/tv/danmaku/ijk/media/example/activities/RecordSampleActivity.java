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
    private TextView mStatusTextView;
    private Button mRecordButton;
    private boolean isRecording = false;
    private String mRecordFilePath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_record_sample);

        mVideoView = findViewById(R.id.video_view);
        mStatusTextView = findViewById(R.id.status_text);
        mRecordButton = findViewById(R.id.record_button);

        // 设置默认URL，这里使用的是一个直播流URL
        String url = "your_rtmp_stream_url";
        mVideoView.setVideoPath(url);
        
        // 初始化播放器设置
        IjkMediaPlayer.loadLibrariesOnce(null);
        IjkMediaPlayer.native_profileBegin("libijkplayer.so");
        
        // 开始播放
        mVideoView.start();

        // 设置录制按钮的点击事件
        mRecordButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggleRecording();
            }
        });
    }

    private void toggleRecording() {
        IjkMediaPlayer player = (IjkMediaPlayer) mVideoView.getMediaPlayer();
        if (player == null) {
            Toast.makeText(this, "播放器未初始化", Toast.LENGTH_SHORT).show();
            return;
        }

        if (isRecording) {
            // 停止录制
            int ret = player.stopRecord();
            if (ret == 0) {
                isRecording = false;
                mRecordButton.setText("开始录制");
                mStatusTextView.setText("已停止录制：" + mRecordFilePath);
                Toast.makeText(this, "录制已停止", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "停止录制失败：" + ret, Toast.LENGTH_SHORT).show();
            }
        } else {
            // 开始录制
            File recordDir = new File(Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_MOVIES), "IjkRecords");
            if (!recordDir.exists() && !recordDir.mkdirs()) {
                Toast.makeText(this, "无法创建录制目录", Toast.LENGTH_SHORT).show();
                return;
            }

            String timestamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
                    .format(new Date());
            mRecordFilePath = new File(recordDir, "ijkrecord_" + timestamp + ".mp4").getAbsolutePath();
            
            int ret = player.startRecord(mRecordFilePath);
            if (ret == 0) {
                isRecording = true;
                mRecordButton.setText("停止录制");
                mStatusTextView.setText("录制中...");
                Toast.makeText(this, "开始录制", Toast.LENGTH_SHORT).show();
            } else {
                Toast.makeText(this, "开始录制失败：" + ret, Toast.LENGTH_SHORT).show();
            }
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        // 如果在录制，暂停时停止录制
        if (isRecording) {
            IjkMediaPlayer player = (IjkMediaPlayer) mVideoView.getMediaPlayer();
            if (player != null) {
                player.stopRecord();
            }
            isRecording = false;
            mRecordButton.setText("开始录制");
            mStatusTextView.setText("已停止录制：" + mRecordFilePath);
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        // 释放播放器资源
        mVideoView.stopPlayback();
        mVideoView.release(true);
        IjkMediaPlayer.native_profileEnd();
    }
} 