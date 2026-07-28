#!/usr/bin/env bash

set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="arm64"
ABI="arm64-v8a"
PREFERRED_NDK_VERSION="27.3.13750724"

log() {
    printf '\n==> %s\n' "$*"
}

die() {
    printf '\nERROR: %s\n' "$*" >&2
    exit 1
}

on_error() {
    local exit_code=$?
    printf '\nBUILD FAILED at line %s (exit code %s)\n' "${BASH_LINENO[0]}" "$exit_code" >&2
    exit "$exit_code"
}

trap on_error ERR

resolve_android_tools() {
    ANDROID_SDK="${ANDROID_SDK:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"

    if [[ -n "${ANDROID_NDK:-}" ]]; then
        :
    elif [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
        ANDROID_NDK="$ANDROID_NDK_HOME"
    elif [[ -d "$ANDROID_SDK/ndk/$PREFERRED_NDK_VERSION" ]]; then
        ANDROID_NDK="$ANDROID_SDK/ndk/$PREFERRED_NDK_VERSION"
    else
        local candidate
        ANDROID_NDK=""
        for candidate in "$ANDROID_SDK"/ndk/*; do
            if [[ -d "$candidate" ]]; then
                ANDROID_NDK="$candidate"
            fi
        done
    fi

    [[ -d "$ANDROID_SDK" ]] || die "Android SDK not found: $ANDROID_SDK"
    [[ -n "${ANDROID_NDK:-}" && -x "$ANDROID_NDK/ndk-build" ]] || \
        die "Android NDK not found. Install NDK $PREFERRED_NDK_VERSION or export ANDROID_NDK."

    export ANDROID_SDK
    export ANDROID_HOME="$ANDROID_SDK"
    export ANDROID_NDK
    export ANDROID_NDK_HOME="$ANDROID_NDK"

    log "Android SDK: $ANDROID_SDK"
    log "Android NDK: $ANDROID_NDK"

    if [[ "$(basename "$ANDROID_NDK")" != "$PREFERRED_NDK_VERSION" ]]; then
        printf 'WARNING: this project targets NDK %s; continuing with %s.\n' \
            "$PREFERRED_NDK_VERSION" "$(basename "$ANDROID_NDK")" >&2
    fi
}

ensure_sources() {
    cd "$ROOT_DIR"

    if [[ ! -f android/contrib/openssl-arm64/Configure ]]; then
        log "Initializing OpenSSL sources"
        ./init-android-openssl.sh
    else
        log "OpenSSL sources already initialized"
    fi

    if [[ ! -f android/contrib/x264-arm64/configure ]]; then
        log "Initializing x264 sources"
        ./init-android-x264.sh
    else
        log "x264 sources already initialized"
    fi

    if [[ ! -f android/contrib/ffmpeg-arm64/configure ]]; then
        log "Initializing FFmpeg and IJK dependencies"
        ./init-android.sh
    else
        log "FFmpeg sources already initialized"

        if [[ ! -f ijkmedia/ijkyuv/Android.mk ]]; then
            log "Initializing libyuv sources"
            ./init-android-libyuv.sh
        fi

        if [[ ! -f ijkmedia/ijksoundtouch/Android.mk ]]; then
            log "Initializing SoundTouch sources"
            ./init-android-soundtouch.sh
        fi
    fi

    [[ -f config/module-wdz.sh ]] || die "Missing config/module-wdz.sh"
    if [[ ! -L config/module.sh || "$(readlink config/module.sh)" != "module-wdz.sh" ]]; then
        log "Selecting config/module-wdz.sh"
        rm -f config/module.sh
        ln -s module-wdz.sh config/module.sh
    fi
}

build_native() {
    log "Building OpenSSL for $TARGET"
    (
        cd "$ROOT_DIR/android/contrib"
        ./compile-openssl.sh "$TARGET"
    )

    log "Building x264 for $TARGET"
    (
        cd "$ROOT_DIR/android/contrib"
        ./compile-x264.sh "$TARGET"
    )

    log "Building FFmpeg for $TARGET"
    (
        cd "$ROOT_DIR/android/contrib"
        ./compile-ffmpeg.sh "$TARGET"
    )

    log "Building IJKPlayer and FFmpeg command module for $TARGET"
    (
        cd "$ROOT_DIR/android"
        APP_ALLOW_MISSING_DEPS=true ./compile-ijk.sh "$TARGET"
    )
}

verify_outputs() {
    local output_dir="$ROOT_DIR/android/ijkplayer/ijkplayer-arm64/src/main/libs/$ABI"
    local failed=0
    local library
    local libraries=(
        libijkwdzffmpeg.so
        libijkplayer.so
        libijkffmpegcmd.so
    )

    log "Verifying build outputs"
    for library in "${libraries[@]}"; do
        if [[ ! -s "$output_dir/$library" ]]; then
            printf 'MISSING: %s\n' "$output_dir/$library" >&2
            failed=1
        fi
    done

    [[ "$failed" -eq 0 ]] || die "One or more required libraries were not generated"

    ls -lh "${libraries[@]/#/$output_dir/}"
    shasum -a 256 "${libraries[@]/#/$output_dir/}"

    printf '\nBUILD SUCCESSFUL\nOutput: %s\n' "$output_dir"
}

main() {
    command -v git >/dev/null 2>&1 || die "git is required"
    command -v make >/dev/null 2>&1 || die "make is required"

    resolve_android_tools
    ensure_sources
    build_native
    verify_outputs
}

main "$@"
