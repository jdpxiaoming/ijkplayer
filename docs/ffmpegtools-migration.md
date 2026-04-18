# FFmpegTools C++ Migration Notes

本文记录 2026-04-18 将 FFmpegTools 的命令执行能力迁移到当前 ijkplayer 工程的实现方式、已完成项、兼容修复和后续验证方法。

## 目标

- 复用同一套 FFmpeg 内核 `libijkwdzffmpeg.so`
- 在 ijkplayer 工程内提供 FFmpegTools 的命令式能力入口
- 支持常见能力：
  - 转码
  - 封装/录制
  - 水印
  - 缩略图
  - GIF 动图

## 本次迁移结论

- 没有重复搬运一整份 FFmpeg CLI 源码。
- 直接复用了工程中已有的 `ijkmedia/ijkplayer/ffmpeg/*.c`。
- 新增了一个轻量 JNI 模块 `ijkffmpegcmd`，让 Java 可以调用 FFmpeg CLI 命令执行能力。

## 新增目录

- [ijkmedia/ijkffmpegcmd](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkffmpegcmd/Android.mk)
- [android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmpegCmd.java)

## Native 侧改动

### 1. 新增 `ijkffmpegcmd` 模块

关键文件：

- [ijkmedia/ijkffmpegcmd/Android.mk](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkffmpegcmd/Android.mk)
- [ijkmedia/ijkffmpegcmd/ffmpeg_cmd.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkffmpegcmd/ffmpeg_cmd.c)
- [ijkmedia/ijkffmpegcmd/ffmpeg_thread.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkffmpegcmd/ffmpeg_thread.c)

作用：

- 对外提供 JNI 入口
- 启动后台线程执行 `ffmpeg_exec(argc, argv)`
- 复用已有 `libijkwdzffmpeg.so`

### 2. 兼容 FFmpeg 4.3 的 CLI 源码

修改文件：

- [ijkmedia/ijkplayer/ffmpeg/cmdutils.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/ffmpeg/cmdutils.c)
- [ijkmedia/ijkplayer/ffmpeg/cmdutils.h](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/ffmpeg/cmdutils.h)
- [ijkmedia/ijkplayer/ffmpeg/ffmpeg.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/ffmpeg/ffmpeg.c)
- [ijkmedia/ijkplayer/ffmpeg/ffmpeg_filter.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkplayer/ffmpeg/ffmpeg_filter.c)

主要修复：

- `libavresample` 已禁用时，不再强行包含 `avresample.h`
- `print_all_libs_info()` 中的 `avresample` 信息打印改为条件编译
- 恢复 `show_help_children(...)` 声明，避免新编译器下隐式声明报错
- 去掉会把 arm32 私有汇编错误带进 arm64 构建的 `libavcodec/mathops.h` 依赖，用本地 `mid_pred()` 替代

### 3. JNI 双包名兼容

当前 so 同时支持：

- 新包：`tv.danmaku.ijk.media.player.ffmpeg.cmd.FFmpegCmd`
- 旧包：`com.jdpxiaoming.ffmpeg_cmd.FFmpegCmd`

原因：

- FFmpegTools 旧工程仍使用原始包名
- 直接替换 `libijkffmpegcmd.so` 时，需要兼容旧 JNI 名称，否则会报：
  - `No implementation found for int com.jdpxiaoming.ffmpeg_cmd.FFmpegCmd.exec(...)`

### 4. 停止录制崩溃修复

问题现象：

- 点击停止后视频文件已经正常生成
- 但应用在停止瞬间崩溃
- ART 日志出现：
  - `attempting to detach while still running code`

根因：

- JNI 辅助函数在任何线程回调后都无条件执行 `DetachCurrentThread()`
- 当 `exit()` 从 Java 主线程进入 native 时，主线程本来就已附着，不应被 detach

修复方式：

- 在 JNI 辅助层先 `GetEnv`
- 只有当前线程是 native 侧临时 `AttachCurrentThread()` 上来的，才会在回调后 `DetachCurrentThread()`

修复文件：

- [ijkmedia/ijkffmpegcmd/ffmpeg_cmd.c](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/ijkmedia/ijkffmpegcmd/ffmpeg_cmd.c)

## Java 侧改动

新增封装：

- [FFmpegCmd.java](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmpegCmd.java)
- [FFmpegUtil.java](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmpegUtil.java)
- [FFmpegFactory.java](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmpegFactory.java)
- [FFmepgTask.java](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmepgTask.java)
- [FFLog.java](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/ffmpeg/cmd/FFLog.java)

当前已接入的命令工厂覆盖：

- 简单转封装
- RTSP/FLV 录制到 MP4
- 缩放
- GIF 截取
- 时间裁剪
- 图片水印
- 文字水印
- 加背景音乐
- concat
- 图片转视频

## FFmpeg 配置补充

修改文件：

- [config/module-wdz.sh](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/config/module-wdz.sh)

新增最小能力集：

- encoder
  - `gif`
  - `mjpeg`
  - `png`
  - `aac`
  - `libx264`
- decoder
  - `gif`
  - `png`
  - `hevc`
  - `opus`
  - `aac`
- muxer
  - `gif`
  - `image2`
  - `mp4`
- demuxer
  - `gif`
  - `image2`
  - `flv`
  - `live_flv`
  - `mov`
  - `rtsp`

说明：

- 如果只替换 `libijkffmpegcmd.so`，但没有重编 FFmpeg，那么新增功能可能在运行时提示缺失 muxer/encoder/filter。
- 代码迁移完成后，仍建议重新编一轮 FFmpeg。

## 构建验证结果

本次已单模块验证通过：

- [android/ijkplayer/ijkplayer-arm64/src/main/libs/arm64-v8a/libijkffmpegcmd.so](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-arm64/src/main/libs/arm64-v8a/libijkffmpegcmd.so)
- [android/ijkplayer/ijkplayer-armv7a/src/main/libs/armeabi-v7a/libijkffmpegcmd.so](/Users/cxm/develop/caixingming/ijkplayer/ijkplayer2026/ijkplayer/android/ijkplayer/ijkplayer-armv7a/src/main/libs/armeabi-v7a/libijkffmpegcmd.so)

使用的验证方式：

```bash
export ANDROID_NDK=/Users/cxm/Library/Android/sdk/ndk/27.3.13750724
export APP_ALLOW_MISSING_DEPS=true

cd android/ijkplayer/ijkplayer-arm64/src/main/jni
$ANDROID_NDK/ndk-build APP_MODULES=ijkffmpegcmd -j8

cd android/ijkplayer/ijkplayer-armv7a/src/main/jni
$ANDROID_NDK/ndk-build APP_MODULES=ijkffmpegcmd -j8
```

说明：

- 当前仓库完整 `compile-ijk.sh all` 仍可能先被既有 `ijksoundtouch` 缺模块问题挡住
- 所以本次优先验证新增模块 `ijkffmpegcmd` 本身可编译、可产物、可回调

## 放回 FFmpegTools 项目时的注意点

如果 FFmpegTools 仍沿用旧 Java 包：

- `com.jdpxiaoming.ffmpeg_cmd.FFmpegCmd`
- `com.jdpxiaoming.ffmpeg_cmd.FFmpegUtil`

那么只需替换新的 `libijkffmpegcmd.so`，不必强制先改包名。

本次 so 已兼容：

- 旧 JNI 方法名
- 新 JNI 方法名
- 旧回调方法名
- 新回调方法名

## 推荐后续动作

1. 重新编译 FFmpeg，使 `module-wdz.sh` 的新增能力生效。
2. 清理或补齐 `ijksoundtouch` 模块，让整仓 `compile-ijk.sh all` 也能恢复全量验证。
3. 如需完全兼容 FFmpegTools 旧工程，再补 `dump_stream` / `dump_Rtsp_h265` 这两个历史特例接口。
