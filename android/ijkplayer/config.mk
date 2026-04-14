# config.mk
# Centralized configuration for ijkplayer Android build system

# Use abspath instead of realpath to keep paths within the project tree (better for IDE indexing)
MY_APP_CONFIG_FILE := $(abspath $(lastword $(MAKEFILE_LIST)))
MY_APP_IJKPLAYER_ROOT := $(patsubst %/,%,$(dir $(MY_APP_CONFIG_FILE)))
MY_APP_ANDROID_ROOT := $(abspath $(MY_APP_IJKPLAYER_ROOT)/..)

# Architecture-specific FFmpeg configuration
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
    MY_APP_FFMPEG_OUTPUT_PATH := $(MY_APP_ANDROID_ROOT)/contrib/build/ffmpeg-armv7a/output
endif
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
    MY_APP_FFMPEG_OUTPUT_PATH := $(MY_APP_ANDROID_ROOT)/contrib/build/ffmpeg-arm64/output
endif
ifeq ($(TARGET_ARCH_ABI),x86)
    MY_APP_FFMPEG_OUTPUT_PATH := $(MY_APP_ANDROID_ROOT)/contrib/build/ffmpeg-x86/output
endif
ifeq ($(TARGET_ARCH_ABI),x86_64)
    MY_APP_FFMPEG_OUTPUT_PATH := $(MY_APP_ANDROID_ROOT)/contrib/build/ffmpeg-x86_64/output
endif

# Final path validation
MY_APP_FFMPEG_INCLUDE_PATH := $(MY_APP_FFMPEG_OUTPUT_PATH)/include

# Define common flags that should be added to each module
# Use these variables in your module Android.mk after $(CLEAR_VARS)
IJK_COMMON_C_INCLUDES := $(MY_APP_FFMPEG_INCLUDE_PATH)
IJK_COMMON_CFLAGS     := -Wno-deprecated-declarations -Wno-unused-variable -Wno-unused-function \
                         -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
                         -Wno-incompatible-pointer-types -Wno-implicit-function-declaration
IJK_COMMON_LDFLAGS    := -Wl,-z,max-page-size=16384
