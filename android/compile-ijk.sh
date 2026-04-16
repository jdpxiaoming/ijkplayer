#! /usr/bin/env bash
#
# Copyright (C) 2013-2014 Bilibili
# Copyright (C) 2013-2014 Zhang Rui <bbcallen@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

if [ -z "$ANDROID_NDK" -o -z "$ANDROID_NDK" ]; then
    echo "You must define ANDROID_NDK, ANDROID_SDK before starting."
    echo "They must point to your NDK and SDK directories.\n"
    exit 1
fi

REQUEST_TARGET=$1
REQUEST_SUB_CMD=$2
ACT_ABI_32="armv7a"
ACT_ABI_64="arm64"
ACT_ABI_ALL="armv7a arm64"
UNAME_S=$(uname -s)

FF_MAKEFLAGS=
if [ "$UNAME_S" = "Darwin" ] && which sysctl >/dev/null
then
    FF_MAKEFLAGS=-j`sysctl -n machdep.cpu.thread_count`
elif which nproc >/dev/null
then
    FF_MAKEFLAGS=-j`nproc`
fi

do_sub_cmd () {
    SUB_CMD=$1
    if [ -L "./android-ndk-prof" ]; then
        rm android-ndk-prof
    fi

    if [ "$PARAM_SUB_CMD" = 'prof' ]; then
        echo 'profiler build: YES';
        ln -s ../../../../../../ijkprof/android-ndk-profiler/jni android-ndk-prof
    else
        echo 'profiler build: NO';
        ln -s ../../../../../../ijkprof/android-ndk-profiler-dummy/jni android-ndk-prof
    fi

    case $SUB_CMD in
        prof)
            $ANDROID_NDK/ndk-build $FF_MAKEFLAGS
        ;;
        clean)
            $ANDROID_NDK/ndk-build clean
        ;;
        rebuild)
            $ANDROID_NDK/ndk-build clean
            $ANDROID_NDK/ndk-build $FF_MAKEFLAGS
        ;;
        *)
            $ANDROID_NDK/ndk-build $FF_MAKEFLAGS
        ;;
    esac
}

do_ndk_build () {
    PARAM_TARGET=$1
    PARAM_SUB_CMD=$2
    case "$PARAM_TARGET" in
        armv7a|arm64)
            cd "ijkplayer/ijkplayer-$PARAM_TARGET/src/main/jni"
            if [ "$PARAM_SUB_CMD" = 'prof' ]; then 
                if [ "$PARAM_TARGET" = "armv7a" ]; then
                    : # prof supported
                else
                    PARAM_SUB_CMD=''
                fi
            fi
            do_sub_cmd $PARAM_SUB_CMD
            cd -
        ;;
    esac
}


case "$REQUEST_TARGET" in
    "")
        do_ndk_build armv7a;
    ;;
    arm64)
        do_ndk_build $REQUEST_TARGET $REQUEST_SUB_CMD;
    ;;
    armv7a)
        do_ndk_build $REQUEST_TARGET $REQUEST_SUB_CMD;
    ;;
    x86_64)
        do_ndk_build $REQUEST_TARGET $REQUEST_SUB_CMD;
    ;;
    all32)
        for ABI in $ACT_ABI_32
        do
            do_ndk_build "$ABI" $REQUEST_SUB_CMD;
        done
    ;;
    all64)
        for ABI in $ACT_ABI_64
        do
            do_ndk_build "$ABI" $REQUEST_SUB_CMD;
        done
    ;;
    all)
        for ABI in $ACT_ABI_ALL
        do
            do_ndk_build "$ABI" $REQUEST_SUB_CMD;
        done
    ;;
    clean)
        for ABI in $ACT_ABI_ALL
        do
            do_ndk_build "$ABI" clean;
        done
    ;;
    *)
        echo "Usage:"
        echo "  compile-ijk.sh armv7a"
        echo "  compile-ijk.sh arm64"
        echo "  compile-ijk.sh x86_64"
        echo "  compile-ijk.sh all|all32"
        echo "  compile-ijk.sh all64"
        echo "  compile-ijk.sh clean"
    ;;
esac
