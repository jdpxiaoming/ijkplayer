#ifndef IJKPLAYER_FFMPEG_THREAD_H
#define IJKPLAYER_FFMPEG_THREAD_H

#include <pthread.h>

int ffmpeg_thread_run_cmd(int cmdnum, char **argv);
void ffmpeg_thread_callback(void (*cb)(int ret));
void ffmpeg_thread_cancel(void);
int ffmpeg_thread_is_running(void);

#endif
