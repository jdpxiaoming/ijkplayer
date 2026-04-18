package tv.danmaku.ijk.media.player.ffmpeg.cmd;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class FFmpegFactory {
    public static int screenWidth = 1280;
    public static int screenHeight = 720;
    public static int titlePicWidth = 200;
    public static int titlePicHeight = 100;
    public static int titleDuration = 3;

    private FFmpegFactory() {
    }

    public static String[] buildSimple(String inputPath, String outputPath) {
        return args("ffmpeg", "-i", inputPath, "-y", outputPath);
    }

    public static String[] buildFlv2Mp4(String inputPath, String outputPath) {
        return args("ffmpeg", "-i", inputPath, "-c:v", "copy", "-c:a", "aac", "-y", outputPath);
    }

    public static String[] buildRtsp2Mp4(String inputPath, String outputPath) {
        return args(
                "ffmpeg",
                "-rtsp_transport", "tcp",
                "-i", inputPath,
                "-map", "0:0",
                "-map", "0:1",
                "-c:v", "copy",
                "-c:a", "aac",
                "-movflags", "+faststart",
                "-f", "mp4",
                "-y",
                outputPath
        );
    }

    public static String[] remuxCopy(String inputPath, String outputPath) {
        return args("ffmpeg", "-i", inputPath, "-c", "copy", "-y", outputPath);
    }

    public static String[] transcode(String inputPath, String outputPath, String videoCodec, String audioCodec) {
        return args(
                "ffmpeg",
                "-i", inputPath,
                "-c:v", videoCodec,
                "-c:a", audioCodec,
                "-y",
                outputPath
        );
    }

    public static String[] scaleVideo(String inputPath, String outputPath, int width, int height) {
        return args(
                "ffmpeg",
                "-i", inputPath,
                "-vf", "scale=" + width + ":" + height,
                "-y",
                outputPath
        );
    }

    public static String[] cutVideoGif(String inputPath, String outputPath, String duration, String fps, String size) {
        return args(
                "ffmpeg",
                "-ss", "00:00:00",
                "-t", duration,
                "-i", inputPath,
                "-s", size,
                "-r", fps,
                "-y",
                outputPath
        );
    }

    public static String[] cutVideoGif(String inputPath, String outputPath, String duration) {
        return args("ffmpeg", "-ss", "00:00:00", "-t", duration, "-i", inputPath, "-y", outputPath);
    }

    public static String[] cutVideoTime(String inputPath, int startMs, int endMs, String outputPath) {
        return args(
                "ffmpeg",
                "-i", inputPath,
                "-ss", formatSeconds(startMs / 1000),
                "-t", formatSeconds((endMs - startMs) / 1000),
                "-c:v", "copy",
                "-c:a", "copy",
                "-y",
                outputPath
        );
    }

    public static String[] captureThumbnail(String inputPath, String outputPath, int second, String size) {
        return args(
                "ffmpeg",
                "-ss", formatSeconds(second),
                "-i", inputPath,
                "-frames:v", "1",
                "-s", size,
                "-y",
                outputPath
        );
    }

    public static String[] addWaterMark(String imagePath, String videoPath, String outputPath) {
        return args(
                "ffmpeg",
                "-i", videoPath,
                "-i", imagePath,
                "-filter_complex", "[1:v]fade=out:st=30:d=1:alpha=1[ov];[0:v][ov]overlay=(main_w-overlay_w)/2:(main_h-overlay_h)/2[v]",
                "-map", "[v]",
                "-map", "0:a?",
                "-c:v", "libx264",
                "-c:a", "copy",
                "-shortest",
                "-preset", "ultrafast",
                "-crf", "28",
                "-y",
                outputPath
        );
    }

    public static String[] addImageMark(String videoPath, String imagePath, String outputPath) {
        return addImageMark(videoPath, imagePath, outputPath, screenWidth, screenHeight);
    }

    public static String[] addImageMark(String videoPath, String imagePath, String outputPath, int width, int height) {
        return args(
                "ffmpeg",
                "-i", videoPath,
                "-i", imagePath,
                "-filter_complex", "[1:v]scale=" + width + ":" + height + "[s];[0:v][s]overlay=0:0",
                "-y",
                outputPath
        );
    }

    public static String[] addTextMark(String videoPath, String text, String outputPath) {
        return args(
                "ffmpeg",
                "-i", videoPath,
                "-vf", "drawtext=text='" + text + "':x=(w-text_w)/2:y=(h-text_h)/2:fontsize=32:fontcolor=white",
                "-y",
                outputPath
        );
    }

    public static String[] addMusic(String videoPath, String musicPath, String outputPath) {
        return args(
                "ffmpeg",
                "-i", videoPath,
                "-i", musicPath,
                "-map", "0:v:0",
                "-map", "1:a:0",
                "-shortest",
                "-y",
                outputPath
        );
    }

    public static String[] concatVideo(String listFilePath, String outputPath) {
        return args("ffmpeg", "-f", "concat", "-safe", "0", "-i", listFilePath, "-c", "copy", "-y", outputPath);
    }

    public static String[] image2mov(String imagePath, String durationSeconds, String outputPath) {
        List<String> args = new ArrayList<>();
        args.add("ffmpeg");
        if (imagePath.toLowerCase(Locale.US).endsWith(".gif")) {
            args.add("-ignore_loop");
            args.add("0");
        } else {
            args.add("-loop");
            args.add("1");
        }
        args.add("-i");
        args.add(imagePath);
        args.add("-r");
        args.add("25");
        args.add("-b:v");
        args.add("200k");
        args.add("-s");
        args.add("640x360");
        args.add("-t");
        args.add(durationSeconds);
        args.add("-y");
        args.add(outputPath);
        return args.toArray(new String[0]);
    }

    public static String[] makeVideo(String titleImagePath, String backgroundImagePath, String musicPath,
                                     String videoPath, String outputPath, int durationSeconds) {
        List<String> args = new ArrayList<>();
        args.add("ffmpeg");
        args.add("-i");
        args.add(videoPath);

        if (!isEmpty(backgroundImagePath)) {
            args.add("-loop");
            args.add("1");
            args.add("-i");
            args.add(backgroundImagePath);
        }
        if (!isEmpty(titleImagePath)) {
            args.add("-i");
            args.add(titleImagePath);
        }
        if (!isEmpty(musicPath)) {
            args.add("-i");
            args.add(musicPath);
        }

        if (!isEmpty(backgroundImagePath) || !isEmpty(titleImagePath)) {
            args.add("-filter_complex");
            if (isEmpty(titleImagePath)) {
                args.add("[1:v]scale=" + screenWidth + ":" + screenHeight + "[bg];[0:v][bg]overlay=0:0");
            } else if (isEmpty(backgroundImagePath)) {
                args.add("overlay=x='if(lte(t," + titleDuration + "),(main_w-overlay_w)/2,NAN)':(main_h-overlay_h)/2");
            } else {
                args.add("[1:v]scale=" + screenWidth + ":" + screenHeight
                        + "[img1];[2:v]scale=" + titlePicWidth + ":" + titlePicHeight
                        + "[img2];[0:v][img1]overlay=0:0[bkg];[bkg][img2]overlay=x='if(lte(t,"
                        + titleDuration + "),(main_w-overlay_w)/2,NAN)':(main_h-overlay_h)/2");
            }
        }

        args.add("-r");
        args.add("25");
        args.add("-b:v");
        args.add("1000k");
        args.add("-s");
        args.add("640x360");
        args.add("-ss");
        args.add("00:00:00");
        args.add("-t");
        args.add(String.valueOf(durationSeconds));
        args.add("-y");
        args.add(outputPath);
        return args.toArray(new String[0]);
    }

    private static String[] args(String... args) {
        return args;
    }

    private static boolean isEmpty(String value) {
        return value == null || value.length() == 0;
    }

    private static String formatSeconds(int seconds) {
        int safeSeconds = Math.max(seconds, 0);
        int hours = safeSeconds / 3600;
        int minutes = (safeSeconds % 3600) / 60;
        int remainingSeconds = safeSeconds % 60;
        return String.format(Locale.US, "%02d:%02d:%02d", hours, minutes, remainingSeconds);
    }
}
