package tv.danmaku.ijk.media.player.ffmpeg.cmd;

final class FFmepgTask {
    private final long taskId;
    private final long durationMs;
    private final String[] commands;
    private final FFmpegUtil.Callback callback;

    FFmepgTask(long taskId, long durationMs, String[] commands, FFmpegUtil.Callback callback) {
        this.taskId = taskId;
        this.durationMs = durationMs;
        this.commands = commands;
        this.callback = callback;
    }

    long getTaskId() {
        return taskId;
    }

    long getDurationMs() {
        return durationMs;
    }

    String[] getCommands() {
        return commands;
    }

    FFmpegUtil.Callback getCallback() {
        return callback;
    }
}
