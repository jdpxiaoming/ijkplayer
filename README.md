# ijkplayer

 Platform | Build Status
 -------- | ------------
 Android | [![Build Status](https://travis-ci.org/Bilibili/ci-ijk-ffmpeg-android.svg?branch=master)](https://travis-ci.org/Bilibili/ci-ijk-ffmpeg-android)
 iOS | [![Build Status](https://travis-ci.org/Bilibili/ci-ijk-ffmpeg-ios.svg?branch=master)](https://travis-ci.org/Bilibili/ci-ijk-ffmpeg-ios)

Video player based on [ffplay](http://ffmpeg.org)

## NEW!

- 增加视频同步开关
- 超过2倍速不生效
- 增加边播边录+录屏代码

https://github.com/jdpxiaoming/ijkrtspdemo

### Download

- Android:
 - Gradle
```
# required
allprojects {
    repositories {
        jcenter()
    }
}

dependencies {
    implementation 'io.github.jdpxiaoming:ijkplayer-view:0.0.26'
    implementation 'io.github.jdpxiaoming:ijkplayer-java:0.0.26'
    implementation 'io.github.jdpxiaoming:ijkplayer-armv7a:0.0.26'
    //看情况如果需要64位so则引入.
    implementation 'io.github.jdpxiaoming:ijkplayer-arm64:0.0.26'
}
```
- iOS
 - in coming...

### My Build Environment
- Common
 - Mac OS X 10.11.5
- Android
 - [NDK r10e](http://developer.android.com/tools/sdk/ndk/index.html)
 - Android Studio 2.1.3
 - Gradle 2.14.1
- iOS
 - Xcode 7.3 (7D175)
- [HomeBrew](http://brew.sh)
 - ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
 - brew install git

### Latest Changes
- [NEWS.md](NEWS.md)

### Features
- Common
 - remove rarely used ffmpeg components to reduce binary size [config/module-lite.sh](config/module-lite.sh)
 - workaround for some buggy online video.
- Android
 - platform: API 9~23
 - cpu: ARMv7a, ARM64v8a, x86 (ARMv5 is not tested on real devices)
 - api: [MediaPlayer-like](android/ijkplayer/ijkplayer-java/src/main/java/tv/danmaku/ijk/media/player/IMediaPlayer.java)
 - video-output: NativeWindow, OpenGL ES 2.0
 - audio-output: AudioTrack, OpenSL ES
 - hw-decoder: MediaCodec (API 16+, Android 4.1+)
 - alternative-backend: android.media.MediaPlayer, ExoPlayer
- iOS
 - platform: iOS 7.0~10.2.x
 - cpu: armv7, arm64, i386, x86_64, (armv7s is obselete)
 - api: [MediaPlayer.framework-like](ios/IJKMediaPlayer/IJKMediaPlayer/IJKMediaPlayback.h)
 - video-output: OpenGL ES 2.0
 - audio-output: AudioQueue, AudioUnit
 - hw-decoder: VideoToolbox (iOS 8+)
 - alternative-backend: AVFoundation.Framework.AVPlayer, MediaPlayer.Framework.MPMoviePlayerControlelr (obselete since iOS 8)

### NOT-ON-PLAN
- obsolete platforms (Android: API-8 and below; iOS: pre-6.0)
- obsolete cpu: ARMv5, ARMv6, MIPS (I don't even have these types of devices…)
- native subtitle render
- avfilter support

### Before Build
```
# install homebrew, git, yasm
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
brew install git
brew install yasm

# add these lines to your ~/.bash_profile or ~/.profile
# export ANDROID_SDK=<your sdk path>
# export ANDROID_NDK=<your ndk path>

# on Cygwin (unmaintained)
# install git, make, yasm
```

- If you prefer more codec/format
```
cd config
rm module.sh
ln -s module-default.sh module.sh
cd android/contrib
# cd ios
sh compile-ffmpeg.sh clean
```

- If you prefer less codec/format for smaller binary size (include hevc function)
```
cd config
rm module.sh
ln -s module-lite-hevc.sh module.sh
cd android/contrib
# cd ios
sh compile-ffmpeg.sh clean
```

- If you prefer less codec/format for smaller binary size (by default)
```
cd config
rm module.sh
ln -s module-lite.sh module.sh
cd android/contrib
# cd ios
sh compile-ffmpeg.sh clean
```

- For Ubuntu/Debian users.
```
# choose [No] to use bash
sudo dpkg-reconfigure dash
```

- If you'd like to share your config, pull request is welcome.

### Build Android
```
git clone https://github.com/Bilibili/ijkplayer.git ijkplayer-android
cd ijkplayer-android
git checkout -B latest k0.8.8

./init-android.sh

cd android/contrib
./compile-ffmpeg.sh clean
./compile-ffmpeg.sh all

cd ..
./compile-ijk.sh all

# Android Studio:
#     Open an existing Android Studio project
#     Select android/ijkplayer/ and import
#
#     define ext block in your root build.gradle
#     ext {
#       compileSdkVersion = 23       // depending on your sdk version
#       buildToolsVersion = "23.0.0" // depending on your build tools version
#
#       targetSdkVersion = 23        // depending on your sdk version
#     }
#
# If you want to enable debugging ijkplayer(native modules) on Android Studio 2.2+: (experimental)
#     sh android/patch-debugging-with-lldb.sh armv7a
#     Install Android Studio 2.2(+)
#     Preference -> Android SDK -> SDK Tools
#     Select (LLDB, NDK, Android SDK Build-tools,Cmake) and install
#     Open an existing Android Studio project
#     Select android/ijkplayer
#     Sync Project with Gradle Files
#     Run -> Edit Configurations -> Debugger -> Symbol Directories
#     Add "ijkplayer-armv7a/.externalNativeBuild/ndkBuild/release/obj/local/armeabi-v7a" to Symbol Directories
#     Run -> Debug 'ijkplayer-example'
#     if you want to reverse patches:
#     sh patch-debugging-with-lldb.sh reverse armv7a
#
# Eclipse: (obselete)
#     File -> New -> Project -> Android Project from Existing Code
#     Select android/ and import all project
#     Import appcompat-v7
#     Import preference-v7
#
# Gradle
#     cd ijkplayer
#     gradle

```


### Build iOS
```
git clone https://github.com/Bilibili/ijkplayer.git ijkplayer-ios
cd ijkplayer-ios
git checkout -B latest k0.8.8

./init-ios.sh

cd ios
./compile-ffmpeg.sh clean
./compile-ffmpeg.sh all

# Demo
#     open ios/IJKMediaDemo/IJKMediaDemo.xcodeproj with Xcode
# 
# Import into Your own Application
#     Select your project in Xcode.
#     File -> Add Files to ... -> Select ios/IJKMediaPlayer/IJKMediaPlayer.xcodeproj
#     Select your Application's target.
#     Build Phases -> Target Dependencies -> Select IJKMediaFramework
#     Build Phases -> Link Binary with Libraries -> Add:
#         IJKMediaFramework.framework
#
#         AudioToolbox.framework
#         AVFoundation.framework
#         CoreGraphics.framework
#         CoreMedia.framework
#         CoreVideo.framework
#         libbz2.tbd
#         libz.tbd
#         MediaPlayer.framework
#         MobileCoreServices.framework
#         OpenGLES.framework
#         QuartzCore.framework
#         UIKit.framework
#         VideoToolbox.framework
#
#         ... (Maybe something else, if you get any link error)
# 
```


### Support (支持) ###
- Please do not send e-mail to me. Public technical discussion on github is preferred.
- 请尽量在 github 上公开讨论[技术问题](https://github.com/bilibili/ijkplayer/issues)，不要以邮件方式私下询问，恕不一一回复。


### License

```
Copyright (c) 2017 Bilibili
Licensed under LGPLv2.1 or later
```

ijkplayer required features are based on or derives from projects below:
- LGPL
  - [FFmpeg](http://git.videolan.org/?p=ffmpeg.git)
  - [libVLC](http://git.videolan.org/?p=vlc.git)
  - [kxmovie](https://github.com/kolyvan/kxmovie)
  - [soundtouch](http://www.surina.net/soundtouch/sourcecode.html)
- zlib license
  - [SDL](http://www.libsdl.org)
- BSD-style license
  - [libyuv](https://code.google.com/p/libyuv/)
- ISC license
  - [libyuv/source/x86inc.asm](https://code.google.com/p/libyuv/source/browse/trunk/source/x86inc.asm)

android/ijkplayer-exo is based on or derives from projects below:
- Apache License 2.0
  - [ExoPlayer](https://github.com/google/ExoPlayer)

android/example is based on or derives from projects below:
- GPL
  - [android-ndk-profiler](https://github.com/richq/android-ndk-profiler) (not included by default)

ios/IJKMediaDemo is based on or derives from projects below:
- Unknown license
  - [iOS7-BarcodeScanner](https://github.com/jpwiddy/iOS7-BarcodeScanner)

ijkplayer's build scripts are based on or derives from projects below:
- [gas-preprocessor](http://git.libav.org/?p=gas-preprocessor.git)
- [VideoLAN](http://git.videolan.org)
- [yixia/FFmpeg-Android](https://github.com/yixia/FFmpeg-Android)
- [kewlbear/FFmpeg-iOS-build-script](https://github.com/kewlbear/FFmpeg-iOS-build-script) 

### Commercial Use
ijkplayer is licensed under LGPLv2.1 or later, so itself is free for commercial use under LGPLv2.1 or later

But ijkplayer is also based on other different projects under various licenses, which I have no idea whether they are compatible to each other or to your product.

[IANAL](https://en.wikipedia.org/wiki/IANAL), you should always ask your lawyer for these stuffs before use it in your product.

# IJKPlayer直播录制功能

本项目为IJKPlayer添加了直播录制功能，使用户能够将正在观看的直播内容保存为本地MP4文件。

## 功能概述

- 支持将正在播放的直播流录制为MP4文件
- 提供简单的Java API接口：`startRecord`和`stopRecord`
- 支持转码录制功能：`startRecordTranscode`，可以将任何格式的视频源（如RTSP）转码为MP4
- 包含示例Activity展示如何使用录制功能
- 在播放直播的同时进行录制，不影响视频播放体验

## 实现细节

### C层实现

1. 在`ff_ffplay.h`和`ff_ffplay.c`中添加了录制相关函数：
   - `ffp_start_record`: 开始录制
   - `ffp_start_record_transcode`: 开始转码录制
   - `ffp_stop_record`: 停止录制
   - `ffp_record_file`: 将视频数据包写入MP4文件

2. 在`read_thread`函数中添加了录制处理逻辑，当`ffp->is_record`为true时，将接收到的数据包写入MP4文件。

3. 转码功能实现：
   - 使用FFmpeg的编解码API将原始格式转换为H.264/AAC编码
   - 支持视频格式转换和音频重采样
   - 自动处理时间戳，确保录制的视频可以正常播放

### JNI层实现

1. 在`ijkplayer_jni.c`中实现了JNI接口：
   - `IjkMediaPlayer_startRecord`: 调用`ijkmp_start_record`开始录制
   - `IjkMediaPlayer_startRecordTranscode`: 调用`ijkmp_start_record_transcode`开始转码录制
   - `IjkMediaPlayer_stopRecord`: 调用`ijkmp_stop_record`停止录制

2. 在JNI注册表中注册了以上接口，使Java层可以调用它们。

### Java层实现

1. 在`IjkMediaPlayer.java`中添加了录制接口：
   - `startRecord(String filePath)`: 开始录制到指定路径
   - `startRecordTranscode(String filePath)`: 开始转码录制到指定路径
   - `stopRecord()`: 停止录制

2. 创建了示例Activity `RecordSampleActivity`，演示如何使用录制功能：
   - 提供按钮控制录制的开始和停止
   - 支持选择直接录制或转码录制模式
   - 显示录制状态和保存路径
   - 处理生命周期事件，确保正确释放资源

## 使用方法

### 直接录制（保留原始格式）

适用于已经是MP4兼容格式的视频流（如H.264/AAC）：

```java
IjkMediaPlayer player = mVideoView.getIjkMediaPlayer();
if (player != null) {
    // 开始录制
    player.startRecord("/sdcard/record.mp4");
    
    // ... 播放一段时间后 ...
    
    // 停止录制
    player.stopRecord();
}
```

### 转码录制（转换为MP4格式）

适用于需要转换格式的视频流（如RTSP、RTMP等非MP4兼容格式）：

```java
IjkMediaPlayer player = mVideoView.getIjkMediaPlayer();
if (player != null) {
    // 开始转码录制，将任何格式转为MP4
    player.startRecordTranscode("/sdcard/record.mp4");
    
    // ... 播放一段时间后 ...
    
    // 停止录制
    player.stopRecord();
}
```

## 注意事项

1. 录制功能需要存储权限，请确保应用已获取相应权限
2. 转码录制会消耗更多CPU资源，可能会影响低端设备的播放流畅度
3. 录制的文件会保存在指定路径，请确保有足够的存储空间
4. 对于没有音频的视频流，录制时会保持原始的视频播放速度

## 示例代码

详见`RecordSampleActivity.java`，它提供了完整的录制功能演示。

## 未来改进

1. 添加录制质量和格式控制
2. 实现暂停/继续录制功能
3. 添加录制时间限制
4. 优化录制过程中的性能
5. 增加录制事件回调
