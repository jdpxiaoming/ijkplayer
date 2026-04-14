#! /usr/bin/env bash
#
# Copyright (C) 2014 Miguel Botón <waninkoko@gmail.com>
# Copyright (C) 2014 Zhang Rui <bbcallen@gmail.com>
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

#--------------------
set -e

if [ -z "$ANDROID_NDK" ]; then
    echo "You must define ANDROID_NDK before starting."
    echo "They must point to your NDK directories.\n"
    exit 1
fi

#--------------------
# common defines
FF_ARCH=$1
if [ -z "$FF_ARCH" ]; then
    echo "You must specific an architecture 'arm, armv7a, x86, ...'.\n"
    exit 1
fi


FF_BUILD_ROOT=`pwd`
FF_ANDROID_PLATFORM=21


FF_BUILD_NAME=
FF_SOURCE=
FF_CROSS_PREFIX=
FF_TOOLCHAIN_NAME=

FF_CFG_FLAGS=
FF_PLATFORM_CFG_FLAGS=

FF_EXTRA_CFLAGS=
FF_EXTRA_LDFLAGS=



#--------------------
echo ""
echo "--------------------"
echo "[*] check environment"
echo "--------------------"
. ./tools/do-detect-env.sh
FF_MAKE_FLAGS=$IJK_MAKE_FLAG


#----- armv7a begin -----
if [ "$FF_ARCH" = "armv7a" ]; then
    FF_BUILD_NAME=openssl-armv7a
    FF_SOURCE=$FF_BUILD_ROOT/$FF_BUILD_NAME
	
    FF_CROSS_PREFIX=armv7a-linux-androideabi
    FF_TOOLCHAIN_NAME=armv7a-linux-androideabi
    FF_PLATFORM_CFG_FLAGS="linux-armv4 -marm no-asm"

elif [ "$FF_ARCH" = "x86" ]; then
    FF_BUILD_NAME=openssl-x86
    FF_SOURCE=$FF_BUILD_ROOT/$FF_BUILD_NAME
	
    FF_CROSS_PREFIX=i686-linux-android
    FF_TOOLCHAIN_NAME=i686-linux-android
    FF_PLATFORM_CFG_FLAGS="android-x86"
    FF_CFG_FLAGS="$FF_CFG_FLAGS no-asm"

elif [ "$FF_ARCH" = "x86_64" ]; then
    FF_BUILD_NAME=openssl-x86_64
    FF_SOURCE=$FF_BUILD_ROOT/$FF_BUILD_NAME

    FF_CROSS_PREFIX=x86_64-linux-android
    FF_TOOLCHAIN_NAME=x86_64-linux-android
    FF_PLATFORM_CFG_FLAGS="linux-x86_64"

elif [ "$FF_ARCH" = "arm64" ]; then
    FF_BUILD_NAME=openssl-arm64
    FF_SOURCE=$FF_BUILD_ROOT/$FF_BUILD_NAME

    FF_CROSS_PREFIX=aarch64-linux-android
    FF_TOOLCHAIN_NAME=aarch64-linux-android
    FF_PLATFORM_CFG_FLAGS="linux-aarch64"



else
    echo "unknown architecture $FF_ARCH";
    exit 1
fi

FF_TOOLCHAIN_PATH=$ANDROID_NDK/toolchains/llvm/prebuilt/$IJK_NDK_HOST_TAG
FF_TOOLCHAIN_BIN=$FF_TOOLCHAIN_PATH/bin
FF_PREFIX=$FF_BUILD_ROOT/build/$FF_BUILD_NAME/output

mkdir -p $FF_PREFIX


#--------------------
echo ""
echo "--------------------"
echo "[*] check openssl env"
echo "--------------------"
export PATH=$FF_TOOLCHAIN_BIN:$PATH

export CC=$FF_TOOLCHAIN_NAME$FF_ANDROID_PLATFORM-clang
export AR=llvm-ar
export NM=llvm-nm
export RANLIB=llvm-ranlib

export COMMON_FF_CFG_FLAGS=

# Support 16KB page size for OpenSSL
export LDFLAGS="-Wl,-z,max-page-size=$IJK_PAGE_SIZE"
echo "Adding $IJK_PAGE_SIZE page size support for openssl"

FF_CFG_FLAGS="$FF_CFG_FLAGS $COMMON_FF_CFG_FLAGS"

#--------------------
# Standard options:
FF_CFG_FLAGS="$FF_CFG_FLAGS zlib-dynamic"
FF_CFG_FLAGS="$FF_CFG_FLAGS no-shared"
FF_CFG_FLAGS="$FF_CFG_FLAGS -fPIC"
FF_CFG_FLAGS="$FF_CFG_FLAGS --openssldir=$FF_PREFIX"
# FF_CFG_FLAGS="$FF_CFG_FLAGS --cross-compile-prefix=$FF_TOOLCHAIN_NAME-"
FF_CFG_FLAGS="$FF_CFG_FLAGS $FF_PLATFORM_CFG_FLAGS"


#--------------------
echo ""
echo "--------------------"
echo "[*] configurate openssl"
echo "--------------------"
cd $FF_SOURCE
echo "./Configure $FF_CFG_FLAGS"
./Configure $FF_CFG_FLAGS

#--------------------
echo ""
echo "--------------------"
echo "[*] compile openssl"
echo "--------------------"
# make depend
make $FF_MAKE_FLAGS
make install_sw

#--------------------
echo ""
echo "--------------------"
echo "[*] link openssl"
echo "--------------------"

