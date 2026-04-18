#ifndef IJKPLAYER_FFMPEG_CMD_H
#define IJKPLAYER_FFMPEG_CMD_H

void ffmpeg_progress(float progress);
void ffmpeg_complete(int errorCode);
void ffmpeg_failure(int errorCode);
void ffmpeg_cancel_finish(int errorCode);

#endif
