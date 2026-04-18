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

typedef struct JniThreadEnv {
    JNIEnv *env;
    int attached_by_native;
} JniThreadEnv;

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

static JniThreadEnv attach_thread(void)
{
    JniThreadEnv thread_env;

    thread_env.env = NULL;
    thread_env.attached_by_native = 0;

    if (!g_jvm)
        return thread_env;

    if ((*g_jvm)->GetEnv(g_jvm, (void **)&thread_env.env, JNI_VERSION_1_6) == JNI_OK)
        return thread_env;

    if ((*g_jvm)->AttachCurrentThread(g_jvm, &thread_env.env, NULL) != JNI_OK) {
        thread_env.env = NULL;
        return thread_env;
    }

    thread_env.attached_by_native = 1;
    return thread_env;
}

static void detach_thread(JniThreadEnv *thread_env)
{
    if (thread_env && thread_env->attached_by_native && g_jvm)
        (*g_jvm)->DetachCurrentThread(g_jvm);
}

static int call_java_int_if_exists(const char *method_name, jint value)
{
    JniThreadEnv thread_env = attach_thread();
    JNIEnv *env = thread_env.env;
    jmethodID method_id = NULL;
    int called = 0;

    if (!env || !g_ffmpeg_cmd_class)
        goto out;

    method_id = (*env)->GetStaticMethodID(env, g_ffmpeg_cmd_class, method_name, "(I)V");
    if (method_id) {
        (*env)->CallStaticVoidMethod(env, g_ffmpeg_cmd_class, method_id, value);
        called = 1;
    } else {
        (*env)->ExceptionClear(env);
    }

out:
    detach_thread(&thread_env);
    return called;
}

static int call_java_void_if_exists(const char *method_name)
{
    JniThreadEnv thread_env = attach_thread();
    JNIEnv *env = thread_env.env;
    jmethodID method_id = NULL;
    int called = 0;

    if (!env || !g_ffmpeg_cmd_class)
        goto out;

    method_id = (*env)->GetStaticMethodID(env, g_ffmpeg_cmd_class, method_name, "()V");
    if (method_id) {
        (*env)->CallStaticVoidMethod(env, g_ffmpeg_cmd_class, method_id);
        called = 1;
    } else {
        (*env)->ExceptionClear(env);
    }

out:
    detach_thread(&thread_env);
    return called;
}

static int call_java_float_if_exists(const char *method_name, jfloat value)
{
    JniThreadEnv thread_env = attach_thread();
    JNIEnv *env = thread_env.env;
    jmethodID method_id = NULL;
    int called = 0;

    if (!env || !g_ffmpeg_cmd_class)
        goto out;

    method_id = (*env)->GetStaticMethodID(env, g_ffmpeg_cmd_class, method_name, "(F)V");
    if (method_id) {
        (*env)->CallStaticVoidMethod(env, g_ffmpeg_cmd_class, method_id, value);
        called = 1;
    } else {
        (*env)->ExceptionClear(env);
    }

out:
    detach_thread(&thread_env);
    return called;
}

static void ffmpeg_thread_finished(int ret)
{
    if (!call_java_int_if_exists("onNativeExecuted", ret))
        call_java_int_if_exists("onExecuted", ret);

    pthread_mutex_lock(&g_cmd_mutex);
    release_args_l();
    pthread_mutex_unlock(&g_cmd_mutex);
}

void ffmpeg_progress(float progress)
{
    if (!call_java_float_if_exists("onNativeProgress", progress))
        call_java_float_if_exists("onProgress", progress);
}

void ffmpeg_complete(int errorCode)
{
    if (!call_java_int_if_exists("onNativeComplete", errorCode))
        call_java_void_if_exists("onComplete");
}

void ffmpeg_failure(int errorCode)
{
    if (!call_java_int_if_exists("onNativeFailure", errorCode))
        call_java_void_if_exists("onFailure");
}

void ffmpeg_cancel_finish(int errorCode)
{
    if (!call_java_int_if_exists("onNativeCancelFinish", errorCode))
        call_java_void_if_exists("onCancelFinish");
}

static jint ijk_ffmpeg_cmd_exec(JNIEnv *env, jclass clazz, jint argc, jobjectArray argv)
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

static void ijk_ffmpeg_cmd_exit(JNIEnv *env, jclass clazz)
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

JNIEXPORT jint JNICALL
Java_tv_danmaku_ijk_media_player_ffmpeg_cmd_FFmpegCmd_nativeExec(JNIEnv *env, jclass clazz,
                                                                 jint argc, jobjectArray argv)
{
    return ijk_ffmpeg_cmd_exec(env, clazz, argc, argv);
}

JNIEXPORT void JNICALL
Java_tv_danmaku_ijk_media_player_ffmpeg_cmd_FFmpegCmd_nativeExit(JNIEnv *env, jclass clazz)
{
    ijk_ffmpeg_cmd_exit(env, clazz);
}

JNIEXPORT jint JNICALL
Java_com_jdpxiaoming_ffmpeg_1cmd_FFmpegCmd_exec(JNIEnv *env, jclass clazz,
                                                jint argc, jobjectArray argv)
{
    return ijk_ffmpeg_cmd_exec(env, clazz, argc, argv);
}

JNIEXPORT void JNICALL
Java_com_jdpxiaoming_ffmpeg_1cmd_FFmpegCmd_exit(JNIEnv *env, jclass clazz)
{
    ijk_ffmpeg_cmd_exit(env, clazz);
}
