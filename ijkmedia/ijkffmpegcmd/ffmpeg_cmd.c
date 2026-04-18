#include "ffmpeg_cmd.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "android_log.h"
#include "ffmpeg_thread.h"

#define JNI_CLASS_FFMPEG_CMD "tv/danmaku/ijk/media/player/ffmpeg/cmd/FFmpegCmd"

static JavaVM *g_jvm = NULL;
static jclass g_ffmpeg_cmd_class = NULL;
static char **g_argv = NULL;
static int g_argc = 0;
static pthread_mutex_t g_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;

static void release_args_l(void)
{
    int i;

    if (!g_argv)
        return;

    for (i = 0; i < g_argc; ++i) {
        free(g_argv[i]);
    }
    free(g_argv);
    g_argv = NULL;
    g_argc = 0;
}

static int copy_args(JNIEnv *env, jint argc, jobjectArray argv)
{
    int i;

    g_argv = calloc(argc, sizeof(char *));
    if (!g_argv)
        return -1;

    g_argc = argc;
    for (i = 0; i < argc; ++i) {
        jstring arg = (jstring)(*env)->GetObjectArrayElement(env, argv, i);
        const char *utf = NULL;
        size_t len = 0;

        if (!arg)
            continue;

        utf = (*env)->GetStringUTFChars(env, arg, NULL);
        if (!utf) {
            (*env)->DeleteLocalRef(env, arg);
            release_args_l();
            return -1;
        }

        len = strlen(utf);
        g_argv[i] = malloc(len + 1);
        if (!g_argv[i]) {
            (*env)->ReleaseStringUTFChars(env, arg, utf);
            (*env)->DeleteLocalRef(env, arg);
            release_args_l();
            return -1;
        }

        memcpy(g_argv[i], utf, len + 1);
        (*env)->ReleaseStringUTFChars(env, arg, utf);
        (*env)->DeleteLocalRef(env, arg);
    }

    return 0;
}

static JNIEnv *attach_thread(void)
{
    JNIEnv *env = NULL;

    if (!g_jvm)
        return NULL;

    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK)
        return NULL;

    return env;
}

static void detach_thread(void)
{
    if (g_jvm)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void call_java_int(const char *method_name, jint value)
{
    JNIEnv *env = attach_thread();
    jmethodID method_id = NULL;

    if (!env || !g_ffmpeg_cmd_class)
        goto out;

    method_id = (*env)->GetStaticMethodID(env, g_ffmpeg_cmd_class, method_name, "(I)V");
    if (method_id)
        (*env)->CallStaticVoidMethod(env, g_ffmpeg_cmd_class, method_id, value);

out:
    detach_thread();
}

static void call_java_float(const char *method_name, jfloat value)
{
    JNIEnv *env = attach_thread();
    jmethodID method_id = NULL;

    if (!env || !g_ffmpeg_cmd_class)
        goto out;

    method_id = (*env)->GetStaticMethodID(env, g_ffmpeg_cmd_class, method_name, "(F)V");
    if (method_id)
        (*env)->CallStaticVoidMethod(env, g_ffmpeg_cmd_class, method_id, value);

out:
    detach_thread();
}

static void ffmpeg_thread_finished(int ret)
{
    call_java_int("onNativeExecuted", ret);

    pthread_mutex_lock(&g_cmd_mutex);
    release_args_l();
    pthread_mutex_unlock(&g_cmd_mutex);
}

void ffmpeg_progress(float progress)
{
    call_java_float("onNativeProgress", progress);
}

void ffmpeg_complete(int errorCode)
{
    call_java_int("onNativeComplete", errorCode);
}

void ffmpeg_failure(int errorCode)
{
    call_java_int("onNativeFailure", errorCode);
}

void ffmpeg_cancel_finish(int errorCode)
{
    call_java_int("onNativeCancelFinish", errorCode);
}

JNIEXPORT jint JNICALL
Java_tv_danmaku_ijk_media_player_ffmpeg_cmd_FFmpegCmd_nativeExec(JNIEnv *env, jclass clazz,
                                                                 jint argc, jobjectArray argv)
{
    int ret = 0;

    if (argc <= 0 || !argv)
        return -1;

    (*env)->GetJavaVM(env, &g_jvm);
    if (!g_ffmpeg_cmd_class)
        g_ffmpeg_cmd_class = (*env)->NewGlobalRef(env, clazz);

    pthread_mutex_lock(&g_cmd_mutex);
    release_args_l();
    ret = copy_args(env, argc, argv);
    pthread_mutex_unlock(&g_cmd_mutex);
    if (ret < 0)
        return ret;

    ffmpeg_thread_callback(ffmpeg_thread_finished);
    ret = ffmpeg_thread_run_cmd(argc, g_argv);
    if (ret < 0) {
        pthread_mutex_lock(&g_cmd_mutex);
        release_args_l();
        pthread_mutex_unlock(&g_cmd_mutex);
    }

    return ret;
}

JNIEXPORT void JNICALL
Java_tv_danmaku_ijk_media_player_ffmpeg_cmd_FFmpegCmd_nativeExit(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;

    if (ffmpeg_thread_is_running()) {
        ffmpeg_thread_cancel();
        ffmpeg_cancel_finish(0);
    }

    pthread_mutex_lock(&g_cmd_mutex);
    release_args_l();
    pthread_mutex_unlock(&g_cmd_mutex);
}
