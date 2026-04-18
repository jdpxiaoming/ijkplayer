#ifndef IJKPLAYER_ANDROID_LOG_H
#define IJKPLAYER_ANDROID_LOG_H

#include <stdarg.h>

#include <android/log.h>
#include <libavutil/log.h>

#define FF_LOG_TAG "IJKFFmpegCmd"

#define LOGE(format, ...) __android_log_print(ANDROID_LOG_ERROR, FF_LOG_TAG, format, ##__VA_ARGS__)
#define LOGI(format, ...) __android_log_print(ANDROID_LOG_INFO, FF_LOG_TAG, format, ##__VA_ARGS__)

#define VLOG(level, tag, ...) ((void)__android_log_vprint(level, tag, __VA_ARGS__))
#define ALOG(level, tag, ...) ((void)__android_log_print(level, tag, __VA_ARGS__))

static void ffp_log_callback_brief(void *ptr, int level, const char *fmt, va_list vl)
{
    int ffplv = ANDROID_LOG_VERBOSE;

    if (level <= AV_LOG_ERROR)
        ffplv = ANDROID_LOG_ERROR;
    else if (level <= AV_LOG_WARNING)
        ffplv = ANDROID_LOG_WARN;
    else if (level <= AV_LOG_INFO)
        ffplv = ANDROID_LOG_INFO;
    else if (level <= AV_LOG_VERBOSE)
        ffplv = ANDROID_LOG_VERBOSE;
    else
        ffplv = ANDROID_LOG_DEBUG;

    if (level <= AV_LOG_INFO)
        VLOG(ffplv, FF_LOG_TAG, fmt, vl);
}

static void ffp_log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    int ffplv = ANDROID_LOG_VERBOSE;
    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    if (level <= AV_LOG_ERROR)
        ffplv = ANDROID_LOG_ERROR;
    else if (level <= AV_LOG_WARNING)
        ffplv = ANDROID_LOG_WARN;
    else if (level <= AV_LOG_INFO)
        ffplv = ANDROID_LOG_INFO;
    else if (level <= AV_LOG_VERBOSE)
        ffplv = ANDROID_LOG_VERBOSE;
    else
        ffplv = ANDROID_LOG_DEBUG;

    va_copy(vl2, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);

    ALOG(ffplv, FF_LOG_TAG, "%s", line);
}

#endif
