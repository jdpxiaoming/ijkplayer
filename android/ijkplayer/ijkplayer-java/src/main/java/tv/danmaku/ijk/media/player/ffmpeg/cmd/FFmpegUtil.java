package tv.danmaku.ijk.media.player.ffmpeg.cmd;

import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;

import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

public final class FFmpegUtil {
    private static final String TAG = "FFmpegUtil";

    private static volatile FFmpegUtil sInstance;

    private final Handler mMainHandler = new Handler(Looper.getMainLooper());
    private final ThreadPoolExecutor mExecutor = new ThreadPoolExecutor(
            1,
            1,
            0L,
            TimeUnit.MILLISECONDS,
            new LinkedBlockingQueue<Runnable>()
    );

    public static FFmpegUtil getInstance() {
        if (sInstance == null) {
            synchronized (FFmpegUtil.class) {
                if (sInstance == null) {
                    sInstance = new FFmpegUtil();
                }
            }
        }
        return sInstance;
    }

    private FFmpegUtil() {
    }

    public void enQueueTask(String[] commands, long durationMs, Callback callback) {
        FFmepgTask task = new FFmepgTask(
                SystemClock.elapsedRealtime(),
                durationMs,
                commands,
                callback
        );

        mExecutor.execute(new Runnable() {
            @Override
            public void run() {
                runTask(task);
            }
        });
    }

    public void exec(String[] commands, Callback callback) {
        enQueueTask(commands, 0L, callback);
    }

    public void stopTask() {
        FFLog.i(TAG, "stopTask");
        mExecutor.getQueue().clear();
        FFmpegCmd.exit();
    }

    private void runTask(FFmepgTask task) {
        final Callback callback = task.getCallback();
        if (callback == null) {
            return;
        }

        FFmpegCmd.exec(task.getCommands(), task.getDurationMs(), new FFmpegCmd.OnCmdExecListener() {
            @Override
            public void onStart() {
                dispatch(new Runnable() {
                    @Override
                    public void run() {
                        callback.onStart();
                    }
                });
            }

            @Override
            public void onFailure() {
                dispatch(new Runnable() {
                    @Override
                    public void run() {
                        callback.onFailure();
                    }
                });
            }

            @Override
            public void onComplete() {
                dispatch(new Runnable() {
                    @Override
                    public void run() {
                        callback.onComplete();
                    }
                });
            }

            @Override
            public void onProgress(final float progress) {
                dispatch(new Runnable() {
                    @Override
                    public void run() {
                        callback.onProgress(progress);
                    }
                });
            }

            @Override
            public void onCancelFinish() {
                dispatch(new Runnable() {
                    @Override
                    public void run() {
                        callback.onCancelFinish();
                    }
                });
            }
        });
    }

    private void dispatch(Runnable runnable) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            runnable.run();
        } else {
            mMainHandler.post(runnable);
        }
    }

    public interface Callback {
        void onStart();

        void onFailure();

        void onComplete();

        void onProgress(float progress);

        void onCancelFinish();
    }
}
