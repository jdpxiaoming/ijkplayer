# IJKPlayer H.265 / FLV 回放卡顿与 0 延迟丢帧问题排查与修复技术报告

**日期**：2026-08-21  
**项目**：IJKPlayer Android (H.265 / FLV 流媒体播放与回放)  
**涉及模块**：`ijkmedia/ijkplayer` (`ff_ffplay.c`, `ffpipenode_android_mediacodec_vdec.c`), Java 播放器配置层  

---

## 目录
1. [问题背景与现象](#1-问题背景与现象)
2. [核心问题根因深度分析](#2-核心问题根因深度分析)
   - [问题一：4x/16x 高倍速回放画面定格](#问题一4x16x-高倍速回放画面定格)
   - [问题二：魔改 FLV H.265 画面每 4~25 秒才跳一帧（致命问题）](#问题二魔改-flv-h265-画面每-425-秒才跳一帧致命问题)
   - [问题三：网络拉流饥饿（1316 字节缓冲区阻塞）](#问题三网络拉流饥饿1316-字节缓冲区阻塞)
   - [问题四：上层过早触发 `stopPlayback()` 中断握手](#问题四上层过早触发-stopplayback-中断握手)
3. [修复方案与代码实现](#3-修复方案与代码实现)
   - [C 底层引擎修复（Commit: `d37f3205`）](#c-底层引擎修复commit-d37f3205)
   - [Java / Kotlin 业务层参数与逻辑优化](#java--kotlin-业务层参数与逻辑优化)
4. [编译与多架构验证](#4-编译与多架构验证)
5. [最佳实践与参数配置推荐表](#5-最佳实践与参数配置推荐表)

---

## 1. 问题背景与现象

在 Android 客户端使用 IJKPlayer 播放 / 回放魔改 FLV (H.265 编码、1080P、无音频轨) 监控录像流时，出现以下异常现象：
* **现象 1**：在 4 倍速或 16 倍速回放时，时间轴正常快进，但视频画面彻底卡死。
* **现象 2**：同一条视频流使用 Mac 电脑端 `ffplay 8.1` 播放非常流畅连续，但在手机端 IJKPlayer 播放时，**画面卡死 4~25 秒才跳动一帧**（跳动周期与摄像机的 GOP 关键帧周期一致）。
* **现象 3**：关闭抓包环境后偶现起播报错 `AVERROR_EXIT`，起播立即被销毁。

---

## 2. 核心问题根因深度分析

### 问题一：4x/16x 高倍速回放画面定格
* **根因 1：H.265 硬解未启用，回退 CPU 软解崩溃**  
  服务端返回 `"vencodingName":"H265"`，但上层业务传入 `isH265: false`，导致未下发 `mediacodec-hevc = 1`。播放器降级为 `avcodec (hevc)` CPU 软解。在 4x (100fps) / 16x (400fps) 下，手机 CPU 算力无法支撑 1080P 软解，造成解码严重积压、丢包、报错 `Could not find ref with POC` 并卡死。
* **根因 2：无音频流时音视频同步（AVSync）时钟膨胀**  
  回放流无音频轨 (`audio_stream = -1`)，播放器主时钟回退到 `AV_SYNC_EXTERNAL_CLOCK`（外部系统物理时钟，1.0x 走速）。服务端以 4x/16x 速率推送视频包，视频 PTS 超前于系统时钟，`compute_target_delay` 计算出巨大的延时 `delay = delay + diff`，导致渲染线程 `video_refresh` 陷入休眠等待。

---

### 问题二：魔改 FLV H.265 画面每 4~25 秒才跳一帧（致命问题）
* **根因：0 延迟模式（`delay_forbidden = 1`）与 `framedrop` 逻辑冲突**
  1. 在 `ffplay_video_thread` 中，0 延迟模式下将每帧时长硬编码为固定值：
     ```c
     duration = 0.01; // 硬编码 10ms (0.01s)
     ```
  2. 视频流实际为 25fps（正常帧间隔为 40ms）。当渲染线程 `video_refresh` 检查丢帧时：
     ```c
     if (!is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) 
         && time > is->frame_timer + duration) {
         frame_queue_next(&is->pictq); // 直接丢弃该帧！
         goto retry;                   // 循环丢弃下一帧！
     }
     ```
  3. 因为 `duration` 被人为压成 10ms，`time > is->frame_timer + 0.01` **永远为真**！
  4. 渲染线程在几毫秒内将队列中解码出的所有 P 帧、B 帧**全部无情丢弃（Drop）**，只有每隔 20 秒下一个 GOP 关键帧（I 帧）到达重置队列时，才侥幸渲染出 1 帧，造成“20秒动一次”。

---

### 问题三：网络拉流饥饿（1316 字节缓冲区阻塞）
* **根因：`max-buffer-size = 1316` 字节导致读取线程停止拉流**  
  在 `ff_ffplay.c:3586` 中：
  ```c
  if (is->audioq.size + is->videoq.size > ffp->dcc.max_buffer_size) {
      continue; // 认为队列已满，停止从 socket 读取
  }
  ```
  单个 1080P I 帧约 50KB~150KB。将 `max-buffer-size` 设为 1316 字节（1.3KB）导致读取线程每读半个包就误判“缓冲区满”而暂停读取，网络拉流断断续续，造成解码端严重饥饿。
* **`infbuf = 1` 分类错误**：`infbuf` 属于 `OPT_CATEGORY_PLAYER`，被错误设置到 `OPT_CATEGORY_FORMAT`，导致无限缓冲未生效。

---

### 问题四：上层过早触发 `stopPlayback()` 中断握手
* **根因**：海外服务器 HTTPS/TLS 握手及探测 FLV 头部需约 1.5 秒。上层代码在起播后仅 **870ms**（尚未完成握手前）由于重复起播或生命周期触发了 `stopPlayback()`，导致底层抛出 `Immediate exit requested (AVERROR_EXIT)`。

---

## 3. 修复方案与代码实现

### C 底层引擎修复（Commit: `d37f3205`）

#### 1. [`ff_ffplay.c`](file:///Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/ff_ffplay.c)
* **`compute_target_delay`**：开启 0 延迟（`delay_forbidden > 0`）时，直接返回 `0.0`，禁止时钟差导致的延时膨胀。
  ```c
  static double compute_target_delay(FFPlayer *ffp, double delay, VideoState *is)
  {
      double sync_threshold, diff = 0;
      if (ffp && ffp->delay_forbidden > 0) {
          return 0.0;
      }
      ...
  }
  ```
* **`video_refresh`**：
  - 0 延迟模式下采用快速轮询刷新（`*remaining_time = VIDEO_ONLY_FAST_POLLING_RATE`）。
  - 渲染延时与丢帧逻辑增加 `ffp->delay_forbidden <= 0` 保护，防止 0 延迟模式误丢正常帧：
  ```c
  if (ffp->delay_forbidden <= 0 && time < is->frame_timer + delay) {
      *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
      goto display;
  }
  ...
  if (ffp->delay_forbidden <= 0 && !is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
      frame_queue_next(&is->pictq);
      goto retry;
  }
  ```
* **`decoder_decode_frame`**：加入 `ffp->delay_forbidden <= 0` 保护，屏蔽解码层早期丢帧。
* **`ffplay_video_thread`**：移除硬编码 `duration = 0.01`，恢复基于帧率计算的真实帧时长：
  ```c
  AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
  ...
  duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
  ```

#### 2. [`ffpipenode_android_mediacodec_vdec.c`](file:///Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c)
* 在 `drain_output_buffer2` 和 `func_run_sync` 中加入 `ffp->delay_forbidden <= 0` 保护，防止 MediaCodec 硬解输出帧被丢弃：
  ```c
  if (ffp->delay_forbidden <= 0 && (ffp->framedrop > 0 || (ffp->framedrop && ffp_get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))) {
      // 仅在非 0 延迟模式下允许丢帧
  }
  ```

---

### Java / Kotlin 业务层参数与逻辑优化

在 `IjkPlayView.java` 中优化参数配置：

```java
// 1. 修正缓冲区配置（避免 1316 字节导致的拉流饥饿）
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_FORMAT, "buffer_size", 1024 * 1024);         // 1MB Socket Buffer
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "max-buffer-size", 2 * 1024 * 1024);  // 2MB Packet Queue Buffer

// 2. 正确开启实时流无限制缓冲
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "infbuf", 1);

// 3. 允许正常缓冲以抗网络抖动
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "packet-buffering", 1);
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "min-frames", 2);

// 4. 开启 H.265 硬解码
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "mediacodec-hevc", 1);
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "mediacodec", 1);
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "framedrop", 0); // 关闭丢帧
```

---

## 4. 编译与多架构验证

已使用 Android NDK 完成全架构构建验证：

```bash
# 1. 编译 arm64-v8a
./compile-ijk.sh arm64
# 输出：ijkplayer/ijkplayer-arm64/src/main/libs/arm64-v8a/libijkplayer.so (601 KB)

# 2. 全量重构 armeabi-v7a
./compile-ijk.sh armv7a rebuild
# 输出：ijkplayer/ijkplayer-armv7a/src/main/libs/armeabi-v7a/libijkplayer.so (412 KB)
```

两套架构对应的 4 个 `.so` 文件（`libijkplayer.so`, `libijksdl.so`, `libijkffmpegcmd.so`, `libijkwdzffmpeg.so`）均已全量更新并对齐。

---

## 5. 最佳实践与参数配置推荐表

| 参数名 | 推荐设置值 | 所属分类 (Category) | 作用说明 |
| :--- | :--- | :--- | :--- |
| `mediacodec-hevc` | `1` | `OPT_CATEGORY_PLAYER` | 开启 H.265 / HEVC 硬件加速解码 |
| `mediacodec` | `1` | `OPT_CATEGORY_PLAYER` | 开启 H.264 硬件加速解码 |
| `framedrop` | `0` | `OPT_CATEGORY_PLAYER` | 设为 `0` 禁止粗暴跳帧，保持画面连续性 |
| `max-buffer-size` | `2097152` (2MB) | `OPT_CATEGORY_PLAYER` | 播放器包队列缓冲上限，避免过小导致网络停拉 |
| `buffer_size` | `1048576` (1MB) | `OPT_CATEGORY_FORMAT` | 底层网络 IO Socket 缓冲区大小 |
| `infbuf` | `1` | `OPT_CATEGORY_PLAYER` | 实时流/低延迟模式下允许连续拉流 |
| `packet-buffering` | `1` | `OPT_CATEGORY_PLAYER` | 开启微缓冲，抗网络抖动 |
| `max-fps` | `0` 或保留 `15` | `OPT_CATEGORY_FORMAT` | 硬解下无效，仅在软解下限制帧率降低功耗 |
| `delay_forbidden` | `1` (直播/回放) | `setZeroVideoDelay` | 0 延迟模式，现已支持平滑不丢帧连续渲染 |
