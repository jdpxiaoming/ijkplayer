LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
MY_FFMPEG_SOURCE_PATH := $(realpath $(LOCAL_PATH)/../../android/contrib/ffmpeg-armv7a)
endif
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
MY_FFMPEG_SOURCE_PATH := $(realpath $(LOCAL_PATH)/../../android/contrib/ffmpeg-arm64)
endif

include $(CLEAR_VARS)

LOCAL_MODULE := ijkffmpegcmd
LOCAL_CONLYFLAGS += -std=c99
LOCAL_LDLIBS += -llog -landroid

LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(realpath $(LOCAL_PATH)/../ijkplayer)
LOCAL_C_INCLUDES += $(realpath $(LOCAL_PATH)/../ijkplayer/ffmpeg)
LOCAL_C_INCLUDES += $(MY_APP_FFMPEG_INCLUDE_PATH)
LOCAL_C_INCLUDES += $(MY_FFMPEG_SOURCE_PATH)

LOCAL_SRC_FILES += ffmpeg_cmd.c
LOCAL_SRC_FILES += ffmpeg_thread.c
LOCAL_SRC_FILES += ../ijkplayer/ffmpeg/cmdutils.c
LOCAL_SRC_FILES += ../ijkplayer/ffmpeg/ffmpeg.c
LOCAL_SRC_FILES += ../ijkplayer/ffmpeg/ffmpeg_filter.c
LOCAL_SRC_FILES += ../ijkplayer/ffmpeg/ffmpeg_opt.c

LOCAL_SHARED_LIBRARIES := ijkwdzffmpeg

include $(BUILD_SHARED_LIBRARY)
