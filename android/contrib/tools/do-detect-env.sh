#! /usr/bin/env bash
#
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

# This script is based on projects below
# https://github.com/yixia/FFmpeg-Android
# http://git.videolan.org/?p=vlc-ports/android.git;a=summary

#--------------------
set -e

UNAME_S=$(uname -s)
UNAME_SM=$(uname -sm)
echo "build on $UNAME_SM"

echo "ANDROID_NDK=$ANDROID_NDK"

if [ -z "$ANDROID_NDK" ]; then
    echo "You must define ANDROID_NDK before starting."
    echo "They must point to your NDK directories."
    echo ""
    exit 1
fi



# try to detect NDK version
export IJK_MAKE_TOOLCHAIN_FLAGS=
export IJK_MAKE_FLAG=
# Support 16KB page size (Default is 16384 for Android 15+ compatibility)
# Can be overridden by setting IJK_PAGE_SIZE environment variable
export IJK_PAGE_SIZE=${IJK_PAGE_SIZE:-16384}

# try to detect cmake
export IJK_CMAKE=$(ls -d $HOME/Library/Android/sdk/cmake/*/bin/cmake 2>/dev/null | tail -n 1)
if [ -n "$IJK_CMAKE" ]; then
    export PATH=$(dirname $IJK_CMAKE):$PATH
fi

# Detect Host OS and Arch
case "$UNAME_S" in
    Darwin)
        export IJK_MAKE_FLAG=-j`sysctl -n machdep.cpu.thread_count`
        case "$(uname -m)" in
            arm64)
                export IJK_NDK_HOST_TAG="darwin-arm64"
                ;;
            *)
                export IJK_NDK_HOST_TAG="darwin-x86_64"
                ;;
        esac
        # Fallback for older NDKs that don't have darwin-arm64 folder
        if [ ! -d "$ANDROID_NDK/toolchains/llvm/prebuilt/$IJK_NDK_HOST_TAG" ]; then
            export IJK_NDK_HOST_TAG="darwin-x86_64"
        fi
    ;;
    Linux)
        export IJK_MAKE_FLAG=-j$(nproc)
        export IJK_NDK_HOST_TAG="linux-x86_64"
    ;;
    CYGWIN_NT-*)
        IJK_WIN_TEMP="$(cygpath -am /tmp)"
        export TEMPDIR=$IJK_WIN_TEMP/
        export IJK_NDK_HOST_TAG="windows-x86_64"
        echo "Cygwin temp prefix=$IJK_WIN_TEMP/"
    ;;
esac

export IJK_NDK_REL=$(grep -o '^Pkg\.Revision.*=[0-9]*.*' $ANDROID_NDK/source.properties 2>/dev/null | sed 's/[[:space:]]*//g' | cut -d "=" -f 2)
echo "IJK_NDK_REL=$IJK_NDK_REL"

case "$IJK_NDK_REL" in
    2[0-9]*)
        echo "NDKr$IJK_NDK_REL detected"
    ;;
    *)
        echo "You need the NDKr20 or later for this modernized build script."
        echo "Detected NDK version: $IJK_NDK_REL"
        exit 1
    ;;
esac

# Add page size flag to toolchain flags if page size is set to 16KB
if [ "$IJK_PAGE_SIZE" = "16384" ]; then
    export IJK_MAKE_TOOLCHAIN_FLAGS="$IJK_MAKE_TOOLCHAIN_FLAGS --page-size=16384"
    echo "Using 16KB page size for toolchain"
fi

