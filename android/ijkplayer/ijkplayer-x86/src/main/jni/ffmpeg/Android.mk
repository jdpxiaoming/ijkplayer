LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := ijkwdzffmpeg
LOCAL_SRC_FILES := $(MY_APP_FFMPEG_OUTPUT_PATH)/libijkwdzffmpeg.so
include $(PREBUILT_SHARED_LIBRARY)