package tv.danmaku.ijk.media.player.ffmpeg.cmd;

import android.util.Log;

final class FFLog {
    private FFLog() {
    }

    static void i(String tag, String message) {
        Log.i(tag, message);
    }

    static void e(String tag, String message) {
        Log.e(tag, message);
    }
}
