#!/usr/bin/env bash
set -e

# Detect host OS for NDK prebuilt toolchain
case "$(uname -s)" in
    Darwin*)
        if [[ $(uname -m) == 'arm64' ]]; then
            IJK_NDK_HOST_TAG=darwin-arm64
            if [ -n "$ANDROID_NDK" ] && [ ! -d "$ANDROID_NDK/toolchains/llvm/prebuilt/$IJK_NDK_HOST_TAG" ]; then
                IJK_NDK_HOST_TAG=darwin-x86_64
            fi
        else
            IJK_NDK_HOST_TAG=darwin-x86_64
        fi
        ;;
    Linux*)   IJK_NDK_HOST_TAG=linux-x86_64 ;;
    CYGWIN*|MINGW*) IJK_NDK_HOST_TAG=windows-x86_64 ;;
    *) echo "Unsupported OS"; exit 1 ;;
esac

# Parallel make flag (adjust as needed)
# Use number of CPU cores if available, fallback to 4
if command -v sysctl >/dev/null 2>&1; then
    IJK_MAKE_FLAG="-j$(sysctl -n hw.ncpu)"
else
    IJK_MAKE_FLAG="-j4"
fi

# Page size for OpenSSL (default 4096)
# If arm64 or x86_64, use 16384 for modern Android compatibility
IJK_PAGE_SIZE=4096
if [[ "$1" == "arm64" || "$1" == "x86_64" ]]; then
    IJK_PAGE_SIZE=16384
fi
