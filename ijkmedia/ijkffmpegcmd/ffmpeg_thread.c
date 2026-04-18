#include "ffmpeg_thread.h"

#include <signal.h>
#include <string.h>

#include "android_log.h"
#include "ffmpeg.h"

static pthread_t s_thread;
static int s_thread_running = 0;
static int s_argc = 0;
static char **s_argv = NULL;
static void (*s_callback)(int ret) = NULL;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *ffmpeg_thread_entry(void *arg)
{
    int result = ffmpeg_exec(s_argc, s_argv);

    pthread_mutex_lock(&s_mutex);
    s_thread_running = 0;
    if (s_callback)
        s_callback(result);
    pthread_mutex_unlock(&s_mutex);

    return NULL;
}

int ffmpeg_thread_run_cmd(int cmdnum, char **argv)
{
    int ret = 0;

    pthread_mutex_lock(&s_mutex);
    if (s_thread_running) {
        pthread_mutex_unlock(&s_mutex);
        LOGE("ffmpeg command is already running");
        return -1;
    }

    s_argc = cmdnum;
    s_argv = argv;
    s_thread_running = 1;

    ret = pthread_create(&s_thread, NULL, ffmpeg_thread_entry, NULL);
    if (ret != 0) {
        s_thread_running = 0;
        pthread_mutex_unlock(&s_mutex);
        LOGE("pthread_create failed: %s", strerror(ret));
        return ret;
    }

    pthread_mutex_unlock(&s_mutex);
    return 0;
}

void ffmpeg_thread_callback(void (*cb)(int ret))
{
    pthread_mutex_lock(&s_mutex);
    s_callback = cb;
    pthread_mutex_unlock(&s_mutex);
}

void ffmpeg_thread_cancel(void)
{
    pthread_t thread = 0;
    int running = 0;

    pthread_mutex_lock(&s_mutex);
    running = s_thread_running;
    thread = s_thread;
    pthread_mutex_unlock(&s_mutex);

    if (!running)
        return;

    sigterm_handler(SIGINT);
    pthread_join(thread, NULL);
}

int ffmpeg_thread_is_running(void)
{
    int running;

    pthread_mutex_lock(&s_mutex);
    running = s_thread_running;
    pthread_mutex_unlock(&s_mutex);

    return running;
}
