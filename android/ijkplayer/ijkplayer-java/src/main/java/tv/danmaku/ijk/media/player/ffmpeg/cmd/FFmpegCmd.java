package tv.danmaku.ijk.media.player.ffmpeg.cmd;

import java.util.concurrent.atomic.AtomicBoolean;

import tv.danmaku.ijk.media.player.IjkLibLoader;
import tv.danmaku.ijk.media.player.IjkMediaPlayer;

public final class FFmpegCmd {
    private static final String TAG = "FFmpegCmd";

    private static volatile boolean sLibLoaded = false;
    private static OnCmdExecListener sListener;
    private static long sDurationMs;
    private static final AtomicBoolean sFinished = new AtomicBoolean(false);
    private static final AtomicBoolean sCancelled = new AtomicBoolean(false);

    private FFmpegCmd() {
    }

    public static void loadLibrariesOnce(IjkLibLoader libLoader) {
        synchronized (FFmpegCmd.class) {
            if (sLibLoaded) {
                return;
            }

            IjkMediaPlayer.loadLibrariesOnce(libLoader);
            if (libLoader == null) {
                System.loadLibrary("ijkffmpegcmd");
            } else {
                libLoader.loadLibrary("ijkffmpegcmd");
            }
            sLibLoaded = true;
        }
    }

    public static int exec(String[] commands, long durationMs, OnCmdExecListener listener) {
        loadLibrariesOnce(null);
        sListener = listener;
        sDurationMs = durationMs;
        sFinished.set(false);
        sCancelled.set(false);

        if (listener != null) {
            listener.onStart();
        }

        return nativeExec(commands.length, commands);
    }

    public static int exec(String[] commands, OnCmdExecListener listener) {
        return exec(commands, 0L, listener);
    }

    public static void exit() {
        loadLibrariesOnce(null);
        sCancelled.set(true);
        nativeExit();
    }

    private static native int nativeExec(int argc, String[] argv);

    private static native void nativeExit();

    static void onNativeExecuted(int ret) {
        FFLog.i(TAG, "onNativeExecuted ret=" + ret);
        if (sCancelled.get()) {
            return;
        }
        if (ret == 0) {
            dispatchCompleteIfNeeded();
        } else {
            dispatchFailureIfNeeded();
        }
    }

    static void onNativeProgress(float progressSeconds) {
        OnCmdExecListener listener = sListener;
        if (listener == null || sFinished.get()) {
            return;
        }

        if (sDurationMs > 0) {
            float totalSeconds = sDurationMs / 1000f;
            listener.onProgress(Math.min(progressSeconds / totalSeconds, 0.95f));
        } else {
            listener.onProgress(progressSeconds);
        }
    }

    static void onNativeComplete(int errorCode) {
        FFLog.i(TAG, "onNativeComplete code=" + errorCode);
        dispatchCompleteIfNeeded();
    }

    static void onNativeFailure(int errorCode) {
        FFLog.e(TAG, "onNativeFailure code=" + errorCode);
        dispatchFailureIfNeeded();
    }

    static void onNativeCancelFinish(int errorCode) {
        FFLog.i(TAG, "onNativeCancelFinish code=" + errorCode);
        if (sFinished.compareAndSet(false, true)) {
            OnCmdExecListener listener = sListener;
            if (listener != null) {
                listener.onCancelFinish();
            }
            sListener = null;
        }
    }

    private static void dispatchCompleteIfNeeded() {
        if (!sFinished.compareAndSet(false, true)) {
            return;
        }

        OnCmdExecListener listener = sListener;
        if (listener != null) {
            if (sDurationMs > 0) {
                listener.onProgress(1.0f);
            }
            listener.onComplete();
        }
        sListener = null;
    }

    private static void dispatchFailureIfNeeded() {
        if (!sFinished.compareAndSet(false, true)) {
            return;
        }

        OnCmdExecListener listener = sListener;
        if (listener != null) {
            listener.onFailure();
        }
        sListener = null;
    }

    public interface OnCmdExecListener {
        void onStart();

        void onFailure();

        void onComplete();

        void onProgress(float progress);

        void onCancelFinish();
    }
}
