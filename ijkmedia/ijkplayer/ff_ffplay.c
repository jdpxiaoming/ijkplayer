/*
 * Copyright (c) 2003 Bilibili
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "ff_ffplay.h"

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#if CONFIG_AVDEVICE
#include "libavdevice/avdevice.h"
#endif
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
#include "libavutil/audio_fifo.h"

#if CONFIG_AVFILTER
# include "libavcodec/avcodec.h"
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include "ijksdl/ijksdl_log.h"
#include "ijkavformat/ijkavformat.h"
#include "ff_cmdutils.h"
#include "ff_fferror.h"
#include "ff_ffpipeline.h"
#include "ff_ffpipenode.h"
#include "ff_ffplay_debug.h"
#include "ijkmeta.h"
#include "ijkversion.h"
#include "ijkplayer.h"
#include <stdatomic.h>
#if defined(__ANDROID__)
#include "ijksoundtouch/ijksoundtouch_wrap.h"
#endif

#ifndef AV_CODEC_FLAG2_FAST
#define AV_CODEC_FLAG2_FAST CODEC_FLAG2_FAST
#endif

#ifndef AV_CODEC_CAP_DR1
#define AV_CODEC_CAP_DR1 CODEC_CAP_DR1
#endif

#define CODEC_FLAG_GLOBAL_HEADER AV_CODEC_FLAG_GLOBAL_HEADER

// FIXME: 9 work around NDKr8e or gcc4.7 bug
// isnan() may not recognize some double NAN, so we test both double and float
#if defined(__ANDROID__)
#ifdef isnan
#undef isnan
#endif
#define isnan(x) (isnan((double)(x)) || isnanf((float)(x)))
#endif

#if defined(__ANDROID__)
#define printf(...) ALOGD(__VA_ARGS__)
#endif

#define FFP_IO_STAT_STEP (50 * 1024)

#define FFP_BUF_MSG_PERIOD (3)

// static const AVOption ffp_context_options[] = ...
#include "ff_ffplay_options.h"

static AVPacket flush_pkt;

#if CONFIG_AVFILTER
// FFP_MERGE: opt_add_vfilter
#endif

#define IJKVERSION_GET_MAJOR(x)     ((x >> 16) & 0xFF)
#define IJKVERSION_GET_MINOR(x)     ((x >>  8) & 0xFF)
#define IJKVERSION_GET_MICRO(x)     ((x      ) & 0xFF)

#if CONFIG_AVFILTER
static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}
#endif

static void free_picture(Frame *vp);

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    if (q->abort_request)
       return -1;

#ifdef FFP_MERGE
    pkt1 = av_malloc(sizeof(MyAVPacketList));
#else
    pkt1 = q->recycle_pkt;
    if (pkt1) {
        q->recycle_pkt = pkt1->next;
        q->recycle_count++;
    } else {
        q->alloc_count++;
        pkt1 = av_malloc(sizeof(MyAVPacketList));
    }
#ifdef FFP_SHOW_PKT_RECYCLE
    int total_count = q->recycle_count + q->alloc_count;
    if (!(total_count % 50)) {
        av_log(ffp, AV_LOG_DEBUG, "pkt-recycle \t%d + \t%d = \t%d\n", q->recycle_count, q->alloc_count, total_count);
    }
#endif
#endif
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &flush_pkt)
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);

    q->duration += FFMAX(pkt1->pkt.duration, MIN_PKT_DURATION);

    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);
    SDL_UnlockMutex(q->mutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
#ifdef FFP_MERGE
        av_freep(&pkt);
#else
        pkt->next = q->recycle_pkt;
        q->recycle_pkt = pkt;
#endif
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);

    SDL_LockMutex(q->mutex);
    while(q->recycle_pkt) {
        MyAVPacketList *pkt = q->recycle_pkt;
        if (pkt)
            q->recycle_pkt = pkt->next;
        av_freep(&pkt);
    }
    SDL_UnlockMutex(q->mutex);

    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;

    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
}

static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= FFMAX(pkt1->pkt.duration, MIN_PKT_DURATION);
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
#ifdef FFP_MERGE
            av_free(pkt1);
#else
            pkt1->next = q->recycle_pkt;
            q->recycle_pkt = pkt1;
#endif
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            SDL_CondWait(q->cond, q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int packet_queue_get_or_buffering(FFPlayer *ffp, PacketQueue *q, AVPacket *pkt, int *serial, int *finished)
{
    assert(finished);
    if (!ffp->packet_buffering)
        return packet_queue_get(q, pkt, 1, serial);

    while (1) {
        int new_packet = packet_queue_get(q, pkt, 0, serial);
        if (new_packet < 0)
            return -1;
        else if (new_packet == 0) {
            //if (q->is_buffer_indicator && !*finished)
                //ffp_toggle_buffering(ffp, 1);
                //解决flv 2s延迟???
            new_packet = packet_queue_get(q, pkt, 1, serial);
            if (new_packet < 0)
                return -1;
        }

        if (*finished == *serial) {
            av_packet_unref(pkt);
            continue;
        }
        else
            break;
    }

    return 1;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;

    d->first_frame_decoded_time = SDL_GetTickHR();
    d->first_frame_decoded = 0;

    SDL_ProfilerReset(&d->decode_profiler, -1);
}

static int convert_image(FFPlayer *ffp, AVFrame *src_frame, int64_t src_frame_pts, int width, int height) {
    GetImgInfo *img_info = ffp->get_img_info;
    VideoState *is = ffp->is;
    AVFrame *dst_frame = NULL;
    AVPacket avpkt;
    int got_packet = 0;
    int dst_width = 0;
    int dst_height = 0;
    int bytes = 0;
    void *buffer = NULL;
    char file_path[1024] = {0};
    char file_name[16] = {0};
    int fd = -1;
    int ret = 0;
    int tmp = 0;
    float origin_dar = 0;
    float dar = 0;
    AVRational display_aspect_ratio;
    int file_name_length = 0;

    if (!height || !width || !img_info->width || !img_info->height) {
        ret = -1;
        return ret;
    }

    dar = (float) img_info->width / img_info->height;

    if (is->viddec.avctx) {
        av_reduce(&display_aspect_ratio.num, &display_aspect_ratio.den,
            is->viddec.avctx->width * (int64_t)is->viddec.avctx->sample_aspect_ratio.num,
            is->viddec.avctx->height * (int64_t)is->viddec.avctx->sample_aspect_ratio.den,
            1024 * 1024);

        if (!display_aspect_ratio.num || !display_aspect_ratio.den) {
            origin_dar = (float) width / height;
        } else {
            origin_dar = (float) display_aspect_ratio.num / display_aspect_ratio.den;
        }
    } else {
        ret = -1;
        return ret;
    }

    if ((int)(origin_dar * 100) != (int)(dar * 100)) {
        tmp = img_info->width / origin_dar;
        if (tmp > img_info->height) {
            img_info->width = img_info->height * origin_dar;
        } else {
            img_info->height = tmp;
        }
        av_log(NULL, AV_LOG_INFO, "%s img_info->width = %d, img_info->height = %d\n", __func__, img_info->width, img_info->height);
    }

    dst_width = img_info->width;
    dst_height = img_info->height;

    av_init_packet(&avpkt);
    avpkt.size = 0;
    avpkt.data = NULL;

    if (!img_info->frame_img_convert_ctx) {
        img_info->frame_img_convert_ctx = sws_getContext(width,
		    height,
		    src_frame->format,
		    dst_width,
		    dst_height,
		    AV_PIX_FMT_RGB24,
		    SWS_BICUBIC,
		    NULL,
		    NULL,
		    NULL);

        if (!img_info->frame_img_convert_ctx) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s sws_getContext failed\n", __func__);
            goto fail0;
        }
    }

    if (!img_info->frame_img_codec_ctx) {
        AVCodec *image_codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
        if (!image_codec) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s avcodec_find_encoder failed\n", __func__);
            goto fail0;
        }
	    img_info->frame_img_codec_ctx = avcodec_alloc_context3(image_codec);
        if (!img_info->frame_img_codec_ctx) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s avcodec_alloc_context3 failed\n", __func__);
            goto fail0;
        }
        img_info->frame_img_codec_ctx->bit_rate = ffp->stat.bit_rate;
        img_info->frame_img_codec_ctx->width = dst_width;
        img_info->frame_img_codec_ctx->height = dst_height;
        img_info->frame_img_codec_ctx->pix_fmt = AV_PIX_FMT_RGB24;
        img_info->frame_img_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        img_info->frame_img_codec_ctx->time_base.num = ffp->is->video_st->time_base.num;
        img_info->frame_img_codec_ctx->time_base.den = ffp->is->video_st->time_base.den;
        avcodec_open2(img_info->frame_img_codec_ctx, image_codec, NULL);
    }

    dst_frame = av_frame_alloc();
    if (!dst_frame) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_frame_alloc failed\n", __func__);
        goto fail0;
    }
    bytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dst_width, dst_height, 1);
	buffer = (uint8_t *) av_malloc(bytes * sizeof(uint8_t));
    if (!buffer) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_image_get_buffer_size failed\n", __func__);
        goto fail1;
    }

    dst_frame->format = AV_PIX_FMT_RGB24;
    dst_frame->width = dst_width;
    dst_frame->height = dst_height;

    ret = av_image_fill_arrays(dst_frame->data,
            dst_frame->linesize,
            buffer,
            AV_PIX_FMT_RGB24,
            dst_width,
            dst_height,
            1);

    if (ret < 0) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s av_image_fill_arrays failed\n", __func__);
        goto fail2;
    }

    ret = sws_scale(img_info->frame_img_convert_ctx,
            (const uint8_t * const *) src_frame->data,
            src_frame->linesize,
            0,
            src_frame->height,
            dst_frame->data,
            dst_frame->linesize);

    if (ret <= 0) {
        ret = -1;
        av_log(NULL, AV_LOG_ERROR, "%s sws_scale failed\n", __func__);
        goto fail2;
    }

    ret = avcodec_encode_video2(img_info->frame_img_codec_ctx, &avpkt, dst_frame, &got_packet);

    if (ret >= 0 && got_packet > 0) {
        strcpy(file_path, img_info->img_path);
        strcat(file_path, "/");
        sprintf(file_name, "%lld", src_frame_pts);
        strcat(file_name, ".png");
        strcat(file_path, file_name);

        fd = open(file_path, O_RDWR | O_TRUNC | O_CREAT, 0600);
        if (fd < 0) {
            ret = -1;
            av_log(NULL, AV_LOG_ERROR, "%s open path = %s failed %s\n", __func__, file_path, strerror(errno));
            goto fail2;
        }
        write(fd, avpkt.data, avpkt.size);
        close(fd);

        img_info->count--;

        file_name_length = (int)strlen(file_name) + 1;

        if (img_info->count <= 0)
            ffp_notify_msg4(ffp, FFP_MSG_GET_IMG_STATE, (int) src_frame_pts, 1, file_name, file_name_length);
        else
            ffp_notify_msg4(ffp, FFP_MSG_GET_IMG_STATE, (int) src_frame_pts, 0, file_name, file_name_length);

        ret = 0;
    }

fail2:
    av_free(buffer);
fail1:
    av_frame_free(&dst_frame);
fail0:
    av_packet_unref(&avpkt);

    return ret;
}
static int decoder_decode_frame(FFPlayer *ffp, Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        // Special handling for HEVC (H.265) to prevent assertion failures
                        if (d->avctx->codec_id == AV_CODEC_ID_HEVC) {
                            // For HEVC, use a more careful approach to avoid assertion failures
                            ret = avcodec_receive_frame(d->avctx, frame);
                            
                            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                                av_log(ffp, AV_LOG_WARNING, "HEVC decoder error (%d), resetting decoder state", ret);
                                avcodec_flush_buffers(d->avctx);
                                ret = AVERROR(EAGAIN);
                            } else if (ret >= 0) {
                                ffp->stat.vdps = SDL_SpeedSamplerAdd(&ffp->vdps_sampler, FFP_SHOW_VDPS_AVCODEC, "vdps[avcodec]");
                                if (ffp->decoder_reorder_pts == -1) {
                                    frame->pts = frame->best_effort_timestamp;
                                } else if (!ffp->decoder_reorder_pts) {
                                    frame->pts = frame->pkt_dts;
                                }
                            }
                        } else {
                            // Normal handling for non-HEVC codecs
                            ret = avcodec_receive_frame(d->avctx, frame);
                            if (ret >= 0) {
                                ffp->stat.vdps = SDL_SpeedSamplerAdd(&ffp->vdps_sampler, FFP_SHOW_VDPS_AVCODEC, "vdps[avcodec]");
                                if (ffp->decoder_reorder_pts == -1) {
                                    frame->pts = frame->best_effort_timestamp;
                                } else if (!ffp->decoder_reorder_pts) {
                                    frame->pts = frame->pkt_dts;
                                }
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational){1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, av_codec_get_pkt_timebase(d->avctx), tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                    default:
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (packet_queue_get_or_buffering(ffp, d->queue, &pkt, &d->pkt_serial, &d->finished) < 0)
                    return -1;
            }
        } while (d->queue->serial != d->pkt_serial);

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                       d->packet_pending = 1;
                       av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);
    SDL_VoutUnrefYUVOverlay(vp->bmp);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    return 0;
}

static void frame_queue_destory(FrameQueue *f)
{
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
        free_picture(vp);
    }
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f)
{
    if (++f->windex == f->max_size)
        f->windex = 0;
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

static void frame_queue_next(FrameQueue *f)
{
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;
}

/* return last shown position */
#ifdef FFP_MERGE
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}
#endif

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, NULL);
    d->decoder_tid = NULL;
    packet_queue_flush(d->queue);
}

// FFP_MERGE: fill_rectangle
// FFP_MERGE: fill_border
// FFP_MERGE: ALPHA_BLEND
// FFP_MERGE: RGBA_IN
// FFP_MERGE: YUVA_IN
// FFP_MERGE: YUVA_OUT
// FFP_MERGE: BPP
// FFP_MERGE: blend_subrect

static void free_picture(Frame *vp)
{
    if (vp->bmp) {
        SDL_VoutFreeYUVOverlay(vp->bmp);
        vp->bmp = NULL;
    }
}

// FFP_MERGE: realloc_texture
// FFP_MERGE: calculate_display_rect
// FFP_MERGE: upload_texture
// FFP_MERGE: video_image_display

static size_t parse_ass_subtitle(const char *ass, char *output)
{
    char *tok = NULL;
    tok = strchr(ass, ':'); if (tok) tok += 1; // skip event
    tok = strchr(tok, ','); if (tok) tok += 1; // skip layer
    tok = strchr(tok, ','); if (tok) tok += 1; // skip start_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip end_time
    tok = strchr(tok, ','); if (tok) tok += 1; // skip style
    tok = strchr(tok, ','); if (tok) tok += 1; // skip name
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_l
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_r
    tok = strchr(tok, ','); if (tok) tok += 1; // skip margin_v
    tok = strchr(tok, ','); if (tok) tok += 1; // skip effect
    if (tok) {
        char *text = tok;
        size_t idx = 0;
        do {
            char *found = strstr(text, "\\N");
            if (found) {
                size_t n = found - text;
                memcpy(output+idx, text, n);
                output[idx + n] = '\n';
                idx = n + 1;
                text = found + 2;
            }
            else {
                size_t left_text_len = strlen(text);
                memcpy(output+idx, text, left_text_len);
                if (output[idx + left_text_len - 1] == '\n')
                    output[idx + left_text_len - 1] = '\0';
                else
                    output[idx + left_text_len] = '\0';
                break;
            }
        } while(1);
        return strlen(output) + 1;
    }
    return 0;
}

static void video_image_display2(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    Frame *vp;
    Frame *sp = NULL;

    vp = frame_queue_peek_last(&is->pictq);

    if (vp->bmp) {
        if (is->subtitle_st) {
            if (frame_queue_nb_remaining(&is->subpq) > 0) {
                sp = frame_queue_peek(&is->subpq);

                if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                    if (!sp->uploaded) {
                        if (sp->sub.num_rects > 0) {
                            char buffered_text[4096];
                            if (sp->sub.rects[0]->text) {
                                strncpy(buffered_text, sp->sub.rects[0]->text, 4096);
                            }
                            else if (sp->sub.rects[0]->ass) {
                                parse_ass_subtitle(sp->sub.rects[0]->ass, buffered_text);
                            }
                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, buffered_text, sizeof(buffered_text));
                        }
                        sp->uploaded = 1;
                    }
                }
            }
        }
        if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
            if (!ffp->first_video_frame_rendered) {
                ffp->first_video_frame_rendered = 1;
                ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
            }
            while (is->pause_req && !is->abort_request) {
                SDL_Delay(20);
            }
        }
        SDL_VoutDisplayYUVOverlay(ffp->vout, vp->bmp);
        ffp->stat.vfps = SDL_SpeedSamplerAdd(&ffp->vfps_sampler, FFP_SHOW_VFPS_FFPLAY, "vfps[ffplay]");
        if (!ffp->first_video_frame_rendered) {
            ffp->first_video_frame_rendered = 1;
            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_RENDERING_START);
        }

        if (is->latest_video_seek_load_serial == vp->serial) {
            int latest_video_seek_load_serial = __atomic_exchange_n(&(is->latest_video_seek_load_serial), -1, memory_order_seq_cst);
            if (latest_video_seek_load_serial == vp->serial) {
                ffp->stat.latest_seek_load_duration = (av_gettime() - is->latest_seek_load_start_at) / 1000;
                if (ffp->av_sync_type == AV_SYNC_VIDEO_MASTER) {
                    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_SEEK_RENDERING_START, 1);
                } else {
                    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_SEEK_RENDERING_START, 0);
                }
            }
        }
    }
}

// FFP_MERGE: compute_mod
// FFP_MERGE: video_audio_display

static void stream_component_close(FFPlayer *ffp, int stream_index)
{
    VideoState *is = ffp->is;
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_AoutCloseAudio(ffp->aout);

        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

#ifdef FFP_MERGE
        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
#endif
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

static void stream_close(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    packet_queue_abort(&is->videoq);
    packet_queue_abort(&is->audioq);
    av_log(NULL, AV_LOG_DEBUG, "wait for read_tid\n");
    SDL_WaitThread(is->read_tid, NULL);

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(ffp, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(ffp, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(ffp, is->subtitle_stream);

    avformat_close_input(&is->ic);

    av_log(NULL, AV_LOG_DEBUG, "wait for video_refresh_tid\n");
    SDL_WaitThread(is->video_refresh_tid, NULL);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    SDL_DestroyCond(is->audio_accurate_seek_cond);
    SDL_DestroyCond(is->video_accurate_seek_cond);
    SDL_DestroyCond(is->continue_read_thread);
    SDL_DestroyMutex(is->accurate_seek_mutex);
    SDL_DestroyMutex(is->play_mutex);
#if !CONFIG_AVFILTER
    sws_freeContext(is->img_convert_ctx);
#endif
#ifdef FFP_MERGE
    sws_freeContext(is->sub_convert_ctx);
#endif

#if defined(__ANDROID__)
    if (ffp->soundtouch_enable && is->handle != NULL) {
        ijk_soundtouch_destroy(is->handle);
    }
#endif
    if (ffp->get_img_info) {
        if (ffp->get_img_info->frame_img_convert_ctx) {
            sws_freeContext(ffp->get_img_info->frame_img_convert_ctx);
        }
        if (ffp->get_img_info->frame_img_codec_ctx) {
            avcodec_free_context(&ffp->get_img_info->frame_img_codec_ctx);
        }
        av_freep(&ffp->get_img_info->img_path);
        av_freep(&ffp->get_img_info);
    }
    av_free(is->filename);
    av_free(is);
    ffp->is = NULL;
}

// FFP_MERGE: do_exit
// FFP_MERGE: sigterm_handler
// FFP_MERGE: video_open
// FFP_MERGE: video_display

/* display the current picture, if any */
static void video_display2(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (is->video_st)
        video_image_display2(ffp);
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

//设置时钟倍速播放或者减速. 
static void check_external_clock_speed(VideoState *is) {
   if ((is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) ||
       (is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES)) {
       set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
   } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
              (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
       set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
   } else {
       double speed = is->extclk.speed;
       if (speed != 1.0)
           set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
   }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    if (!is->seek_req) {
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause_l(FFPlayer *ffp, int pause_on)
{
    VideoState *is = ffp->is;
    if (is->paused && !pause_on) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;

#ifdef FFP_MERGE
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
#endif
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
    } else {
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    if (is->step && (is->pause_req || is->buffering_on)) {
        is->paused = is->vidclk.paused = is->extclk.paused = pause_on;
    } else {
        is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = pause_on;
        SDL_AoutPauseAudio(ffp->aout, pause_on);
    }
}

static void stream_update_pause_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (!is->step && (is->pause_req || is->buffering_on)) {
        stream_toggle_pause_l(ffp, 1);
    } else {
        stream_toggle_pause_l(ffp, 0);
    }
}

static void toggle_pause_l(FFPlayer *ffp, int pause_on)
{
    VideoState *is = ffp->is;
    if (is->pause_req && !pause_on) {
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
        set_clock(&is->audclk, get_clock(&is->audclk), is->audclk.serial);
    }
    is->pause_req = pause_on;
    ffp->auto_resume = !pause_on;
    stream_update_pause_l(ffp);
    is->step = 0;
}

static void toggle_pause(FFPlayer *ffp, int pause_on)
{
    SDL_LockMutex(ffp->is->play_mutex);
    toggle_pause_l(ffp, pause_on);
    SDL_UnlockMutex(ffp->is->play_mutex);
}

// FFP_MERGE: toggle_mute
// FFP_MERGE: update_volume

static void step_to_next_frame_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    is->step = 1;
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause_l(ffp, 0);
}

static double compute_target_delay(FFPlayer *ffp, double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        /* -- by bbcallen: replace is->max_frame_duration with AV_NOSYNC_THRESHOLD */
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    if (ffp) {
        ffp->stat.avdelay = delay;
        ffp->stat.avdiff  = diff;
    }
#ifdef FFP_SHOW_AUDIO_DELAY
    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
            delay, -diff);
#endif

    return delay;
}

/**
 * @brief返回当前帧的pts等待时间. 
 * 
 * @param is 
 * @param vp 
 * @param nextvp 
 * @return double 
 */
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp, int delay_forbidden) {
    if(delay_forbidden >0){
      //ALOGD("vp_duration current delay_forbidden > 0 is true!");
		  return vp->duration;
	  }
	  //ALOGD("vp_duration current delay_forbidden > 0 is false!");
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(FFPlayer *opaque, double *remaining_time)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (!ffp->display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + ffp->rdftspeed < time) {
            video_display2(ffp);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + ffp->rdftspeed - time);
    }

  //放开倍速2.0限制. 
	if (ffp->pf_playback_rate > ADJUST_POLLING_RATE_THRESHOLD &&
			!is->audio_st) {
			*remaining_time = VIDEO_ONLY_FAST_POLLING_RATE;
		}
   
    if (is->video_st) {
retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp, ffp->delay_forbidden);
            delay = compute_target_delay(ffp, last_duration, is);

            time= av_gettime_relative()/1000000.0;
            if (isnan(is->frame_timer) || time < is->frame_timer)
                is->frame_timer = time;
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp, ffp->delay_forbidden);
                if(!is->step && (ffp->framedrop > 0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial
                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))
                    {
                        if (sp->uploaded) {
                            ffp_notify_msg4(ffp, FFP_MSG_TIMED_TEXT, 0, 0, "", 1);
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            SDL_LockMutex(ffp->is->play_mutex);
            if (is->step) {
                is->step = 0;
                if (!is->paused)
                    stream_update_pause_l(ffp);
            }
            SDL_UnlockMutex(ffp->is->play_mutex);
        }
display:
        /* display picture */
        if (!ffp->display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display2(ffp);
    }
    is->force_refresh = 0;
    if (ffp->show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize __unused;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
#ifdef FFP_MERGE
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
#else
            sqsize = 0;
#endif
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

/* allocate a picture (needs to do that in main thread to avoid
   potential locking problems */
static void alloc_picture(FFPlayer *ffp, int frame_format)
{
    VideoState *is = ffp->is;
    Frame *vp;
#ifdef FFP_MERGE
    int sdl_format;
#endif

    vp = &is->pictq.queue[is->pictq.windex];

    free_picture(vp);

#ifdef FFP_MERGE
    video_open(is, vp);
#endif

    SDL_VoutSetOverlayFormat(ffp->vout, ffp->overlay_format);
    vp->bmp = SDL_Vout_CreateOverlay(vp->width, vp->height,
                                   frame_format,
                                   ffp->vout);
#ifdef FFP_MERGE
    if (vp->format == AV_PIX_FMT_YUV420P)
        sdl_format = SDL_PIXELFORMAT_YV12;
    else
        sdl_format = SDL_PIXELFORMAT_ARGB8888;

    if (realloc_texture(&vp->bmp, sdl_format, vp->width, vp->height, SDL_BLENDMODE_NONE, 0) < 0) {
#else
    /* RV16, RV32 contains only one plane */
    if (!vp->bmp || (!vp->bmp->is_private && vp->bmp->pitches[0] < vp->width)) {
#endif
        /* SDL allocates a buffer smaller than requested if the video
         * overlay hardware is unable to support the requested size. */
        av_log(NULL, AV_LOG_FATAL,
               "Error: the video system does not support an image\n"
                        "size of %dx%d pixels. Try using -lowres or -vf \"scale=w:h\"\n"
                        "to reduce the image size.\n", vp->width, vp->height );
        free_picture(vp);
    }

    SDL_LockMutex(is->pictq.mutex);
    vp->allocated = 1;
    SDL_CondSignal(is->pictq.cond);
    SDL_UnlockMutex(is->pictq.mutex);
}

static int queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    VideoState *is = ffp->is;
    Frame *vp;
    int video_accurate_seek_fail = 0;
    int64_t video_seek_pos = 0;
    int64_t now = 0;
    int64_t deviation = 0;

    int64_t deviation2 = 0;
    int64_t deviation3 = 0;

    if (ffp->enable_accurate_seek && is->video_accurate_seek_req && !is->seek_req) {
        if (!isnan(pts)) {
            video_seek_pos = is->seek_pos;
            is->accurate_seek_vframe_pts = pts * 1000 * 1000;
            deviation = llabs((int64_t)(pts * 1000 * 1000) - is->seek_pos);
            if ((pts * 1000 * 1000 < is->seek_pos) || deviation > MAX_DEVIATION) {
                now = av_gettime_relative() / 1000;
                if (is->drop_vframe_count == 0) {
                    SDL_LockMutex(is->accurate_seek_mutex);
                    if (is->accurate_seek_start_time <= 0 && (is->audio_stream < 0 || is->audio_accurate_seek_req)) {
                        is->accurate_seek_start_time = now;
                    }
                    SDL_UnlockMutex(is->accurate_seek_mutex);
                    av_log(NULL, AV_LOG_INFO, "video accurate_seek start, is->seek_pos=%lld, pts=%lf, is->accurate_seek_time = %lld\n", is->seek_pos, pts, is->accurate_seek_start_time);
                }
                is->drop_vframe_count++;

                while (is->audio_accurate_seek_req && !is->abort_request) {
                    int64_t apts = is->accurate_seek_aframe_pts ;
                    deviation2 = apts - pts * 1000 * 1000;
                    deviation3 = apts - is->seek_pos;

                    if (deviation2 > -100 * 1000 && deviation3 < 0) {
                        break;
                    } else {
                        av_usleep(20 * 1000);
                    }
                    now = av_gettime_relative() / 1000;
                    if ((now - is->accurate_seek_start_time) > ffp->accurate_seek_timeout) {
                        break;
                    }
                }

                if ((now - is->accurate_seek_start_time) <= ffp->accurate_seek_timeout) {
                    return 1;  // drop some old frame when do accurate seek
                } else {
                    av_log(NULL, AV_LOG_WARNING, "video accurate_seek is error, is->drop_vframe_count=%d, now = %lld, pts = %lf\n", is->drop_vframe_count, now, pts);
                    video_accurate_seek_fail = 1;  // if KEY_FRAME interval too big, disable accurate seek
                }
            } else {
                av_log(NULL, AV_LOG_INFO, "video accurate_seek is ok, is->drop_vframe_count =%d, is->seek_pos=%lld, pts=%lf\n", is->drop_vframe_count, is->seek_pos, pts);
                if (video_seek_pos == is->seek_pos) {
                    is->drop_vframe_count       = 0;
                    SDL_LockMutex(is->accurate_seek_mutex);
                    is->video_accurate_seek_req = 0;
                    SDL_CondSignal(is->audio_accurate_seek_cond);
                    if (video_seek_pos == is->seek_pos && is->audio_accurate_seek_req && !is->abort_request) {
                        SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
                    } else {
                        ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
                    }
                    if (video_seek_pos != is->seek_pos && !is->abort_request) {
                        is->video_accurate_seek_req = 1;
                        SDL_UnlockMutex(is->accurate_seek_mutex);
                        return 1;
                    }

                    SDL_UnlockMutex(is->accurate_seek_mutex);
                }
            }
        } else {
            video_accurate_seek_fail = 1;
        }

        if (video_accurate_seek_fail) {
            is->drop_vframe_count = 0;
            SDL_LockMutex(is->accurate_seek_mutex);
            is->video_accurate_seek_req = 0;
            SDL_CondSignal(is->audio_accurate_seek_cond);
            if (is->audio_accurate_seek_req && !is->abort_request) {
                SDL_CondWaitTimeout(is->video_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
            } else {
                if (!isnan(pts)) {
                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(pts * 1000));
                } else {
                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, 0);
                }
            }
            SDL_UnlockMutex(is->accurate_seek_mutex);
        }
        is->accurate_seek_start_time = 0;
        video_accurate_seek_fail = 0;
        is->accurate_seek_vframe_pts = 0;
    }

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
#ifdef FFP_MERGE
    vp->uploaded = 0;
#endif

    /* alloc or resize hardware picture buffer */
    if (!vp->bmp || !vp->allocated ||
        vp->width  != src_frame->width ||
        vp->height != src_frame->height ||
        vp->format != src_frame->format) {

        if (vp->width != src_frame->width || vp->height != src_frame->height)
            ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, src_frame->width, src_frame->height);

        vp->allocated = 0;
        vp->width = src_frame->width;
        vp->height = src_frame->height;
        vp->format = src_frame->format;

        /* the allocation must be done in the main thread to avoid
           locking problems. */
        alloc_picture(ffp, src_frame->format);

        if (is->videoq.abort_request)
            return -1;
    }

    /* if the frame is not skipped, then display it */
    if (vp->bmp) {
        /* get a pointer on the bitmap */
        SDL_VoutLockYUVOverlay(vp->bmp);

#ifdef FFP_MERGE
#if CONFIG_AVFILTER
        // FIXME use direct rendering
        av_image_copy(data, linesize, (const uint8_t **)src_frame->data, src_frame->linesize,
                        src_frame->format, vp->width, vp->height);
#else
        // sws_getCachedContext(...);
#endif
#endif
        // FIXME: set swscale options
        if (SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame) < 0) {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            exit(1);
        }
        /* update the bitmap content */
        SDL_VoutUnlockYUVOverlay(vp->bmp);

        vp->pts = pts;
        vp->duration = duration;
        vp->pos = pos;
        vp->serial = serial;
        vp->sar = src_frame->sample_aspect_ratio;
        vp->bmp->sar_num = vp->sar.num;
        vp->bmp->sar_den = vp->sar.den;

#ifdef FFP_MERGE
        av_frame_move_ref(vp->frame, src_frame);
#endif
        frame_queue_push(&is->pictq);
        if (!is->viddec.first_frame_decoded) {
            ALOGD("Video: first frame decoded\n");
            ffp_notify_msg1(ffp, FFP_MSG_VIDEO_DECODED_START);
            is->viddec.first_frame_decoded_time = SDL_GetTickHR();
            is->viddec.first_frame_decoded = 1;
        }
    }
    return 0;
}

static int get_video_frame(FFPlayer *ffp, AVFrame *frame)
{
    VideoState *is = ffp->is;
    int got_picture;

    ffp_video_statistic_l(ffp);
    if ((got_picture = decoder_decode_frame(ffp, &is->viddec, frame, NULL)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (ffp->framedrop>0 || (ffp->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            ffp->stat.decode_frame_count++;
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    is->continuous_frame_drops_early++;
                    if (is->continuous_frame_drops_early > ffp->framedrop) {
                        is->continuous_frame_drops_early = 0;
                    } else {
                        ffp->stat.drop_frame_count++;
                        ffp->stat.drop_frame_rate = (float)(ffp->stat.drop_frame_count) / (float)(ffp->stat.decode_frame_count);
                        av_frame_unref(frame);
                        got_picture = 0;
                    }
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, NULL);
fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(FFPlayer *ffp, AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_BGRA, AV_PIX_FMT_NONE };
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;

    while ((e = av_dict_get(ffp->sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (ffp->autorotate) {
        double theta  = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);
            INSERT_FILT("vflip", NULL);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        char setpts_buf[256];
        float rate = 1.0f / ffp->pf_playback_rate;
        rate = av_clipf_c(rate, 0.5f, 2.0f);
        av_log(ffp, AV_LOG_INFO, "vf_rate=%f(1/%f)\n", ffp->pf_playback_rate, rate);
        snprintf(setpts_buf, sizeof(setpts_buf), "%f*PTS", rate);
        INSERT_FILT("setpts", setpts_buf);
    }
#endif

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

static int configure_audio_filters(FFPlayer *ffp, const char *afilters, int force_output_format)
{
    VideoState *is = ffp->is;
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;
    char afilters_args[4096];

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);

    while ((e = av_dict_get(ffp->swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    afilters_args[0] = 0;
    if (afilters)
        snprintf(afilters_args, sizeof(afilters_args), "%s", afilters);

#ifdef FFP_AVFILTER_PLAYBACK_RATE
    if (fabsf(ffp->pf_playback_rate) > 0.00001 &&
        fabsf(ffp->pf_playback_rate - 1.0f) > 0.00001) {
        if (afilters_args[0])
            av_strlcatf(afilters_args, sizeof(afilters_args), ",");

        av_log(ffp, AV_LOG_INFO, "af_rate=%f\n", ffp->pf_playback_rate);
        av_strlcatf(afilters_args, sizeof(afilters_args), "atempo=%f", ffp->pf_playback_rate);
    }
#endif

    if ((ret = configure_filtergraph(is->agraph, afilters_args[0] ? afilters_args : NULL, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}
#endif  /* CONFIG_AVFILTER */

static int audio_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFrame *frame = av_frame_alloc();
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;
    AVRational tb;
    int ret = 0;
    int audio_accurate_seek_fail = 0;
    int64_t audio_seek_pos = 0;
    double frame_pts = 0;
    double audio_clock = 0;
    int64_t now = 0;
    double samples_duration = 0;
    int64_t deviation = 0;
    int64_t deviation2 = 0;
    int64_t deviation3 = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        ffp_audio_statistic_l(ffp);
        if ((got_frame = decoder_decode_frame(ffp, &is->auddec, frame, NULL)) < 0)
            goto the_end;

        if (got_frame) {
                tb = (AVRational){1, frame->sample_rate};
                if (ffp->enable_accurate_seek && is->audio_accurate_seek_req && !is->seek_req) {
                    frame_pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                    now = av_gettime_relative() / 1000;
                    if (!isnan(frame_pts)) {
                        samples_duration = (double) frame->nb_samples / frame->sample_rate;
                        audio_clock = frame_pts + samples_duration;
                        is->accurate_seek_aframe_pts = audio_clock * 1000 * 1000;
                        audio_seek_pos = is->seek_pos;
                        deviation = llabs((int64_t)(audio_clock * 1000 * 1000) - is->seek_pos);
                        if ((audio_clock * 1000 * 1000 < is->seek_pos ) || deviation > MAX_DEVIATION) {
                            if (is->drop_aframe_count == 0) {
                                SDL_LockMutex(is->accurate_seek_mutex);
                                if (is->accurate_seek_start_time <= 0 && (is->video_stream < 0 || is->video_accurate_seek_req)) {
                                    is->accurate_seek_start_time = now;
                                }
                                SDL_UnlockMutex(is->accurate_seek_mutex);
                                av_log(NULL, AV_LOG_INFO, "audio accurate_seek start, is->seek_pos=%lld, audio_clock=%lf, is->accurate_seek_start_time = %lld\n", is->seek_pos, audio_clock, is->accurate_seek_start_time);
                            }
                            is->drop_aframe_count++;
                            while (is->video_accurate_seek_req && !is->abort_request) {
                                int64_t vpts = is->accurate_seek_vframe_pts;
                                deviation2 = vpts  - audio_clock * 1000 * 1000;
                                deviation3 = vpts  - is->seek_pos;
                                if (deviation2 > -100 * 1000 && deviation3 < 0) {

                                    break;
                                } else {
                                    av_usleep(20 * 1000);
                                }
                                now = av_gettime_relative() / 1000;
                                if ((now - is->accurate_seek_start_time) > ffp->accurate_seek_timeout) {
                                    break;
                                }
                            }

                            if(!is->video_accurate_seek_req && is->video_stream >= 0 && audio_clock * 1000 * 1000 > is->accurate_seek_vframe_pts) {
                                audio_accurate_seek_fail = 1;
                            } else {
                                now = av_gettime_relative() / 1000;
                                if ((now - is->accurate_seek_start_time) <= ffp->accurate_seek_timeout) {
                                    av_frame_unref(frame);
                                    continue;  // drop some old frame when do accurate seek
                                } else {
                                    audio_accurate_seek_fail = 1;
                                }
                            }
                        } else {
                            if (audio_seek_pos == is->seek_pos) {
                                av_log(NULL, AV_LOG_INFO, "audio accurate_seek is ok, is->drop_aframe_count=%d, audio_clock = %lf\n", is->drop_aframe_count, audio_clock);
                                is->drop_aframe_count       = 0;
                                SDL_LockMutex(is->accurate_seek_mutex);
                                is->audio_accurate_seek_req = 0;
                                SDL_CondSignal(is->video_accurate_seek_cond);
                                if (audio_seek_pos == is->seek_pos && is->video_accurate_seek_req && !is->abort_request) {
                                    SDL_CondWaitTimeout(is->audio_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
                                } else {
                                    ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(audio_clock * 1000));
                                }

                                if (audio_seek_pos != is->seek_pos && !is->abort_request) {
                                    is->audio_accurate_seek_req = 1;
                                    SDL_UnlockMutex(is->accurate_seek_mutex);
                                    av_frame_unref(frame);
                                    continue;
                                }

                                SDL_UnlockMutex(is->accurate_seek_mutex);
                            }
                        }
                    } else {
                        audio_accurate_seek_fail = 1;
                    }
                    if (audio_accurate_seek_fail) {
                        av_log(NULL, AV_LOG_INFO, "audio accurate_seek is error, is->drop_aframe_count=%d, now = %lld, audio_clock = %lf\n", is->drop_aframe_count, now, audio_clock);
                        is->drop_aframe_count       = 0;
                        SDL_LockMutex(is->accurate_seek_mutex);
                        is->audio_accurate_seek_req = 0;
                        SDL_CondSignal(is->video_accurate_seek_cond);
                        if (is->video_accurate_seek_req && !is->abort_request) {
                            SDL_CondWaitTimeout(is->audio_accurate_seek_cond, is->accurate_seek_mutex, ffp->accurate_seek_timeout);
                        } else {
                            ffp_notify_msg2(ffp, FFP_MSG_ACCURATE_SEEK_COMPLETE, (int)(audio_clock * 1000));
                        }
                        SDL_UnlockMutex(is->accurate_seek_mutex);
                    }
                    is->accurate_seek_start_time = 0;
                    audio_accurate_seek_fail = 0;
                }

#if CONFIG_AVFILTER
                dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

                reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   frame->format, frame->channels)    ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial        ||
                    ffp->af_changed;

                if (reconfigure) {
                    SDL_LockMutex(ffp->af_mutex);
                    ffp->af_changed = 0;
                    char buf1[1024], buf2[1024];
                    av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                    av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                    av_log(NULL, AV_LOG_DEBUG,
                           "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                           is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                           frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                    is->audio_filter_src.fmt            = frame->format;
                    is->audio_filter_src.channels       = frame->channels;
                    is->audio_filter_src.channel_layout = dec_channel_layout;
                    is->audio_filter_src.freq           = frame->sample_rate;
                    last_serial                         = is->auddec.pkt_serial;

                    if ((ret = configure_audio_filters(ffp, ffp->afilters, 1)) < 0) {
                        SDL_UnlockMutex(ffp->af_mutex);
                        goto the_end;
                    }
                    SDL_UnlockMutex(ffp->af_mutex);
                }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

static int decoder_start(Decoder *d, int (*fn)(void *), void *arg, const char *name)
{
    packet_queue_start(d->queue);
    d->decoder_tid = SDL_CreateThreadEx(&d->_decoder_tid, fn, arg, name);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int ffplay_video_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFrame *frame = av_frame_alloc();
    int delay_forbidden = ffp->delay_forbidden;
    double pts;
    double duration;
    int ret;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate;
    if(delay_forbidden == 0){
      frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);
    } 
    int64_t dst_pts = -1;
    int64_t last_dst_pts = -1;
    int retry_convert_image = 0;
    int convert_frame_count = 0;

#if CONFIG_AVFILTER
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;
    if (!graph) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }

#else
    ffp_notify_msg2(ffp, FFP_MSG_VIDEO_ROTATION_CHANGED, ffp_get_video_rotate_degrees(ffp));
#endif

    if (!frame) {
#if CONFIG_AVFILTER
        avfilter_graph_free(&graph);
#endif
        return AVERROR(ENOMEM);
    }

    for (;;) {
        ret = get_video_frame(ffp, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

        if (ffp->get_frame_mode) {
            if (!ffp->get_img_info || ffp->get_img_info->count <= 0) {
                av_frame_unref(frame);
                continue;
            }

            last_dst_pts = dst_pts;

            if (dst_pts < 0) {
                dst_pts = ffp->get_img_info->start_time;
            } else {
                dst_pts += (ffp->get_img_info->end_time - ffp->get_img_info->start_time) / (ffp->get_img_info->num - 1);
            }

            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            pts = pts * 1000;
            if (pts >= dst_pts) {
                while (retry_convert_image <= MAX_RETRY_CONVERT_IMAGE) {
                    ret = convert_image(ffp, frame, (int64_t)pts, frame->width, frame->height);
                    if (!ret) {
                        convert_frame_count++;
                        break;
                    }
                    retry_convert_image++;
                    av_log(NULL, AV_LOG_ERROR, "convert image error retry_convert_image = %d\n", retry_convert_image);
                }

                retry_convert_image = 0;
                if (ret || ffp->get_img_info->count <= 0) {
                    if (ret) {
                        av_log(NULL, AV_LOG_ERROR, "convert image abort ret = %d\n", ret);
                        ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, ret);
                    } else {
                        av_log(NULL, AV_LOG_INFO, "convert image complete convert_frame_count = %d\n", convert_frame_count);
                    }
                    goto the_end;
                }
            } else {
                dst_pts = last_dst_pts;
            }
            av_frame_unref(frame);
            continue;
        }

#if CONFIG_AVFILTER
        if (   last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || ffp->vf_changed
            || last_vfilter_idx != is->vfilter_idx) {
            SDL_LockMutex(ffp->vf_mutex);
            ffp->vf_changed = 0;
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if ((ret = configure_video_filters(ffp, graph, is, ffp->vfilters_list ? ffp->vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                // FIXME: post error
                SDL_UnlockMutex(ffp->vf_mutex);
                goto the_end;
            }
            filt_in  = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            if(delay_forbidden == 0){
              frame_rate = av_buffersink_get_frame_rate(filt_out);
            }
            SDL_UnlockMutex(ffp->vf_mutex);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
        if (ret < 0) {
            if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
            break;
        }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            if(delay_forbidden == 0){//关闭0延迟 默认关闭.
              duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            }else{
              //直接这里写出
			        duration=0.01;
            }
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(ffp, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
        }
#endif

        if (ret < 0)
            goto the_end;
    }
 the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_log(NULL, AV_LOG_INFO, "convert image convert_frame_count = %d\n", convert_frame_count);
    av_frame_free(&frame);
    return 0;
}

static int video_thread(void *arg)
{
    FFPlayer *ffp = (FFPlayer *)arg;
    int       ret = 0;

    if (ffp->node_vdec) {
        ret = ffpipenode_run_sync(ffp->node_vdec);
    }
    return ret;
}

static int subtitle_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(ffp, &is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;
#ifdef FFP_MERGE
        if (got_subtitle && sp->sub.format == 0) {
#else
        if (got_subtitle) {
#endif
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
#ifdef FFP_MERGE
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
#endif
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(NULL, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                        diff, avg_diff, wanted_nb_samples - nb_samples,
                        is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;
#if defined(__ANDROID__)
    int translate_time = 1;
#endif

    if (is->paused || is->step)
        return -1;

    if (ffp->sync_av_start &&                       /* sync enabled */
        is->video_st &&                             /* has video stream */
        !is->viddec.first_frame_decoded &&          /* not hot */
        is->viddec.finished != is->videoq.serial) { /* not finished */
        /* waiting for first video frame */
        Uint64 now = SDL_GetTickHR();
        if (now < is->viddec.first_frame_decoded_time ||
            now > is->viddec.first_frame_decoded_time + 2000) {
            is->viddec.first_frame_decoded = 1;
        } else {
            /* video pipeline is not ready yet */
            return -1;
        }
    }
reload:
    do {
#if defined(_WIN32) || defined(__APPLE__)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - ffp->audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(NULL, af->frame->channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);

    dec_channel_layout =
        (af->frame->channel_layout && af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
        af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format        != is->audio_src.fmt            ||
        dec_channel_layout       != is->audio_src.channel_layout ||
        af->frame->sample_rate   != is->audio_src.freq           ||
        (wanted_nb_samples       != af->frame->nb_samples && !is->swr_ctx)) {
        AVDictionary *swr_opts = NULL;
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout,           af->frame->format, af->frame->sample_rate,
                                         0, NULL);
        if (!is->swr_ctx) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            return -1;
        }
        av_dict_copy(&swr_opts, ffp->swr_opts, 0);
        if (af->frame->channel_layout == AV_CH_LAYOUT_5POINT1_BACK)
            av_opt_set_double(is->swr_ctx, "center_mix_level", ffp->preset_5_1_center_mix_level, 0);
        av_opt_set_dict(is->swr_ctx, &swr_opts);
        av_dict_free(&swr_opts);

        if (swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                    af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                    is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **)af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int)((int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256);
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);

        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        int bytes_per_sample = av_get_bytes_per_sample(is->audio_tgt.fmt);
        resampled_data_size = len2 * is->audio_tgt.channels * bytes_per_sample;
#if defined(__ANDROID__)
        if (ffp->soundtouch_enable && ffp->pf_playback_rate != 1.0f && !is->abort_request) {
            av_fast_malloc(&is->audio_new_buf, &is->audio_new_buf_size, out_size * translate_time);
            for (int i = 0; i < (resampled_data_size / 2); i++)
            {
                is->audio_new_buf[i] = (is->audio_buf1[i * 2] | (is->audio_buf1[i * 2 + 1] << 8));
            }

            int ret_len = ijk_soundtouch_translate(is->handle, is->audio_new_buf, (float)(ffp->pf_playback_rate), (float)(1.0f/ffp->pf_playback_rate),
                    resampled_data_size / 2, bytes_per_sample, is->audio_tgt.channels, af->frame->sample_rate);
            if (ret_len > 0) {
                is->audio_buf = (uint8_t*)is->audio_new_buf;
                resampled_data_size = ret_len;
            } else {
                translate_time++;
                goto reload;
            }
        }
#endif
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef FFP_SHOW_AUDIO_DELAY
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    if (!is->auddec.first_frame_decoded) {
        ALOGD("avcodec/Audio: first frame decoded\n");
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_DECODED_START);
        is->auddec.first_frame_decoded_time = SDL_GetTickHR();
        is->auddec.first_frame_decoded = 1;
    }
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    int audio_size, len1;
    if (!ffp || !is) {
        memset(stream, 0, len);
        return;
    }

    ffp->audio_callback_time = av_gettime_relative();

    if (ffp->pf_playback_rate_changed) {
        ffp->pf_playback_rate_changed = 0;
#if defined(__ANDROID__)
        if (!ffp->soundtouch_enable) {
            SDL_AoutSetPlaybackRate(ffp->aout, ffp->pf_playback_rate);
        }
#else
        SDL_AoutSetPlaybackRate(ffp->aout, ffp->pf_playback_rate);
#endif
    }
    if (ffp->pf_playback_volume_changed) {
        ffp->pf_playback_volume_changed = 0;
        SDL_AoutSetPlaybackVolume(ffp->aout, ffp->pf_playback_volume);
    }

    while (len > 0) {
        if (is->audio_buf_index >= is->audio_buf_size) {
           audio_size = audio_decode_frame(ffp);
           if (audio_size < 0) {
                /* if error, just output silence */
               is->audio_buf = NULL;
               is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
           } else {
               if (is->show_mode != SHOW_MODE_VIDEO)
                   update_sample_display(is, (int16_t *)is->audio_buf, audio_size);
               is->audio_buf_size = audio_size;
           }
           is->audio_buf_index = 0;
        }
        if (is->auddec.pkt_serial != is->audioq.serial) {
            is->audio_buf_index = is->audio_buf_size;
            memset(stream, 0, len);
            // stream += len;
            // len = 0;
            SDL_AoutFlushAudio(ffp->aout);
        break;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudio(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1, is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk, is->audio_clock - (double)(is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec - SDL_AoutGetLatencySeconds(ffp->aout), is->audio_clock_serial, ffp->audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
    if (!ffp->first_audio_frame_rendered) {
        ffp->first_audio_frame_rendered = 1;
        ffp_notify_msg1(ffp, FFP_MSG_AUDIO_RENDERING_START);
    }

    if (is->latest_audio_seek_load_serial == is->audio_clock_serial) {
        int latest_audio_seek_load_serial = __atomic_exchange_n(&(is->latest_audio_seek_load_serial), -1, memory_order_seq_cst);
        if (latest_audio_seek_load_serial == is->audio_clock_serial) {
            if (ffp->av_sync_type == AV_SYNC_AUDIO_MASTER) {
                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 1);
            } else {
                ffp_notify_msg2(ffp, FFP_MSG_AUDIO_SEEK_RENDERING_START, 0);
            }
        }
    }

    if (ffp->render_wait_start && !ffp->start_on_prepared && is->pause_req) {
        while (is->pause_req && !is->abort_request) {
            SDL_Delay(20);
        }
    }
}

static int audio_open(FFPlayer *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params)
{
    FFPlayer *ffp = opaque;
    VideoState *is = ffp->is;
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
#ifdef FFP_MERGE
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
#endif
    static const int next_sample_rates[] = {0, 44100, 48000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AoutGetAudioPerSecondCallBacks(ffp->aout)));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (SDL_AoutOpenAudio(ffp->aout, &wanted_spec, &spec) < 0) {
        /* avoid infinity loop on exit. --by bbcallen */
        if (is->abort_request)
            return -1;
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(NULL, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    SDL_AoutSetDefaultLatencySeconds(ffp->aout, ((double)(2 * spec.size)) / audio_hw_params->bytes_per_sec);
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(FFPlayer *ffp, int stream_index)
{
    VideoState *is = ffp->is;
    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec = NULL;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = ffp->lowres;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    av_codec_set_pkt_timebase(avctx, ic->streams[stream_index]->time_base);

    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index; forced_codec_name = ffp->audio_codec_name; break;
        case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = ffp->subtitle_codec_name; break;
        case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index; forced_codec_name = ffp->video_codec_name; break;
        default: break;
    }
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with id %d\n", avctx->codec_id);
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > av_codec_get_max_lowres(codec)){
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                av_codec_get_max_lowres(codec));
        stream_lowres = av_codec_get_max_lowres(codec);
    }
    av_codec_set_lowres(avctx, stream_lowres);

#if FF_API_EMU_EDGE
    if(stream_lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif
    if (ffp->fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;
#if FF_API_EMU_EDGE
    if(codec->capabilities & AV_CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
#endif

    opts = filter_codec_opts(ffp->codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq           = avctx->sample_rate;
            is->audio_filter_src.channels       = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt            = avctx->sample_fmt;
            SDL_LockMutex(ffp->af_mutex);
            if ((ret = configure_audio_filters(ffp, ffp->afilters, 0)) < 0) {
                SDL_UnlockMutex(ffp->af_mutex);
                goto fail;
            }
            ffp->af_changed = 0;
            SDL_UnlockMutex(ffp->af_mutex);
            sink = is->out_audio_filter;
            sample_rate    = av_buffersink_get_sample_rate(sink);
            nb_channels    = av_buffersink_get_channels(sink);
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output */
        if ((ret = audio_open(ffp, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;
        ffp_set_audio_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));
        is->audio_hw_buf_size = ret;
        is->audio_src = is->audio_tgt;
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* init averaging filter */
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
        is->audio_diff_avg_count = 0;
        /* since we do not have a precise anough audio FIFO fullness,
           we correct audio sync only if larger than this threshold */
        is->audio_diff_threshold = 2.0 * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec;

        is->audio_stream = stream_index;
        is->audio_st = ic->streams[stream_index];

        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }
        if ((ret = decoder_start(&is->auddec, audio_thread, ffp, "ff_audio_dec")) < 0)
            goto out;
        SDL_AoutPauseAudio(ffp->aout, 0);
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;
        is->video_st = ic->streams[stream_index];

        if (ffp->async_init_decoder) {
            while (!is->initialized_decoder) {
                SDL_Delay(5);
            }
            if (ffp->node_vdec) {
                is->viddec.avctx = avctx;
                ret = ffpipeline_config_video_decoder(ffp->pipeline, ffp);
            }
            if (ret || !ffp->node_vdec) {
                decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
                ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp);
                if (!ffp->node_vdec)
                    goto fail;
            }
        } else {
            decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
            ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp);
            if (!ffp->node_vdec)
                goto fail;
        }
        if ((ret = decoder_start(&is->viddec, video_thread, ffp, "ff_video_dec")) < 0)
            goto out;

        is->queue_attachments_req = 1;

        if (ffp->max_fps >= 0) {
            if(is->video_st->avg_frame_rate.den && is->video_st->avg_frame_rate.num) {
                double fps = av_q2d(is->video_st->avg_frame_rate);
                SDL_ProfilerReset(&is->viddec.decode_profiler, fps + 0.5);
                if (fps > ffp->max_fps && fps < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", fps);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", fps);
                }
            }
            if(is->video_st->r_frame_rate.den && is->video_st->r_frame_rate.num) {
                double tbr = av_q2d(is->video_st->r_frame_rate);
                if (tbr > ffp->max_fps && tbr < 130.0) {
                    is->is_video_high_fps = 1;
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (too high)\n", tbr);
                } else {
                    av_log(ffp, AV_LOG_WARNING, "fps: %lf (normal)\n", tbr);
                }
            }
        }

        if (is->is_video_high_fps) {
            avctx->skip_frame       = FFMAX(avctx->skip_frame, AVDISCARD_NONREF);
            avctx->skip_loop_filter = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
            avctx->skip_idct        = FFMAX(avctx->skip_loop_filter, AVDISCARD_NONREF);
        }

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        if (!ffp->subtitle) break;

        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        ffp_set_subtitle_codec_info(ffp, AVCODEC_MODULE_NAME, avcodec_get_name(avctx->codec_id));

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, ffp, "ff_subtitle_dec")) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue, int min_frames) {
    return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
#ifdef FFP_MERGE
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
#endif
           queue->nb_packets > min_frames;
}

static int is_realtime(AVFormatContext *s)
{
    if(   !strcmp(s->iformat->name, "rtp")
       || !strcmp(s->iformat->name, "rtsp")
       || !strcmp(s->iformat->name, "sdp")
    )
        return 1;

    if(s->pb && (   !strncmp(s->filename, "rtp:", 4)
                 || !strncmp(s->filename, "udp:", 4)
                )
    )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    AVFormatContext *ic = NULL;
    int err, i, ret __unused;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int completed = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;
    int last_error = 0;
    int64_t prev_io_tick_counter = 0;
    int64_t io_tick_counter = 0;
    int init_ijkmeta = 0;
    int64_t recordTs;//读取文件标签.

    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
        av_dict_set(&ffp->format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    if (av_stristart(is->filename, "rtmp", NULL) ||
        av_stristart(is->filename, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }

    if (ffp->skip_calc_frame_rate) {
        av_dict_set_int(&ic->metadata, "skip-calc-frame-rate", ffp->skip_calc_frame_rate, 0);
        av_dict_set_int(&ffp->format_opts, "skip-calc-frame-rate", ffp->skip_calc_frame_rate, 0);
    }

    if (ffp->iformat_name)
        is->iformat = av_find_input_format(ffp->iformat_name);
    err = avformat_open_input(&ic, is->filename, is->iformat, &ffp->format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    ffp_notify_msg1(ffp, FFP_MSG_OPEN_INPUT);

    if (scan_all_pmts_set)
        av_dict_set(&ffp->format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(ffp->format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
#ifdef FFP_MERGE
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
#endif
    }
    is->ic = ic;

    if (ffp->genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);
    //
    //AVDictionary **opts;
    //int orig_nb_streams;
    //opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
    //orig_nb_streams = ic->nb_streams;


    if (ffp->find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, ffp->codec_opts);
        int orig_nb_streams = ic->nb_streams;

        do {
            if (av_stristart(is->filename, "data:", NULL) && orig_nb_streams > 0) {
                for (i = 0; i < orig_nb_streams; i++) {
                    if (!ic->streams[i] || !ic->streams[i]->codecpar || ic->streams[i]->codecpar->profile == FF_PROFILE_UNKNOWN) {
                        break;
                    }
                }

                if (i == orig_nb_streams) {
                    break;
                }
            }
            err = avformat_find_stream_info(ic, opts);
        } while(0);
        ffp_notify_msg1(ffp, FFP_MSG_FIND_STREAM_INFO);

        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }
    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    if (ffp->seek_by_bytes < 0)
        ffp->seek_by_bytes = !!(ic->iformat->flags & AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    is->max_frame_duration = 10.0;
    av_log(ffp, AV_LOG_INFO, "max_frame_duration: %.3f\n", is->max_frame_duration);

#ifdef FFP_MERGE
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

#endif
    /* if seeking requested, we execute it */
    if (ffp->start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = ffp->start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                    is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);//= 1;//

    av_dump_format(ic, 0, is->filename, 0);

    int video_stream_count = 0;
    int h264_stream_count = 0;
    int first_h264_stream = -1;
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if (type >= 0 && ffp->wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, ffp->wanted_stream_spec[type]) > 0)
                st_index[type] = i;

        // choose first h264

        if (type == AVMEDIA_TYPE_VIDEO) {
            enum AVCodecID codec_id = st->codecpar->codec_id;
            video_stream_count++;
            if (codec_id == AV_CODEC_ID_H264) {
                h264_stream_count++;
                if (first_h264_stream < 0)
                    first_h264_stream = i;
            }
        }
    }
    if (video_stream_count > 1 && st_index[AVMEDIA_TYPE_VIDEO] < 0) {
        st_index[AVMEDIA_TYPE_VIDEO] = first_h264_stream;
        av_log(NULL, AV_LOG_WARNING, "multiple video stream found, prefer first h264 stream: %d\n", first_h264_stream);
    }
    if (!ffp->video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!ffp->audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (!ffp->video_disable && !ffp->subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                 st_index[AVMEDIA_TYPE_AUDIO] :
                                 st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

    is->show_mode = ffp->show_mode;
#ifdef FFP_MERGE // bbc: dunno if we need this
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
    }
#endif

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_AUDIO]);
    } else {
        ffp->av_sync_type = AV_SYNC_VIDEO_MASTER;
        is->av_sync_type  = ffp->av_sync_type;
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        ret = stream_component_open(ffp, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
        stream_component_open(ffp, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }
    ffp_notify_msg1(ffp, FFP_MSG_COMPONENT_OPEN);

    if (!ffp->ijkmeta_delay_init) {
        ijkmeta_set_avformat_context_l(ffp->meta, ic);
    }

    ffp->stat.bit_rate = ic->bit_rate;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_VIDEO_STREAM, st_index[AVMEDIA_TYPE_VIDEO]);
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_AUDIO_STREAM, st_index[AVMEDIA_TYPE_AUDIO]);
    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0)
        ijkmeta_set_int64_l(ffp->meta, IJKM_KEY_TIMEDTEXT_STREAM, st_index[AVMEDIA_TYPE_SUBTITLE]);

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }
    if (is->audio_stream >= 0) {
        is->audioq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->audioq;
    } else if (is->video_stream >= 0) {
        is->videoq.is_buffer_indicator = 1;
        is->buffer_indicator_queue = &is->videoq;
    } else {
        assert("invalid streams");
    }

    if (ffp->infinite_buffer < 0 && is->realtime)
        ffp->infinite_buffer = 1;

    if (!ffp->render_wait_start && !ffp->start_on_prepared)
        toggle_pause(ffp, 1);
    if (is->video_st && is->video_st->codecpar) {
        AVCodecParameters *codecpar = is->video_st->codecpar;
        ffp_notify_msg3(ffp, FFP_MSG_VIDEO_SIZE_CHANGED, codecpar->width, codecpar->height);
        ffp_notify_msg3(ffp, FFP_MSG_SAR_CHANGED, codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den);
    }
    ffp->prepared = true;
    ffp_notify_msg1(ffp, FFP_MSG_PREPARED);
    if (!ffp->render_wait_start && !ffp->start_on_prepared) {
        while (is->pause_req && !is->abort_request) {
            SDL_Delay(20);
        }
    }
    if (ffp->auto_resume) {
        ffp_notify_msg1(ffp, FFP_REQ_START);
        ffp->auto_resume = 0;
    }
    /* offset should be seeked*/
    if (ffp->seek_at_start > 0) {
        ffp_seek_to_l(ffp, (long)(ffp->seek_at_start));
    }

    for (;;) {
        if (is->abort_request)
            break;
#ifdef FFP_MERGE
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }
#endif
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(ffp->input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        if (is->seek_req) {
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

            ffp_toggle_buffering(ffp, 1);
            ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, 0, 0);
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->filename);
            } else {
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                    // TODO: clear invaild audio data
                    // SDL_AoutFlushAudio(ffp->aout);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {
                    if (ffp->node_vdec) {
                        ffpipenode_flush(ffp->node_vdec);
                    }
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                   set_clock(&is->extclk, NAN, 0);
                } else {
                   set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }

                is->latest_video_seek_load_serial = is->videoq.serial;
                is->latest_audio_seek_load_serial = is->audioq.serial;
                is->latest_seek_load_start_at = av_gettime();
            }
            ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
#ifdef FFP_MERGE
            if (is->paused)
                step_to_next_frame(is);
#endif
            completed = 0;
            SDL_LockMutex(ffp->is->play_mutex);
            if (ffp->auto_resume) {
                is->pause_req = 0;
                if (ffp->packet_buffering)
                    is->buffering_on = 1;
                ffp->auto_resume = 0;
                stream_update_pause_l(ffp);
            }
            if (is->pause_req)
                step_to_next_frame_l(ffp);
            SDL_UnlockMutex(ffp->is->play_mutex);

            if (ffp->enable_accurate_seek) {
                is->drop_aframe_count = 0;
                is->drop_vframe_count = 0;
                SDL_LockMutex(is->accurate_seek_mutex);
                if (is->video_stream >= 0) {
                    is->video_accurate_seek_req = 1;
                }
                if (is->audio_stream >= 0) {
                    is->audio_accurate_seek_req = 1;
                }
                SDL_CondSignal(is->audio_accurate_seek_cond);
                SDL_CondSignal(is->video_accurate_seek_cond);
                SDL_UnlockMutex(is->accurate_seek_mutex);
            }

            ffp_notify_msg3(ffp, FFP_MSG_SEEK_COMPLETE, (int)fftime_to_milliseconds(seek_target), ret);
            ffp_toggle_buffering(ffp, 1);
        }
        if (is->queue_attachments_req) {
            if (is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        /* if the queue are full, no need to read more */
        if (ffp->infinite_buffer<1 && !is->seek_req &&
#ifdef FFP_MERGE
              (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
#else
              (is->audioq.size + is->videoq.size + is->subtitleq.size > ffp->dcc.max_buffer_size
#endif
            || (   stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq, MIN_FRAMES)
                && stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq, MIN_FRAMES)
                && stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq, MIN_FRAMES)))) {
            if (!is->eof) {
                ffp_toggle_buffering(ffp, 0);
            }
            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            //SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;
        }
        if ((!is->paused || completed) &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
            if (ffp->loop != 1 && (!ffp->loop || --ffp->loop)) {
                stream_seek(is, ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0, 0, 0);
            } else if (ffp->autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            } else {
                ffp_statistic_l(ffp);
                if (completed) {
                    av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: eof\n");
                    SDL_LockMutex(wait_mutex);
                    // infinite wait may block shutdown
                    while(!is->abort_request && !is->seek_req)
                        SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 100);
                    SDL_UnlockMutex(wait_mutex);
                    if (!is->abort_request)
                        continue;
                } else {
                    completed = 1;
                    ffp->auto_resume = 0;

                    // TODO: 0 it's a bit early to notify complete here
                    ffp_toggle_buffering(ffp, 0);
                    toggle_pause(ffp, 1);
                    if (ffp->error) {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: error: %d\n", ffp->error);
                        ffp_notify_msg1(ffp, FFP_MSG_ERROR);
                    } else {
                        av_log(ffp, AV_LOG_INFO, "ffp_toggle_buffering: completed: OK\n");
                        ffp_notify_msg1(ffp, FFP_MSG_COMPLETED);
                    }
                }
            }
        }
        pkt->flags = 0;
        ret = av_read_frame(ic, pkt);
        if (ret < 0) {
            int pb_eof = 0;
            int pb_error = 0;
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                ffp_check_buffering_l(ffp);
                pb_eof = 1;
                // check error later
            }
            if (ic->pb && ic->pb->error) {
                pb_eof = 1;
                pb_error = ic->pb->error;
            }
            if (ret == AVERROR_EXIT) {
                pb_eof = 1;
                pb_error = AVERROR_EXIT;
            }

            if (pb_eof) {
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
    if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }
            if (pb_error) {
    if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
    if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
                ffp->error = pb_error;
                av_log(ffp, AV_LOG_ERROR, "av_read_frame error: %s\n", ffp_get_error_string(ffp->error));
                // break;
            } else {
                ffp->error = 0;
            }
            if (is->eof) {
                ffp_toggle_buffering(ffp, 0);
                SDL_Delay(100);
            }
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            ffp_statistic_l(ffp);
            continue;
        } else {
            is->eof = 0;
        }

        if (pkt->flags & AV_PKT_FLAG_DISCONTINUITY) {
            if (is->audio_stream >= 0) {
                packet_queue_put(&is->audioq, &flush_pkt);
            }
            if (is->subtitle_stream >= 0) {
                packet_queue_put(&is->subtitleq, &flush_pkt);
            }
            if (is->video_stream >= 0) {
                packet_queue_put(&is->videoq, &flush_pkt);
            }
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = ffp->duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(ffp->start_time != AV_NOPTS_VALUE ? ffp->start_time : 0) / 1000000
                <= ((double)ffp->duration / 1000000);
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st && (is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC))) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }

        ffp_statistic_l(ffp);

        if (ffp->ijkmeta_delay_init && !init_ijkmeta &&
                (ffp->first_video_frame_rendered || !is->video_st) && (ffp->first_audio_frame_rendered || !is->audio_st)) {
            ijkmeta_set_avformat_context_l(ffp->meta, ic);
            init_ijkmeta = 1;
        }

        if (ffp->packet_buffering) {
            io_tick_counter = SDL_GetTickHR();
            if ((!ffp->first_video_frame_rendered && is->video_st) || (!ffp->first_audio_frame_rendered && is->audio_st)) {
                if (abs((int)(io_tick_counter - prev_io_tick_counter)) > FAST_BUFFERING_CHECK_PER_MILLISECONDS) {
                    prev_io_tick_counter = io_tick_counter;
                    ffp->dcc.current_high_water_mark_in_ms = ffp->dcc.first_high_water_mark_in_ms;
                    ffp_check_buffering_l(ffp);
                }
            } else {
                if (abs((int)(io_tick_counter - prev_io_tick_counter)) > BUFFERING_CHECK_PER_MILLISECONDS) {
                    prev_io_tick_counter = io_tick_counter;
                    ffp_check_buffering_l(ffp);
                }
            }
        }
        //av_log(ffp, AV_LOG_INFO, "common pkt->pts:%ld",pkt->pts);
        //av_log(ffp, AV_LOG_INFO, "common pkt->dts:%ld",pkt->dts);

		// if (!ffp->is_first && pkt->pts == pkt->dts) { // 获取开始录制前dts等于pts最后的值，用于
            //ffp->start_pts = pkt->pts;
            //ffp->start_dts = pkt->dts;
        // }
        if (ffp->is_record) { // 可以录制时，写入文件
            recordTs++;
            if (0 != ffp_record_file(ffp, pkt, recordTs)) {
                ffp->record_error = 1;
                ffp_stop_record(ffp);
            }
        }
    }

    ret = 0;
 fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (!ffp->prepared || !is->abort_request) {
        ffp->last_error = last_error;
        ffp_notify_msg2(ffp, FFP_MSG_ERROR, last_error);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

static int video_refresh_thread(void *arg);
static VideoState *stream_open(FFPlayer *ffp, const char *filename, AVInputFormat *iformat)
{
    assert(!ffp->is);
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;
    is->filename = av_strdup(filename);
    if (!is->filename)
        goto fail;
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;
#if defined(__ANDROID__)
    if (ffp->soundtouch_enable) {
        is->handle = ijk_soundtouch_create();
    }
#endif

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, ffp->pictq_size, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    if (!(is->video_accurate_seek_cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        ffp->enable_accurate_seek = 0;
    }

    if (!(is->audio_accurate_seek_cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        ffp->enable_accurate_seek = 0;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;
    if (ffp->startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", ffp->startup_volume);
    if (ffp->startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", ffp->startup_volume);
    ffp->startup_volume = av_clip(ffp->startup_volume, 0, 100);
    ffp->startup_volume = av_clip(SDL_MIX_MAXVOLUME * ffp->startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = ffp->startup_volume;
    is->muted = 0;
    is->av_sync_type = ffp->av_sync_type;

    is->play_mutex = SDL_CreateMutex();
    is->accurate_seek_mutex = SDL_CreateMutex();
    ffp->is = is;
    is->pause_req = !ffp->start_on_prepared;

    is->video_refresh_tid = SDL_CreateThreadEx(&is->_video_refresh_tid, video_refresh_thread, ffp, "ff_vout");
    if (!is->video_refresh_tid) {
        av_freep(&ffp->is);
        return NULL;
    }

    is->initialized_decoder = 0;
    is->read_tid = SDL_CreateThreadEx(&is->_read_tid, read_thread, ffp, "ff_read");
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        goto fail;
    }

    if (ffp->async_init_decoder && !ffp->video_disable && ffp->video_mime_type && strlen(ffp->video_mime_type) > 0
                    && ffp->mediacodec_default_name && strlen(ffp->mediacodec_default_name) > 0) {
        if (ffp->mediacodec_all_videos || ffp->mediacodec_avc || ffp->mediacodec_hevc || ffp->mediacodec_mpeg2) {
            decoder_init(&is->viddec, NULL, &is->videoq, is->continue_read_thread);
            ffp->node_vdec = ffpipeline_init_video_decoder(ffp->pipeline, ffp);
        }
    }
    is->initialized_decoder = 1;

    return is;
fail:
    is->initialized_decoder = 1;
    is->abort_request = true;
    if (is->video_refresh_tid)
        SDL_WaitThread(is->video_refresh_tid, NULL);
        stream_close(ffp);
    return NULL;
}

// FFP_MERGE: stream_cycle_channel
// FFP_MERGE: toggle_full_screen
// FFP_MERGE: toggle_audio_display
// FFP_MERGE: refresh_loop_wait_event
// FFP_MERGE: event_loop
// FFP_MERGE: opt_frame_size
// FFP_MERGE: opt_width
// FFP_MERGE: opt_height
// FFP_MERGE: opt_format
// FFP_MERGE: opt_frame_pix_fmt
// FFP_MERGE: opt_sync
// FFP_MERGE: opt_seek
// FFP_MERGE: opt_duration
// FFP_MERGE: opt_show_mode
// FFP_MERGE: opt_input_file
// FFP_MERGE: opt_codec
// FFP_MERGE: dummy
// FFP_MERGE: options
// FFP_MERGE: show_usage
// FFP_MERGE: show_help_default
static int video_refresh_thread(void *arg)
{
    FFPlayer *ffp = arg;
    VideoState *is = ffp->is;
    double remaining_time = 0.0;
    while (!is->abort_request) {
        if (remaining_time > 0.0)
            av_usleep((int)(int64_t)(remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
            video_refresh(ffp, &remaining_time);
    }

    return 0;
}

static int lockmgr(void **mtx, enum AVLockOp op)
{
    switch (op) {
    case AV_LOCK_CREATE:
        *mtx = SDL_CreateMutex();
        if (!*mtx) {
            av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
            return 1;
        }
        return 0;
    case AV_LOCK_OBTAIN:
        return !!SDL_LockMutex(*mtx);
    case AV_LOCK_RELEASE:
        return !!SDL_UnlockMutex(*mtx);
    case AV_LOCK_DESTROY:
        SDL_DestroyMutex(*mtx);
        return 0;
    }
    return 1;
}

// FFP_MERGE: main

/*****************************************************************************
 * end last line in ffplay.c
 ****************************************************************************/

static bool g_ffmpeg_global_inited = false;

inline static int log_level_av_to_ijk(int av_level)
{
    int ijk_level = IJK_LOG_VERBOSE;
    if      (av_level <= AV_LOG_PANIC)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_FATAL)      ijk_level = IJK_LOG_FATAL;
    else if (av_level <= AV_LOG_ERROR)      ijk_level = IJK_LOG_ERROR;
    else if (av_level <= AV_LOG_WARNING)    ijk_level = IJK_LOG_WARN;
    else if (av_level <= AV_LOG_INFO)       ijk_level = IJK_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (av_level <= AV_LOG_VERBOSE)    ijk_level = IJK_LOG_INFO;
    else if (av_level <= AV_LOG_DEBUG)      ijk_level = IJK_LOG_DEBUG;
    else if (av_level <= AV_LOG_TRACE)      ijk_level = IJK_LOG_VERBOSE;
    else                                    ijk_level = IJK_LOG_VERBOSE;
    return ijk_level;
}

inline static int log_level_ijk_to_av(int ijk_level)
{
    int av_level = IJK_LOG_VERBOSE;
    if      (ijk_level >= IJK_LOG_SILENT)   av_level = AV_LOG_QUIET;
    else if (ijk_level >= IJK_LOG_FATAL)    av_level = AV_LOG_FATAL;
    else if (ijk_level >= IJK_LOG_ERROR)    av_level = AV_LOG_ERROR;
    else if (ijk_level >= IJK_LOG_WARN)     av_level = AV_LOG_WARNING;
    else if (ijk_level >= IJK_LOG_INFO)     av_level = AV_LOG_INFO;
    // AV_LOG_VERBOSE means detailed info
    else if (ijk_level >= IJK_LOG_DEBUG)    av_level = AV_LOG_DEBUG;
    else if (ijk_level >= IJK_LOG_VERBOSE)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_DEFAULT)  av_level = AV_LOG_TRACE;
    else if (ijk_level >= IJK_LOG_UNKNOWN)  av_level = AV_LOG_TRACE;
    else                                    av_level = AV_LOG_TRACE;
    return av_level;
}

static void ffp_log_callback_brief(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);
    VLOG(ffplv, IJK_LOG_TAG, fmt, vl);
}

static void ffp_log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level())
        return;

    int ffplv __unused = log_level_av_to_ijk(level);

    va_list vl2;
    char line[1024];
    static int print_prefix = 1;

    va_copy(vl2, vl);
    // av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);
    va_end(vl2);

    ALOG(ffplv, IJK_LOG_TAG, "%s", line);
}

int ijkav_register_all(void);
void ffp_global_init()
{
    if (g_ffmpeg_global_inited)
        return;

    ALOGD("ijkmediaplayer version : %s", ijkmp_version());
    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();

    ijkav_register_all();

    avformat_network_init();

    av_lockmgr_register(lockmgr);
    av_log_set_callback(ffp_log_callback_brief);

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;

    g_ffmpeg_global_inited = true;
}

void ffp_global_uninit()
{
    if (!g_ffmpeg_global_inited)
        return;

    av_lockmgr_register(NULL);

    // FFP_MERGE: uninit_opts

    avformat_network_deinit();

    g_ffmpeg_global_inited = false;
}

void ffp_global_set_log_report(int use_report)
{
    if (use_report) {
        av_log_set_callback(ffp_log_callback_report);
    } else {
        av_log_set_callback(ffp_log_callback_brief);
    }
}

void ffp_global_set_log_level(int log_level)
{
    int av_level = log_level_ijk_to_av(log_level);
    av_log_set_level(av_level);
}

static ijk_inject_callback s_inject_callback;
int inject_callback(void *opaque, int type, void *data, size_t data_size)
{
    if (s_inject_callback)
        return s_inject_callback(opaque, type, data, data_size);
    return 0;
}

void ffp_global_set_inject_callback(ijk_inject_callback cb)
{
    s_inject_callback = cb;
}

void ffp_io_stat_register(void (*cb)(const char *url, int type, int bytes))
{
    // avijk_io_stat_register(cb);
}

void ffp_io_stat_complete_register(void (*cb)(const char *url,
                                              int64_t read_bytes, int64_t total_size,
                                              int64_t elpased_time, int64_t total_duration))
{
    // avijk_io_stat_complete_register(cb);
}

static const char *ffp_context_to_name(void *ptr)
{
    return "FFPlayer";
}


static void *ffp_context_child_next(void *obj, void *prev)
{
    return NULL;
}

static const AVClass *ffp_context_child_class_next(const AVClass *prev)
{
    return NULL;
}

const AVClass ffp_context_class = {
    .class_name       = "FFPlayer",
    .item_name        = ffp_context_to_name,
    .option           = ffp_context_options,
    .version          = LIBAVUTIL_VERSION_INT,
    .child_next       = ffp_context_child_next,
    .child_class_next = ffp_context_child_class_next,
};

static const char *ijk_version_info()
{
    return IJKPLAYER_VERSION;
}

FFPlayer *ffp_create()
{
    av_log(NULL, AV_LOG_INFO, "av_version_info: %s\n", av_version_info());
    av_log(NULL, AV_LOG_INFO, "ijk_version_info: %s\n", ijk_version_info());

    FFPlayer* ffp = (FFPlayer*) av_mallocz(sizeof(FFPlayer));
    if (!ffp)
        return NULL;

    msg_queue_init(&ffp->msg_queue);
    ffp->af_mutex = SDL_CreateMutex();
    ffp->vf_mutex = SDL_CreateMutex();

    ffp_reset_internal(ffp);
    ffp->av_class = &ffp_context_class;
    ffp->meta = ijkmeta_create();

    av_opt_set_defaults(ffp);

    return ffp;
}

void ffp_destroy(FFPlayer *ffp)
{
    if (!ffp)
        return;

    if (ffp->is) {
        av_log(NULL, AV_LOG_WARNING, "ffp_destroy_ffplayer: force stream_close()");
        stream_close(ffp);
        ffp->is = NULL;
    }

    SDL_VoutFreeP(&ffp->vout);
    SDL_AoutFreeP(&ffp->aout);
    ffpipenode_free_p(&ffp->node_vdec);
    ffpipeline_free_p(&ffp->pipeline);
    ijkmeta_destroy_p(&ffp->meta);
    ffp_reset_internal(ffp);

    SDL_DestroyMutexP(&ffp->af_mutex);
    SDL_DestroyMutexP(&ffp->vf_mutex);

    msg_queue_destroy(&ffp->msg_queue);


    av_free(ffp);
}

void ffp_destroy_p(FFPlayer **pffp)
{
    if (!pffp)
        return;

    ffp_destroy(*pffp);
    *pffp = NULL;
}

static AVDictionary **ffp_get_opt_dict(FFPlayer *ffp, int opt_category)
{
    assert(ffp);

    switch (opt_category) {
        case FFP_OPT_CATEGORY_FORMAT:   return &ffp->format_opts;
        case FFP_OPT_CATEGORY_CODEC:    return &ffp->codec_opts;
        case FFP_OPT_CATEGORY_SWS:      return &ffp->sws_dict;
        case FFP_OPT_CATEGORY_PLAYER:   return &ffp->player_opts;
        case FFP_OPT_CATEGORY_SWR:      return &ffp->swr_opts;
        default:
            av_log(ffp, AV_LOG_ERROR, "unknown option category %d\n", opt_category);
            return NULL;
    }
}

static int app_func_event(AVApplicationContext *h, int message ,void *data, size_t size)
{
    if (!h || !h->opaque || !data)
        return 0;

    FFPlayer *ffp = (FFPlayer *)h->opaque;
    if (!ffp->inject_opaque)
        return 0;
    if (message == AVAPP_EVENT_IO_TRAFFIC && sizeof(AVAppIOTraffic) == size) {
        AVAppIOTraffic *event = (AVAppIOTraffic *)(intptr_t)data;
        if (event->bytes > 0) {
            ffp->stat.byte_count += event->bytes;
            SDL_SpeedSampler2Add(&ffp->stat.tcp_read_sampler, event->bytes);
        }
    } else if (message == AVAPP_EVENT_ASYNC_STATISTIC && sizeof(AVAppAsyncStatistic) == size) {
        AVAppAsyncStatistic *statistic =  (AVAppAsyncStatistic *) (intptr_t)data;
        ffp->stat.buf_backwards = statistic->buf_backwards;
        ffp->stat.buf_forwards = statistic->buf_forwards;
        ffp->stat.buf_capacity = statistic->buf_capacity;
    }
    return inject_callback(ffp->inject_opaque, message , data, size);
}

static int ijkio_app_func_event(IjkIOApplicationContext *h, int message ,void *data, size_t size)
{
    if (!h || !h->opaque || !data)
        return 0;

    FFPlayer *ffp = (FFPlayer *)h->opaque;
    if (!ffp->ijkio_inject_opaque)
        return 0;

    if (message == IJKIOAPP_EVENT_CACHE_STATISTIC && sizeof(IjkIOAppCacheStatistic) == size) {
        IjkIOAppCacheStatistic *statistic =  (IjkIOAppCacheStatistic *) (intptr_t)data;
        ffp->stat.cache_physical_pos      = statistic->cache_physical_pos;
        ffp->stat.cache_file_forwards     = statistic->cache_file_forwards;
        ffp->stat.cache_file_pos          = statistic->cache_file_pos;
        ffp->stat.cache_count_bytes       = statistic->cache_count_bytes;
        ffp->stat.logical_file_size       = statistic->logical_file_size;
    }

    return 0;
}

void ffp_set_frame_at_time(FFPlayer *ffp, const char *path, int64_t start_time, int64_t end_time, int num, int definition) {
    if (!ffp->get_img_info) {
        ffp->get_img_info = av_mallocz(sizeof(GetImgInfo));
        if (!ffp->get_img_info) {
            ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, -1);
            return;
        }
    }

    if (start_time >= 0 && num > 0 && end_time >= 0 && end_time >= start_time) {
        ffp->get_img_info->img_path   = av_strdup(path);
        ffp->get_img_info->start_time = start_time;
        ffp->get_img_info->end_time   = end_time;
        ffp->get_img_info->num        = num;
        ffp->get_img_info->count      = num;
        if (definition== HD_IMAGE) {
            ffp->get_img_info->width  = 640;
            ffp->get_img_info->height = 360;
        } else if (definition == SD_IMAGE) {
            ffp->get_img_info->width  = 320;
            ffp->get_img_info->height = 180;
    } else {
            ffp->get_img_info->width  = 160;
            ffp->get_img_info->height = 90;
        }
    } else {
        ffp->get_img_info->count = 0;
        ffp_notify_msg3(ffp, FFP_MSG_GET_IMG_STATE, 0, -1);
    }
}

void *ffp_set_ijkio_inject_opaque(FFPlayer *ffp, void *opaque)
{
    if (!ffp)
        return NULL;
    void *prev_weak_thiz = ffp->ijkio_inject_opaque;
    ffp->ijkio_inject_opaque = opaque;

    ijkio_manager_destroyp(&ffp->ijkio_manager_ctx);
    ijkio_manager_create(&ffp->ijkio_manager_ctx, ffp);
    ijkio_manager_set_callback(ffp->ijkio_manager_ctx, ijkio_app_func_event);
    ffp_set_option_int(ffp, FFP_OPT_CATEGORY_FORMAT, "ijkiomanager", (int64_t)(intptr_t)ffp->ijkio_manager_ctx);

    return prev_weak_thiz;
}

void *ffp_set_inject_opaque(FFPlayer *ffp, void *opaque)
{
    if (!ffp)
        return NULL;
    void *prev_weak_thiz = ffp->inject_opaque;
    ffp->inject_opaque = opaque;

    av_application_closep(&ffp->app_ctx);
    av_application_open(&ffp->app_ctx, ffp);
    ffp_set_option_int(ffp, FFP_OPT_CATEGORY_FORMAT, "ijkapplication", (int64_t)(intptr_t)ffp->app_ctx);

    ffp->app_ctx->func_on_app_event = app_func_event;
    return prev_weak_thiz;
}

void ffp_set_option(FFPlayer *ffp, int opt_category, const char *name, const char *value)
{
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set(dict, name, value, 0);
}

void ffp_set_option_int(FFPlayer *ffp, int opt_category, const char *name, int64_t value)
{
    if (!ffp)
        return;

    AVDictionary **dict = ffp_get_opt_dict(ffp, opt_category);
    av_dict_set_int(dict, name, value, 0);
}

void ffp_set_overlay_format(FFPlayer *ffp, int chroma_fourcc)
{
    switch (chroma_fourcc) {
        case SDL_FCC__GLES2:
        case SDL_FCC_I420:
        case SDL_FCC_YV12:
        case SDL_FCC_RV16:
        case SDL_FCC_RV24:
        case SDL_FCC_RV32:
            ffp->overlay_format = chroma_fourcc;
            break;
#ifdef __APPLE__
        case SDL_FCC_I444P10LE:
            ffp->overlay_format = chroma_fourcc;
            break;
#endif
        default:
            av_log(ffp, AV_LOG_ERROR, "ffp_set_overlay_format: unknown chroma fourcc: %d\n", chroma_fourcc);
            break;
    }
}

int ffp_get_video_codec_info(FFPlayer *ffp, char **codec_info)
{
    if (!codec_info)
        return -1;

    // FIXME: not thread-safe
    if (ffp->video_codec_info) {
        *codec_info = strdup(ffp->video_codec_info);
   } else {
        *codec_info = NULL;
    }
    return 0;
}

int ffp_get_audio_codec_info(FFPlayer *ffp, char **codec_info)
{
    if (!codec_info)
        return -1;

    // FIXME: not thread-safe
    if (ffp->audio_codec_info) {
        *codec_info = strdup(ffp->audio_codec_info);
    } else {
        *codec_info = NULL;
    }
    return 0;
}

static void ffp_show_dict(FFPlayer *ffp, const char *tag, AVDictionary *dict)
{
    AVDictionaryEntry *t = NULL;

    while ((t = av_dict_get(dict, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_log(ffp, AV_LOG_INFO, "%-*s: %-*s = %s\n", 12, tag, 28, t->key, t->value);
    }
}

#define FFP_VERSION_MODULE_NAME_LENGTH 13
static void ffp_show_version_str(FFPlayer *ffp, const char *module, const char *version)
{
        av_log(ffp, AV_LOG_INFO, "%-*s: %s\n", FFP_VERSION_MODULE_NAME_LENGTH, module, version);
}

static void ffp_show_version_int(FFPlayer *ffp, const char *module, unsigned version)
{
    av_log(ffp, AV_LOG_INFO, "%-*s: %u.%u.%u\n",
           FFP_VERSION_MODULE_NAME_LENGTH, module,
           (unsigned int)IJKVERSION_GET_MAJOR(version),
           (unsigned int)IJKVERSION_GET_MINOR(version),
           (unsigned int)IJKVERSION_GET_MICRO(version));
}

int ffp_prepare_async_l(FFPlayer *ffp, const char *file_name)
{
    assert(ffp);
    assert(!ffp->is);
    assert(file_name);

    if (av_stristart(file_name, "rtmp", NULL) ||
        av_stristart(file_name, "rtsp", NULL)) {
        // There is total different meaning for 'timeout' option in rtmp
        av_log(ffp, AV_LOG_WARNING, "remove 'timeout' option for rtmp.\n");
        av_dict_set(&ffp->format_opts, "timeout", NULL, 0);
    }

    /* there is a length limit in avformat */
    if (strlen(file_name) + 1 > 1024) {
        av_log(ffp, AV_LOG_ERROR, "%s too long url\n", __func__);
        if (avio_find_protocol_name("ijklongurl:")) {
            av_dict_set(&ffp->format_opts, "ijklongurl-url", file_name, 0);
            file_name = "ijklongurl:";
        }
    }

    av_log(NULL, AV_LOG_INFO, "===== versions =====\n");
    ffp_show_version_str(ffp, "ijkplayer",      ijk_version_info());
    ffp_show_version_str(ffp, "FFmpeg",         av_version_info());
    ffp_show_version_int(ffp, "libavutil",      avutil_version());
    ffp_show_version_int(ffp, "libavcodec",     avcodec_version());
    ffp_show_version_int(ffp, "libavformat",    avformat_version());
    ffp_show_version_int(ffp, "libswscale",     swscale_version());
    ffp_show_version_int(ffp, "libswresample",  swresample_version());
    av_log(NULL, AV_LOG_INFO, "===== options =====\n");
    ffp_show_dict(ffp, "player-opts", ffp->player_opts);
    ffp_show_dict(ffp, "format-opts", ffp->format_opts);
    ffp_show_dict(ffp, "codec-opts ", ffp->codec_opts);
    ffp_show_dict(ffp, "sws-opts   ", ffp->sws_dict);
    ffp_show_dict(ffp, "swr-opts   ", ffp->swr_opts);
    av_log(NULL, AV_LOG_INFO, "===================\n");

    av_opt_set_dict(ffp, &ffp->player_opts);
    if (!ffp->aout) {
        ffp->aout = ffpipeline_open_audio_output(ffp->pipeline, ffp);
        if (!ffp->aout)
            return -1;
    }

#if CONFIG_AVFILTER
    if (ffp->vfilter0) {
        GROW_ARRAY(ffp->vfilters_list, ffp->nb_vfilters);
        ffp->vfilters_list[ffp->nb_vfilters - 1] = ffp->vfilter0;
    }
#endif

    VideoState *is = stream_open(ffp, file_name, NULL);
    if (!is) {
        av_log(NULL, AV_LOG_WARNING, "ffp_prepare_async_l: stream_open failed OOM");
        return EIJK_OUT_OF_MEMORY;
    }

    ffp->is = is;
    ffp->input_filename = av_strdup(file_name);
    return 0;
}

int ffp_start_from_l(FFPlayer *ffp, long msec)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    ffp->auto_resume = 1;
    ffp_toggle_buffering(ffp, 1);
    ffp_seek_to_l(ffp, msec);
    return 0;
}

int ffp_start_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    toggle_pause(ffp, 0);
    return 0;
}

int ffp_pause_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return EIJK_NULL_IS_PTR;

    toggle_pause(ffp, 1);
    return 0;
}

int ffp_is_paused_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is)
        return 1;

    return is->paused;
}

int ffp_stop_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (is) {
        is->abort_request = 1;
        toggle_pause(ffp, 1);
    }

    msg_queue_abort(&ffp->msg_queue);
    if (ffp->enable_accurate_seek && is && is->accurate_seek_mutex
        && is->audio_accurate_seek_cond && is->video_accurate_seek_cond) {
        SDL_LockMutex(is->accurate_seek_mutex);
        is->audio_accurate_seek_req = 0;
        is->video_accurate_seek_req = 0;
        SDL_CondSignal(is->audio_accurate_seek_cond);
        SDL_CondSignal(is->video_accurate_seek_cond);
        SDL_UnlockMutex(is->accurate_seek_mutex);
    }
    return 0;
}

int ffp_wait_stop_l(FFPlayer *ffp)
{
    assert(ffp);

    if (ffp->is) {
        ffp_stop_l(ffp);
        stream_close(ffp);
        ffp->is = NULL;
    }
    return 0;
}

int ffp_seek_to_l(FFPlayer *ffp, long msec)
{
    assert(ffp);
    VideoState *is = ffp->is;
    int64_t start_time = 0;
    int64_t seek_pos = milliseconds_to_fftime(msec);
    int64_t duration = milliseconds_to_fftime(ffp_get_duration_l(ffp));

    if (!is)
        return EIJK_NULL_IS_PTR;

    if (duration > 0 && seek_pos >= duration && ffp->enable_accurate_seek) {
        toggle_pause(ffp, 1);
        ffp_notify_msg1(ffp, FFP_MSG_COMPLETED);
        return 0;
    }

    start_time = is->ic->start_time;
    if (start_time > 0 && start_time != AV_NOPTS_VALUE)
        seek_pos += start_time;

    // FIXME: 9 seek by bytes
    // FIXME: 9 seek out of range
    // FIXME: 9 seekable
    av_log(ffp, AV_LOG_DEBUG, "stream_seek %"PRId64"(%d) + %"PRId64", \n", seek_pos, (int)msec, start_time);
    stream_seek(is, seek_pos, 0, 0);
    return 0;
}

long ffp_get_current_position_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is || !is->ic)
        return 0;

    int64_t start_time = is->ic->start_time;
    int64_t start_diff = 0;
    if (start_time > 0 && start_time != AV_NOPTS_VALUE)
        start_diff = fftime_to_milliseconds(start_time);

    int64_t pos = 0;
    double pos_clock = get_master_clock(is);
    if (isnan(pos_clock)) {
        pos = fftime_to_milliseconds(is->seek_pos);
    } else {
        pos = pos_clock * 1000;
    }

    // If using REAL time and not ajusted, then return the real pos as calculated from the stream
    // the use case for this is primarily when using a custom non-seekable data source that starts
    // with a buffer that is NOT the start of the stream.  We want the get_current_position to
    // return the time in the stream, and not the player's internal clock.
    if (ffp->no_time_adjust) {
        return (long)pos;
    }

    if (pos < 0 || pos < start_diff)
        return 0;

    int64_t adjust_pos = pos - start_diff;
    return (long)adjust_pos;
}

long ffp_get_duration_l(FFPlayer *ffp)
{
    assert(ffp);
    VideoState *is = ffp->is;
    if (!is || !is->ic)
        return 0;

    int64_t duration = fftime_to_milliseconds(is->ic->duration);
    if (duration < 0)
        return 0;

    return (long)duration;
}

long ffp_get_playable_duration_l(FFPlayer *ffp)
{
    assert(ffp);
    if (!ffp)
        return 0;

    return (long)ffp->playable_duration_ms;
}

void ffp_set_loop(FFPlayer *ffp, int loop)
{
    assert(ffp);
    if (!ffp)
        return;
    ffp->loop = loop;
}

int ffp_get_loop(FFPlayer *ffp)
{
    assert(ffp);
    if (!ffp)
        return 1;
    return ffp->loop;
}

int ffp_packet_queue_init(PacketQueue *q)
{
    return packet_queue_init(q);
}

void ffp_packet_queue_destroy(PacketQueue *q)
{
    return packet_queue_destroy(q);
}

void ffp_packet_queue_abort(PacketQueue *q)
{
    return packet_queue_abort(q);
}

void ffp_packet_queue_start(PacketQueue *q)
{
    return packet_queue_start(q);
}

void ffp_packet_queue_flush(PacketQueue *q)
{
    return packet_queue_flush(q);
}

int ffp_packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    return packet_queue_get(q, pkt, block, serial);
}

int ffp_packet_queue_get_or_buffering(FFPlayer *ffp, PacketQueue *q, AVPacket *pkt, int *serial, int *finished)
{
    return packet_queue_get_or_buffering(ffp, q, pkt, serial, finished);
}

int ffp_packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    return packet_queue_put(q, pkt);
}

bool ffp_is_flush_packet(AVPacket *pkt)
{
    if (!pkt)
        return false;

    return pkt->data == flush_pkt.data;
}

Frame *ffp_frame_queue_peek_writable(FrameQueue *f)
{
    return frame_queue_peek_writable(f);
}

void ffp_frame_queue_push(FrameQueue *f)
{
    return frame_queue_push(f);
}

int ffp_queue_picture(FFPlayer *ffp, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial)
{
    return queue_picture(ffp, src_frame, pts, duration, pos, serial);
}

int ffp_get_master_sync_type(VideoState *is)
{
    return get_master_sync_type(is);
}

double ffp_get_master_clock(VideoState *is)
{
    return get_master_clock(is);
}

void ffp_toggle_buffering_l(FFPlayer *ffp, int buffering_on)
{
    if (!ffp->packet_buffering)
        return;

    VideoState *is = ffp->is;
    if (buffering_on && !is->buffering_on) {
        av_log(ffp, AV_LOG_DEBUG, "ffp_toggle_buffering_l: start\n");
        is->buffering_on = 1;
        stream_update_pause_l(ffp);
        if (is->seek_req) {
            is->seek_buffering = 1;
            ffp_notify_msg2(ffp, FFP_MSG_BUFFERING_START, 1);
    } else {
            ffp_notify_msg2(ffp, FFP_MSG_BUFFERING_START, 0);
        }
    } else if (!buffering_on && is->buffering_on){
        av_log(ffp, AV_LOG_DEBUG, "ffp_toggle_buffering_l: end\n");
        is->buffering_on = 0;
        stream_update_pause_l(ffp);
        if (is->seek_buffering) {
            is->seek_buffering = 0;
            ffp_notify_msg2(ffp, FFP_MSG_BUFFERING_END, 1);
    } else {
            ffp_notify_msg2(ffp, FFP_MSG_BUFFERING_END, 0);
        }
    }
}

void ffp_toggle_buffering(FFPlayer *ffp, int start_buffering)
{
    SDL_LockMutex(ffp->is->play_mutex);
    ffp_toggle_buffering_l(ffp, start_buffering);
    SDL_UnlockMutex(ffp->is->play_mutex);
}

void ffp_track_statistic_l(FFPlayer *ffp, AVStream *st, PacketQueue *q, FFTrackCacheStatistic *cache)
{
    assert(cache);

    if (q) {
        cache->bytes   = q->size;
        cache->packets = q->nb_packets;
    }

    if (q && st && st->time_base.den > 0 && st->time_base.num > 0) {
        cache->duration = q->duration * av_q2d(st->time_base) * 1000;
    }
}

void ffp_audio_statistic_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    ffp_track_statistic_l(ffp, is->audio_st, &is->audioq, &ffp->stat.audio_cache);
}

void ffp_video_statistic_l(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    ffp_track_statistic_l(ffp, is->video_st, &is->videoq, &ffp->stat.video_cache);
}

void ffp_statistic_l(FFPlayer *ffp)
{
    ffp_audio_statistic_l(ffp);
    ffp_video_statistic_l(ffp);
}

void ffp_check_buffering_l(FFPlayer *ffp)
{
    VideoState *is            = ffp->is;
    int hwm_in_ms             = ffp->dcc.current_high_water_mark_in_ms; // use fast water mark for first loading
    int buf_size_percent      = -1;
    int buf_time_percent      = -1;
    int hwm_in_bytes          = ffp->dcc.high_water_mark_in_bytes;
    int need_start_buffering  = 0;
    int audio_time_base_valid = 0;
    int video_time_base_valid = 0;
    int64_t buf_time_position = -1;

    if(is->audio_st)
        audio_time_base_valid = is->audio_st->time_base.den > 0 && is->audio_st->time_base.num > 0;
    if(is->video_st)
        video_time_base_valid = is->video_st->time_base.den > 0 && is->video_st->time_base.num > 0;

    if (hwm_in_ms > 0) {
        int     cached_duration_in_ms = -1;
        int64_t audio_cached_duration = -1;
        int64_t video_cached_duration = -1;

        if (is->audio_st && audio_time_base_valid) {
            audio_cached_duration = ffp->stat.audio_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int audio_cached_percent = (int)av_rescale(audio_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "audio cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", audio_cached_percent,
                  (int)audio_cached_duration, hwm_in_ms,
                  is->audioq.size, hwm_in_bytes,
                  is->audioq.nb_packets, MIN_FRAMES);
#endif
        }

        if (is->video_st && video_time_base_valid) {
            video_cached_duration = ffp->stat.video_cache.duration;
#ifdef FFP_SHOW_DEMUX_CACHE
            int video_cached_percent = (int)av_rescale(video_cached_duration, 1005, hwm_in_ms * 10);
            av_log(ffp, AV_LOG_DEBUG, "video cache=%%%d milli:(%d/%d) bytes:(%d/%d) packet:(%d/%d)\n", video_cached_percent,
                  (int)video_cached_duration, hwm_in_ms,
                  is->videoq.size, hwm_in_bytes,
                  is->videoq.nb_packets, MIN_FRAMES);
#endif
        }

        if (video_cached_duration > 0 && audio_cached_duration > 0) {
            cached_duration_in_ms = (int)IJKMIN(video_cached_duration, audio_cached_duration);
        } else if (video_cached_duration > 0) {
            cached_duration_in_ms = (int)video_cached_duration;
        } else if (audio_cached_duration > 0) {
            cached_duration_in_ms = (int)audio_cached_duration;
        }

        if (cached_duration_in_ms >= 0) {
            buf_time_position = ffp_get_current_position_l(ffp) + cached_duration_in_ms;
            ffp->playable_duration_ms = buf_time_position;

            buf_time_percent = (int)av_rescale(cached_duration_in_ms, 1005, hwm_in_ms * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
            av_log(ffp, AV_LOG_DEBUG, "time cache=%%%d (%d/%d)\n", buf_time_percent, cached_duration_in_ms, hwm_in_ms);
#endif
#ifdef FFP_NOTIFY_BUF_TIME
            ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_TIME_UPDATE, cached_duration_in_ms, hwm_in_ms);
#endif
        }
    }

    int cached_size = is->audioq.size + is->videoq.size;
    if (hwm_in_bytes > 0) {
        buf_size_percent = (int)av_rescale(cached_size, 1005, hwm_in_bytes * 10);
#ifdef FFP_SHOW_DEMUX_CACHE
        av_log(ffp, AV_LOG_DEBUG, "size cache=%%%d (%d/%d)\n", buf_size_percent, cached_size, hwm_in_bytes);
#endif
#ifdef FFP_NOTIFY_BUF_BYTES
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_BYTES_UPDATE, cached_size, hwm_in_bytes);
#endif
    }

    int buf_percent = -1;
    if (buf_time_percent >= 0) {
        // alwas depend on cache duration if valid
        if (buf_time_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_time_percent;
    } else {
        if (buf_size_percent >= 100)
            need_start_buffering = 1;
        buf_percent = buf_size_percent;
    }

    if (buf_time_percent >= 0 && buf_size_percent >= 0) {
        buf_percent = FFMIN(buf_time_percent, buf_size_percent);
    }
    if (buf_percent) {
#ifdef FFP_SHOW_BUF_POS
        av_log(ffp, AV_LOG_DEBUG, "buf pos=%"PRId64", %%%d\n", buf_time_position, buf_percent);
#endif
        ffp_notify_msg3(ffp, FFP_MSG_BUFFERING_UPDATE, (int)buf_time_position, buf_percent);
    }

    if (need_start_buffering) {
        if (hwm_in_ms < ffp->dcc.next_high_water_mark_in_ms) {
            hwm_in_ms = ffp->dcc.next_high_water_mark_in_ms;
        } else {
            hwm_in_ms *= 2;
        }

        if (hwm_in_ms > ffp->dcc.last_high_water_mark_in_ms)
            hwm_in_ms = ffp->dcc.last_high_water_mark_in_ms;

        ffp->dcc.current_high_water_mark_in_ms = hwm_in_ms;

        if (is->buffer_indicator_queue && is->buffer_indicator_queue->nb_packets > 0) {
            if (   (is->audioq.nb_packets >= MIN_MIN_FRAMES || is->audio_stream < 0 || is->audioq.abort_request)
                && (is->videoq.nb_packets >= MIN_MIN_FRAMES || is->video_stream < 0 || is->videoq.abort_request)) {
                ffp_toggle_buffering(ffp, 0);
            }
        }
    }
}

int ffp_video_thread(FFPlayer *ffp)
{
    return ffplay_video_thread(ffp);
}

void ffp_set_video_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->video_codec_info);
    ffp->video_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "VideoCodec: %s\n", ffp->video_codec_info);
}

void ffp_set_audio_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->audio_codec_info);
    ffp->audio_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "AudioCodec: %s\n", ffp->audio_codec_info);
}

void ffp_set_subtitle_codec_info(FFPlayer *ffp, const char *module, const char *codec)
{
    av_freep(&ffp->subtitle_codec_info);
    ffp->subtitle_codec_info = av_asprintf("%s, %s", module ? module : "", codec ? codec : "");
    av_log(ffp, AV_LOG_INFO, "SubtitleCodec: %s\n", ffp->subtitle_codec_info);
}

void ffp_set_playback_rate(FFPlayer *ffp, float rate)
{
    if (!ffp)
        return;

    av_log(ffp, AV_LOG_INFO, "Playback rate: %f\n", rate);
    ffp->pf_playback_rate = rate;
    ffp->pf_playback_rate_changed = 1;
    
    //倍速播放放开2.0限制. 
	if (ffp->is->audio_stream < 0) {
        av_log(ffp, AV_LOG_INFO, "didn't select an audio track, setclock speed instead");
        set_clock_speed(&ffp->is->extclk, (double)rate);
    }
}

void ffp_set_playback_volume(FFPlayer *ffp, float volume)
{
    if (!ffp)
        return;
    ffp->pf_playback_volume = volume;
    ffp->pf_playback_volume_changed = 1;
}

int ffp_get_video_rotate_degrees(FFPlayer *ffp)
{
    VideoState *is = ffp->is;
    if (!is)
        return 0;

    int theta  = abs((int)((int64_t)round(fabs(get_rotation(is->video_st))) % 360));
    switch (theta) {
        case 0:
        case 90:
        case 180:
        case 270:
            break;
        case 360:
            theta = 0;
            break;
        default:
            ALOGW("Unknown rotate degress: %d\n", theta);
            theta = 0;
            break;
    }

    return theta;
}

int ffp_set_stream_selected(FFPlayer *ffp, int stream, int selected)
{
    VideoState        *is = ffp->is;
    AVFormatContext   *ic = NULL;
    AVCodecParameters *codecpar = NULL;
    if (!is)
        return -1;
    ic = is->ic;
    if (!ic)
        return -1;

    if (stream < 0 || stream >= ic->nb_streams) {
        av_log(ffp, AV_LOG_ERROR, "invalid stream index %d >= stream number (%d)\n", stream, ic->nb_streams);
        return -1;
    }

    codecpar = ic->streams[stream]->codecpar;

    if (selected) {
        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (stream != is->video_stream && is->video_stream >= 0)
                    stream_component_close(ffp, is->video_stream);
                        break;
            case AVMEDIA_TYPE_AUDIO:
                if (stream != is->audio_stream && is->audio_stream >= 0)
                    stream_component_close(ffp, is->audio_stream);
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                if (stream != is->subtitle_stream && is->subtitle_stream >= 0)
                    stream_component_close(ffp, is->subtitle_stream);
                break;
            default:
                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of video type %d\n", stream, codecpar->codec_type);
                return -1;
        }
        return stream_component_open(ffp, stream);
                    } else {
        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO:
                if (stream == is->video_stream)
                    stream_component_close(ffp, is->video_stream);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (stream == is->audio_stream)
                    stream_component_close(ffp, is->audio_stream);
                break;
            case AVMEDIA_TYPE_SUBTITLE:
                if (stream == is->subtitle_stream)
                    stream_component_close(ffp, is->subtitle_stream);
                break;
            default:
                av_log(ffp, AV_LOG_ERROR, "select invalid stream %d of audio type %d\n", stream, codecpar->codec_type);
                return -1;
        }
        return 0;
    }
}

float ffp_get_property_float(FFPlayer *ffp, int id, float default_value)
{
    switch (id) {
        case FFP_PROP_FLOAT_VIDEO_DECODE_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vdps : default_value;
        case FFP_PROP_FLOAT_VIDEO_OUTPUT_FRAMES_PER_SECOND:
            return ffp ? ffp->stat.vfps : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            return ffp ? ffp->pf_playback_rate : default_value;
        case FFP_PROP_FLOAT_AVDELAY:
            return ffp ? ffp->stat.avdelay : default_value;
        case FFP_PROP_FLOAT_AVDIFF:
            return ffp ? ffp->stat.avdiff : default_value;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            return ffp ? ffp->pf_playback_volume : default_value;
        case FFP_PROP_FLOAT_DROP_FRAME_RATE:
            return ffp ? ffp->stat.drop_frame_rate : default_value;
        default:
            return default_value;
    }
}

void ffp_set_property_float(FFPlayer *ffp, int id, float value)
{
    switch (id) {
        case FFP_PROP_FLOAT_PLAYBACK_RATE:
            ffp_set_playback_rate(ffp, value);
                        break;
        case FFP_PROP_FLOAT_PLAYBACK_VOLUME:
            ffp_set_playback_volume(ffp, value);
            break;
        default:
            return;
    }
}

int64_t ffp_get_property_int64(FFPlayer *ffp, int id, int64_t default_value)
{
    switch (id) {
        case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->video_stream;
        case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->audio_stream;
        case FFP_PROP_INT64_SELECTED_TIMEDTEXT_STREAM:
            if (!ffp || !ffp->is)
                return default_value;
            return ffp->is->subtitle_stream;
        case FFP_PROP_INT64_VIDEO_DECODER:
            if (!ffp)
                return default_value;
            return ffp->stat.vdec_type;
        case FFP_PROP_INT64_AUDIO_DECODER:
            return FFP_PROPV_DECODER_AVCODEC;

        case FFP_PROP_INT64_VIDEO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.duration;
        case FFP_PROP_INT64_AUDIO_CACHED_DURATION:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.duration;
        case FFP_PROP_INT64_VIDEO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.bytes;
        case FFP_PROP_INT64_AUDIO_CACHED_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.bytes;
        case FFP_PROP_INT64_VIDEO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.video_cache.packets;
        case FFP_PROP_INT64_AUDIO_CACHED_PACKETS:
            if (!ffp)
                return default_value;
            return ffp->stat.audio_cache.packets;
        case FFP_PROP_INT64_BIT_RATE:
            return ffp ? ffp->stat.bit_rate : default_value;
        case FFP_PROP_INT64_TCP_SPEED:
            return ffp ? SDL_SpeedSampler2GetSpeed(&ffp->stat.tcp_read_sampler) : default_value;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_BACKWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_backwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_FORWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_forwards;
        case FFP_PROP_INT64_ASYNC_STATISTIC_BUF_CAPACITY:
            if (!ffp)
                return default_value;
            return ffp->stat.buf_capacity;
        case FFP_PROP_INT64_LATEST_SEEK_LOAD_DURATION:
            return ffp ? ffp->stat.latest_seek_load_duration : default_value;
        case FFP_PROP_INT64_TRAFFIC_STATISTIC_BYTE_COUNT:
            return ffp ? ffp->stat.byte_count : default_value;
        case FFP_PROP_INT64_CACHE_STATISTIC_PHYSICAL_POS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_physical_pos;
       case FFP_PROP_INT64_CACHE_STATISTIC_FILE_FORWARDS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_file_forwards;
       case FFP_PROP_INT64_CACHE_STATISTIC_FILE_POS:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_file_pos;
       case FFP_PROP_INT64_CACHE_STATISTIC_COUNT_BYTES:
            if (!ffp)
                return default_value;
            return ffp->stat.cache_count_bytes;
       case FFP_PROP_INT64_LOGICAL_FILE_SIZE:
            if (!ffp)
                return default_value;
            return ffp->stat.logical_file_size;
        default:
            return default_value;
    }
}

void ffp_set_property_int64(FFPlayer *ffp, int id, int64_t value)
{
    switch (id) {
        // case FFP_PROP_INT64_SELECTED_VIDEO_STREAM:
        // case FFP_PROP_INT64_SELECTED_AUDIO_STREAM:
        case FFP_PROP_INT64_SHARE_CACHE_DATA:
            if (ffp) {
                if (value) {
                    ijkio_manager_will_share_cache_map(ffp->ijkio_manager_ctx);
            } else {
                    ijkio_manager_did_share_cache_map(ffp->ijkio_manager_ctx);
                }
            }
            break;
        case FFP_PROP_INT64_IMMEDIATE_RECONNECT:
            if (ffp) {
                ijkio_manager_immediate_reconnect(ffp->ijkio_manager_ctx);
            }
        default:
            break;
    }
}

IjkMediaMeta *ffp_get_meta_l(FFPlayer *ffp)
{
    if (!ffp)
        return NULL;

    return ffp->meta;
}

//start record video . add by poe 2024/07/27.
int ffp_start_record(FFPlayer *ffp, const char *file_name)
{
    assert(ffp);
    
    VideoState *is = ffp->is;
    int ret = 0, i;
    
    ffp->m_ofmt_ctx = NULL;
    ffp->m_ofmt = NULL;
    ffp->is_record = 0;
    ffp->record_error = 0;
    ffp->is_first = 0;
    ffp->direct_video_copy = 1; // Enable direct video copy by default
    ffp->last_video_dts = -1;   // Initialize DTS tracking
    ffp->last_audio_dts = -1;
    ffp->waiting_for_keyframe = 1;
    ffp->record_first_vpts = AV_NOPTS_VALUE;
    packet_queue_init(&ffp->record_audio_queue);
    
    if (!file_name || !strlen(file_name)) { // 没有路径
        av_log(ffp, AV_LOG_ERROR, "filename is invalid");
        goto end;
    }
    
    if (!is || !is->ic|| is->paused || is->abort_request) { // 没有上下文，或者上下文已经停止
        av_log(ffp, AV_LOG_ERROR, "is,is->ic,is->paused is invalid");
        goto end;
    }
    
    if (ffp->is_record) { // 已经在录制
        av_log(ffp, AV_LOG_ERROR, "recording has started");
        goto end;
    }
    
    // 初始化一个用于输出的AVFormatContext结构体
    ret = avformat_alloc_output_context2(&ffp->m_ofmt_ctx, NULL, "mp4", file_name);
    if (!ffp->m_ofmt_ctx) {
        av_log(ffp, AV_LOG_ERROR, "Could not create output context filename is %s\n", file_name);
        goto end;
    }
    ffp->m_ofmt = ffp->m_ofmt_ctx->oformat;
    
    // 初始化时间戳追踪变量
    ffp->last_video_dts = -1;
    ffp->last_audio_dts = -1;
    
    // 初始化记录实际时间的变量
    ffp->record_real_time = 0;
    
    av_log(ffp, AV_LOG_INFO, "Creating output streams for MP4 recording");
    
    // 检查是否有PCM音频流或HEVC视频流
    int has_pcm_audio = 0;
    int has_hevc_video = 0;
    AVCodecContext *hevc_codec_ctx = NULL;
    
    // 检查视频流是否是HEVC
    if (is->viddec.avctx && is->viddec.avctx->codec_id == AV_CODEC_ID_HEVC) {
        has_hevc_video = 1;
        hevc_codec_ctx = is->viddec.avctx;
        av_log(ffp, AV_LOG_INFO, "HEVC video stream detected, ensuring proper MP4 compatibility");
    }
    
    // 检查音频流
    for (i = 0; i < is->ic->nb_streams; i++) {
        AVStream *in_stream = is->ic->streams[i];
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            enum AVCodecID codec_id = in_stream->codecpar->codec_id;
            if (codec_id == AV_CODEC_ID_PCM_MULAW || 
                codec_id == AV_CODEC_ID_PCM_ALAW ||
                codec_id == AV_CODEC_ID_PCM_S16LE ||
                codec_id == AV_CODEC_ID_PCM_S16BE ||
                codec_id == AV_CODEC_ID_PCM_U8) {
                has_pcm_audio = 1;
                av_log(ffp, AV_LOG_WARNING, "PCM audio format detected (codec_id=%d), MP4 container requires transcoding", codec_id);
                break;
            }
        }
    }
    
    if (has_pcm_audio) {
        av_log(ffp, AV_LOG_INFO, "PCM audio detected, using transcoded recording instead of direct recording");
        
        // 确保使用转码模式，这会正确处理PCM音频
        ffp->need_transcode = 1;
        
        ret = ffp_start_record_transcode(ffp, file_name);
        return ret;
    }
    
    for (i = 0; i < is->ic->nb_streams; i++) {
        // 对照输入流创建输出流通道
        AVStream *in_stream = is->ic->streams[i];
        AVStream *out_stream = avformat_new_stream(ffp->m_ofmt_ctx, in_stream->codec->codec);
        if (!out_stream) {
            av_log(ffp, AV_LOG_ERROR, "Failed allocating output stream\n");
            goto end;
        }
        
        // 将输入视频/音频的参数拷贝至输出视频/音频的AVCodecContext结构体
        av_log(ffp, AV_LOG_DEBUG, "Processing stream %d, codec_type=%d, codec_id=%d", 
               i, in_stream->codecpar->codec_type, in_stream->codecpar->codec_id);
               
        if (avcodec_copy_context(out_stream->codec, in_stream->codec) < 0) {
            av_log(ffp, AV_LOG_ERROR, "Failed to copy context from input to output stream codec context\n");
            goto end;
        }
        
        // 确保正确设置codecpar (对于较新的FFmpeg版本)
        ret = avcodec_parameters_from_context(out_stream->codecpar, in_stream->codec);
        if (ret < 0) {
            av_log(ffp, AV_LOG_ERROR, "Failed to copy context parameters to codecpar\n");
            goto end;
        }
        
        // 对于HEVC视频流，确保extradata被正确设置
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
            in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC && hevc_codec_ctx) {
            
            av_log(ffp, AV_LOG_INFO, "Setting up HEVC extradata for MP4 container");
            
            // 确保extradata被正确复制
            if (hevc_codec_ctx->extradata_size > 0) {
                // 释放可能存在的旧extradata
                if (out_stream->codecpar->extradata) {
                    av_freep(&out_stream->codecpar->extradata);
                    out_stream->codecpar->extradata_size = 0;
                }
                
                // 分配新的extradata空间
                out_stream->codecpar->extradata = av_mallocz(hevc_codec_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (out_stream->codecpar->extradata) {
                    memcpy(out_stream->codecpar->extradata, hevc_codec_ctx->extradata, hevc_codec_ctx->extradata_size);
                    out_stream->codecpar->extradata_size = hevc_codec_ctx->extradata_size;
                    av_log(ffp, AV_LOG_INFO, "HEVC extradata copied: size=%d", out_stream->codecpar->extradata_size);
                } else {
                    av_log(ffp, AV_LOG_ERROR, "Failed to allocate memory for HEVC extradata");
                }
                
                // 同样更新codec结构体中的extradata
                if (out_stream->codec) {
                    if (out_stream->codec->extradata) {
                        av_freep(&out_stream->codec->extradata);
                        out_stream->codec->extradata_size = 0;
                    }
                    
                    out_stream->codec->extradata = av_mallocz(hevc_codec_ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
                    if (out_stream->codec->extradata) {
                        memcpy(out_stream->codec->extradata, hevc_codec_ctx->extradata, hevc_codec_ctx->extradata_size);
                        out_stream->codec->extradata_size = hevc_codec_ctx->extradata_size;
                    }
                }
            } else {
                av_log(ffp, AV_LOG_WARNING, "HEVC codec has no extradata, this may cause issues with MP4 container");
            }
            
            // 设置HEVC特定的参数
            if (strcmp(ffp->m_ofmt->name, "mp4") == 0) {
                // 为HEVC在MP4中设置正确的时间基准
                out_stream->time_base = (AVRational){1, 90000}; // 标准HEVC时间基准
                av_log(ffp, AV_LOG_INFO, "Setting HEVC time base to 1/90000 for MP4 container");
                
                // 设置其他HEVC特定参数
                av_dict_set(&ffp->m_ofmt_ctx->metadata, "encoder", "IJKPlayer HEVC", 0);
                
                // 显式设置codec_tag为"hvc1"而不是"hevc"或"hev1"
                uint32_t hvc1_tag = MKTAG('h','v','c','1'); // 创建'hvc1'标签
                out_stream->codecpar->codec_tag = hvc1_tag;
                if (out_stream->codec) {
                    out_stream->codec->codec_tag = hvc1_tag;
                }
                
                // 添加HVC1特定的标志和参数
                av_dict_set(&ffp->m_ofmt_ctx->metadata, "compatible_brands", "iso6mp42hvc1", 0);
                
                av_log(ffp, AV_LOG_INFO, "Explicitly setting HEVC codec tag to 'hvc1' for better compatibility");
            }
        }
        
        // 对于HEVC流，已经在上面设置了codec_tag，这里只处理非HEVC流
        if (!(in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && 
              in_stream->codecpar->codec_id == AV_CODEC_ID_HEVC)) {
            out_stream->codec->codec_tag = 0;
            out_stream->codecpar->codec_tag = 0; // 让容器选择合适的标签
        }
        
        if (ffp->m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
            out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
        }
        
        // MP4容器特殊处理 - 确保使用兼容的编解码器
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && strcmp(ffp->m_ofmt->name, "mp4") == 0) {
            // 如果是不兼容的音频编解码器，应该已经通过上面的转码路径处理
            if (out_stream->codecpar->codec_id != AV_CODEC_ID_AAC && 
                out_stream->codecpar->codec_id != AV_CODEC_ID_MP3 &&
                out_stream->codecpar->codec_id != AV_CODEC_ID_AC3) {
                av_log(ffp, AV_LOG_WARNING, "Audio codec %d may not be compatible with MP4 container", 
                       out_stream->codecpar->codec_id);
            }
        }
    }
    
    av_dump_format(ffp->m_ofmt_ctx, 0, file_name, 1);
    
    // 打开输出文件
    if (!(ffp->m_ofmt->flags & AVFMT_NOFILE)) {
        av_log(ffp, AV_LOG_DEBUG, "Opening output file: %s", file_name);
        ret = avio_open(&ffp->m_ofmt_ctx->pb, file_name, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(ffp, AV_LOG_ERROR, "Could not open output file '%s': %s", file_name, av_err2str(ret));
            goto end;
        }
    }
    
    // 写视频文件头
    av_log(ffp, AV_LOG_INFO, "Writing file header");
    
    // 检查是否有HEVC流，如果有则设置为hvc1格式
    for (i = 0; i < ffp->m_ofmt_ctx->nb_streams; i++) {
        AVStream *stream = ffp->m_ofmt_ctx->streams[i];
        if (stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            // 确保HEVC流使用'hvc1'标签
            uint32_t hvc1_tag = MKTAG('h','v','c','1');
            stream->codecpar->codec_tag = hvc1_tag;
            if (stream->codec)
                stream->codec->codec_tag = hvc1_tag;
                
            av_log(ffp, AV_LOG_INFO, "Found HEVC stream at index %d, forcing 'hvc1' codec tag", i);
        }
    }
    
    // 创建写入头部的选项
    AVDictionary *header_opts = NULL;
    av_dict_set(&header_opts, "movflags", "faststart+frag_keyframe", 0);
    av_dict_set(&header_opts, "brand", "iso6", 0);
    av_dict_set(&header_opts, "hvc1_tag", "hvc1", 0);  // 强制使用hvc1标签
    
    // 设置MP4容器的兼容性品牌
    char *compatible_brands = "iso6mp42hvc1";
    av_dict_set(&header_opts, "compatible_brands", compatible_brands, 0);
    
    ret = avformat_write_header(ffp->m_ofmt_ctx, &header_opts);
    av_dict_free(&header_opts);
    
    if (ret < 0) {
        av_log(ffp, AV_LOG_ERROR, "Error occurred when opening output file: %s\n", av_err2str(ret));
        goto end;
    }
    av_log(ffp, AV_LOG_INFO, "File header written successfully");
    
    ffp->is_record = 1;
    ffp->record_error = 0;
    ffp->need_transcode = 0; // 明确标记为不需要转码的模式
    ffp->record_start_time = av_gettime() / 1000; // 当前时间（毫秒）
    ffp->min_record_time = 5000; // 至少录制5秒
    ffp->last_video_dts = -1; // 初始化DTS跟踪变量
    ffp->last_audio_dts = -1;
    
    // 初始化互斥锁
    if (pthread_mutex_init(&ffp->record_mutex, NULL) != 0) {
        av_log(ffp, AV_LOG_ERROR, "Failed to initialize recording mutex");
        ffp->is_record = 0;
        goto end;
    }
    
    av_log(ffp, AV_LOG_INFO, "Recording started successfully with direct video copy: %d", ffp->direct_video_copy);
    return 0;
end:
    ffp->record_error = 1;
    return -1;
}

//stop record video. add by poe 2024/07/27. 
int ffp_stop_record(FFPlayer *ffp)
{
    assert(ffp);
    if (ffp->is_record) {
        av_log(ffp, AV_LOG_INFO, "Stopping recording with duration %"PRId64" ms", 
               av_gettime() / 1000 - ffp->record_start_time);
        
        // 先设置标志位，防止新的数据继续写入
        ffp->is_record = 0;
        pthread_mutex_lock(&ffp->record_mutex);
        
        packet_queue_destroy(&ffp->record_audio_queue);

        // 如果是转码模式，先尝试刷新编码器缓冲区
        if (ffp->need_transcode) {
            av_log(ffp, AV_LOG_INFO, "Flushing encoder buffers before stopping");
            
            // 刷新视频编码器
            if (ffp->video_enc_ctx && ffp->out_video_stream_index >= 0) {
                AVPacket enc_pkt;
                int got_packet = 0;
                int ret = 0;
                
                av_init_packet(&enc_pkt);
                enc_pkt.data = NULL;
                enc_pkt.size = 0;
                
                // 发送NULL帧来刷新缓冲区
                av_log(ffp, AV_LOG_DEBUG, "Flushing video encoder");
                while (1) {
                    ret = avcodec_encode_video2(ffp->video_enc_ctx, &enc_pkt, NULL, &got_packet);
                    if (ret < 0 || !got_packet)
                        break;
                    
                    av_log(ffp, AV_LOG_DEBUG, "Got delayed video packet during flush, size=%d", enc_pkt.size);
                    
                    // 设置流索引
                    enc_pkt.stream_index = ffp->out_video_stream_index;
                    
                    // 转换时间戳
                    enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
                                                 ffp->video_enc_ctx->time_base,
                                                 ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index]->time_base,
                                                 AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
                                                 ffp->video_enc_ctx->time_base,
                                                 ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index]->time_base,
                                                 AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    
                    // 写入文件
                    ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, &enc_pkt);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error writing delayed video frame: %s", av_err2str(ret));
        } else {
                        av_log(ffp, AV_LOG_DEBUG, "Successfully wrote delayed video packet");
                    }
                    
                    av_packet_unref(&enc_pkt);
                }
            }
            
            // 刷新音频编码器
            if (ffp->audio_enc_ctx && ffp->out_audio_stream_index >= 0) {
                AVPacket enc_pkt;
                int got_packet = 0;
                int ret = 0;
                
                av_init_packet(&enc_pkt);
                enc_pkt.data = NULL;
                enc_pkt.size = 0;
                
                // 发送NULL帧来刷新缓冲区
                av_log(ffp, AV_LOG_DEBUG, "Flushing audio encoder");
                while (1) {
                    ret = avcodec_encode_audio2(ffp->audio_enc_ctx, &enc_pkt, NULL, &got_packet);
                    if (ret < 0 || !got_packet)
                        break;
                    
                    av_log(ffp, AV_LOG_DEBUG, "Got delayed audio packet during flush, size=%d", enc_pkt.size);
                    
                    // 设置流索引
                    enc_pkt.stream_index = ffp->out_audio_stream_index;
                    
                    // 转换时间戳
                    enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
                                                 ffp->audio_enc_ctx->time_base,
                                                 ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->time_base,
                                                 AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
                                                 ffp->audio_enc_ctx->time_base,
                                                 ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->time_base,
                                                 AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                    
                    // 写入文件
                    ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, &enc_pkt);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error writing delayed audio frame: %s", av_err2str(ret));
            } else {
                        av_log(ffp, AV_LOG_DEBUG, "Successfully wrote delayed audio packet");
                    }
                    
                    av_packet_unref(&enc_pkt);
                }
            }
        }
        
        // 写入文件尾并关闭文件
        if (ffp->m_ofmt_ctx != NULL) {
            // 对于只有视频的流，确保至少有一些数据写入
            if (ffp->video_only && !ffp->is_first) {
                av_log(ffp, AV_LOG_WARNING, "Video-only recording without any frames, trying to write minimal valid file");
                
                // 尝试写入一个空的视频帧，以确保文件有效
                if (ffp->out_video_stream_index >= 0) {
                    AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                    
                    AVPacket pkt;
                    av_init_packet(&pkt);
                    pkt.data = NULL;
                    pkt.size = 0;
                    pkt.stream_index = ffp->out_video_stream_index;
                    pkt.pts = 0;
                    pkt.dts = 0;
                    pkt.duration = 0;
                    pkt.flags = AV_PKT_FLAG_KEY;
                    
                    int ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, &pkt);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error writing empty frame: %s", av_err2str(ret));
                    } else {
                        av_log(ffp, AV_LOG_DEBUG, "Successfully wrote empty frame");
                        ffp->is_first = 1;  // 标记为已写入数据
                    }
                }
            }
            
            av_log(ffp, AV_LOG_INFO, "Writing file trailer...");
            
            // 确保所有HEVC流在写入尾部前保持正确的codec_tag
            for (int i = 0; i < ffp->m_ofmt_ctx->nb_streams; i++) {
                AVStream *stream = ffp->m_ofmt_ctx->streams[i];
                if (stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                    // 不修改已经设置好的codec_tag，确保与之前一致
                    uint32_t hevc_tag = stream->codecpar->codec_tag;
                    if (hevc_tag == 0) {
                        // 如果没有设置，默认使用hvc1
                        hevc_tag = MKTAG('h','v','c','1');
                        stream->codecpar->codec_tag = hevc_tag;
                        if (stream->codec)
                            stream->codec->codec_tag = hevc_tag;
                    }
                    
                    char tag_str[5] = {0};
                    tag_str[0] = (hevc_tag >> 0) & 0xFF;
                    tag_str[1] = (hevc_tag >> 8) & 0xFF;
                    tag_str[2] = (hevc_tag >> 16) & 0xFF;
                    tag_str[3] = (hevc_tag >> 24) & 0xFF;
                    av_log(ffp, AV_LOG_INFO, "Final check: ensuring HEVC stream %d has '%s' codec tag", i, tag_str);
                }
            }
            
            int ret = av_write_trailer(ffp->m_ofmt_ctx);
            if (ret < 0) {
                av_log(ffp, AV_LOG_ERROR, "Error writing trailer: %s", av_err2str(ret));
            } else {
                av_log(ffp, AV_LOG_INFO, "Successfully wrote file trailer");
            }
            
            if (ffp->m_ofmt_ctx && !(ffp->m_ofmt->flags & AVFMT_NOFILE)) {
                avio_flush(ffp->m_ofmt_ctx->pb);
                ret = avio_close(ffp->m_ofmt_ctx->pb);
                if (ret < 0) {
                    av_log(ffp, AV_LOG_ERROR, "Error closing output file: %s", av_err2str(ret));
                } else {
                    av_log(ffp, AV_LOG_INFO, "Successfully closed output file");
                }
            }
            avformat_free_context(ffp->m_ofmt_ctx);
            ffp->m_ofmt_ctx = NULL;
        }
        
        // 清理转码资源
        if (ffp->need_transcode) {
            av_log(ffp, AV_LOG_INFO, "Cleaning up transcode resources");
            
            if (ffp->video_enc_ctx) {
                avcodec_flush_buffers(ffp->video_enc_ctx);
                avcodec_free_context(&ffp->video_enc_ctx);
                ffp->video_enc_ctx = NULL;
            }
            
            if (ffp->audio_enc_ctx) {
                avcodec_flush_buffers(ffp->audio_enc_ctx);
                avcodec_free_context(&ffp->audio_enc_ctx);
                ffp->audio_enc_ctx = NULL;
            }
            
            if (ffp->sws_ctx) {
                sws_freeContext(ffp->sws_ctx);
                ffp->sws_ctx = NULL;
            }
            
            if (ffp->swr_ctx_record) {
                swr_free(&ffp->swr_ctx_record);
                ffp->swr_ctx_record = NULL;
            }
            
            if (ffp->tmp_frame) {
                av_frame_free(&ffp->tmp_frame);
                ffp->tmp_frame = NULL;
            }
            
            ffp->need_transcode = 0;
        }
        
        ffp->is_first = 0;
        pthread_mutex_unlock(&ffp->record_mutex);
        pthread_mutex_destroy(&ffp->record_mutex);
        av_log(ffp, AV_LOG_INFO, "Recording stopped successfully");
    } else {
        av_log(ffp, AV_LOG_WARNING, "Recording is not active, no need to stop");
    }
    return 0;
}

//screen shot .
void ffp_get_current_frame_l(FFPlayer *ffp, uint8_t *frame_buf)
{
  ALOGD("=============>start snapshot\n");

    VideoState *is = ffp->is;
  Frame *vp;
  int i = 0, linesize = 0, pixels = 0;
  uint8_t *src;

  vp = &is->pictq.queue[is->pictq.rindex];
  int height = vp->bmp->h;
  int width = vp->bmp->w;

  ALOGD("=============>%d X %d === %d\n", width, height, vp->bmp->pitches[0]);

  // copy data to bitmap in java code
  linesize = vp->bmp->pitches[0];
  //point to bitmap memory address. 
  src = vp->bmp->pixels[0];
  //rgba use 4 bits .
  pixels = width * 4;
  for (i = 0; i < height; i++) {
    //逐行拷贝. 
      memcpy(frame_buf + i * pixels, src + i * linesize, pixels);
  }
  
  ALOGD("=============>end snapshot\n");
}

static int ffp_write_record_packet(FFPlayer *ffp, AVPacket *packet)
{
    VideoState *is = ffp->is;
    AVStream *in_stream = is->ic->streams[packet->stream_index];
    AVStream *out_stream = ffp->m_ofmt_ctx->streams[packet->stream_index];
    int ret = 0;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return AVERROR(ENOMEM);
    }
    av_packet_ref(pkt, packet);

    // Rescale PTS/DTS
    if (ffp->record_first_vpts != AV_NOPTS_VALUE) {
        pkt->pts = av_rescale_q_rnd(pkt->pts - ffp->record_first_vpts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts - ffp->record_first_vpts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    } else {
        pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base, (enum AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
    }

    pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
    pkt->pos = -1;

    // Ensure DTS is not behind the last one for this stream
    if (packet->stream_index == is->video_stream && ffp->last_video_dts >= pkt->dts) {
        pkt->dts = ffp->last_video_dts + 1;
    }
    if (packet->stream_index == is->audio_stream && ffp->last_audio_dts >= pkt->dts) {
        pkt->dts = ffp->last_audio_dts + 1;
    }
    // Also ensure PTS is not behind DTS
    if (pkt->pts < pkt->dts) {
        pkt->pts = pkt->dts;
    }

    if ((ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, pkt)) < 0) {
        av_log(ffp, AV_LOG_ERROR, "Error muxing packet: %s\n", av_err2str(ret));
    } else {
        // Update last DTS
        if (packet->stream_index == is->video_stream) {
            ffp->last_video_dts = pkt->dts;
        }
        if (packet->stream_index == is->audio_stream) {
            ffp->last_audio_dts = pkt->dts;
        }
    }

    av_packet_free(&pkt);
    return ret;
}

//保存文件
int ffp_record_file(FFPlayer *ffp, AVPacket *packet,int64_t recordTs) {
    assert(ffp);
    VideoState *is = ffp->is;
    int ret = 0;
    AVStream *in_stream;
    AVStream *out_stream;

    if (ffp->is_record) {
        if (packet == NULL) {
            ffp->record_error = 1;
            av_log(ffp, AV_LOG_ERROR, "packet == NULL");
            return -1;
        }

        pthread_mutex_lock(&ffp->record_mutex);

        // 如果需要转码
        if (ffp->need_transcode) {
            av_log(ffp, AV_LOG_DEBUG, "Processing packet for transcoding, stream_index=%d, size=%d", 
                   packet->stream_index, packet->size);
                   
            // 检查是否是视频或音频流
            if (packet->stream_index == is->video_stream && ffp->out_video_stream_index >= 0) {
                // 检查是否使用直接视频流复制模式
                if (ffp->direct_video_copy) {
                    av_log(ffp, AV_LOG_DEBUG, "Direct video packet copying mode");
                    
                    // 检查输出上下文是否有效
                    if (!ffp->m_ofmt_ctx) {
                        av_log(ffp, AV_LOG_ERROR, "Output format context is NULL");
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(EINVAL);
                    }
                    
                    // 检查输出视频流索引是否有效
                    if (ffp->out_video_stream_index < 0 || 
                        ffp->out_video_stream_index >= ffp->m_ofmt_ctx->nb_streams) {
                        av_log(ffp, AV_LOG_ERROR, "Invalid output video stream index: %d", 
                               ffp->out_video_stream_index);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(EINVAL);
                    }
                    
                    AVPacket *pkt = av_packet_alloc();
                    if (!pkt) {
                        av_log(ffp, AV_LOG_ERROR, "Failed to allocate packet for direct video copy");
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(ENOMEM);
                    }
                    
                    if (av_packet_ref(pkt, packet) < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Failed to reference packet");
                        av_packet_free(&pkt);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(ENOMEM);
                    }
                    
                    // 设置流索引
                    pkt->stream_index = ffp->out_video_stream_index;
                    
                    // 检查输入流是否有效
                    if (packet->stream_index < 0 || packet->stream_index >= is->ic->nb_streams) {
                        av_log(ffp, AV_LOG_ERROR, "Invalid input stream index: %d", packet->stream_index);
                        av_packet_unref(pkt);
                        av_packet_free(&pkt);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(EINVAL);
                    }
                    
                    // 转换时间戳
                    AVStream *in_stream = is->ic->streams[packet->stream_index];
                    AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                    
                    if (!in_stream || !out_stream) {
                        av_log(ffp, AV_LOG_ERROR, "Invalid stream pointers: in_stream=%p, out_stream=%p", 
                               in_stream, out_stream);
                        av_packet_unref(pkt);
                        av_packet_free(&pkt);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(EINVAL);
                    }
                    
                    // Use the global last_video_dts instead of a static variable
                    
                    if (!ffp->is_first) {
                        ffp->is_first = 1;
                        
                        // 使用当前帧的PTS/DTS作为起始时间，而不是使用可能有大偏移的值
                        // 这样可以避免前几秒没有画面的问题
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            ffp->start_pts = pkt->pts;
                        } else {
                            ffp->start_pts = 0;
                        }
                        
                        if (pkt->dts != AV_NOPTS_VALUE) {
                            ffp->start_dts = pkt->dts;
                        } else {
                            ffp->start_dts = ffp->start_pts;
                        }
                        
                        ffp->record_real_time = av_gettime_relative() / 1000; // 记录实际开始时间（毫秒）
                        
                        // 第一帧从0开始
                        pkt->pts = 0;
                        pkt->dts = 0;
                        ffp->last_video_dts = 0; // Initialize last DTS
                        av_log(ffp, AV_LOG_INFO, "First video packet: pts=%"PRId64", dts=%"PRId64", real_time=%"PRId64", keyframe=%d",
                               ffp->start_pts, ffp->start_dts, ffp->record_real_time, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                    } else {
                        // 处理后续视频帧
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t pts_diff = pkt->pts - ffp->start_pts;
                            
                            // 确保时间差不是负数，这会导致前几秒没有画面
                            if (pts_diff < 0) {
                                pts_diff = 0;
                                av_log(ffp, AV_LOG_WARNING, "Negative PTS diff detected, correcting to 0");
                            }
                            
                            // 转换到输出时基
                            pkt->pts = av_rescale_q(pts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保PTS始终递增
                            if (pkt->pts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                pkt->pts = ffp->last_video_dts + 1;
                            }
                            
                            av_log(ffp, AV_LOG_DEBUG, "Video packet: pts_diff=%"PRId64", new_pts=%"PRId64", keyframe=%d", 
                                pts_diff, pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                        }
                        
                        // 处理DTS
                        if (pkt->dts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t dts_diff = pkt->dts - ffp->start_dts;
                            
                            // 转换到输出时基
                            pkt->dts = av_rescale_q(dts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保DTS始终递增
                            if (pkt->dts <= ffp->last_video_dts) {
                                pkt->dts = ffp->last_video_dts + 1;
                            }
                            
                            ffp->last_video_dts = pkt->dts;
                            
                            // 确保PTS不小于DTS
                            if (pkt->pts < pkt->dts) {
                                pkt->pts = pkt->dts;
                            }
                        } else {
                            // 如果没有DTS，使用PTS
                            if (pkt->pts != AV_NOPTS_VALUE) {
                                pkt->dts = pkt->pts;
                                
                                // 确保DTS始终递增
                                if (pkt->dts <= ffp->last_video_dts) {
                                    pkt->dts = ffp->last_video_dts + 1;
                                    pkt->pts = pkt->dts; // 保持PTS和DTS同步
                                }
                                
                                ffp->last_video_dts = pkt->dts;
                            }
                        }
                    }
                    
                    // 转换持续时间
                    if (pkt->duration > 0) {
                        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
                    }
                    pkt->pos = -1;
                    
                    // 写入文件
                    av_log(ffp, AV_LOG_INFO, "Writing video packet directly, size=%d, pts=%"PRId64", dts=%"PRId64", keyframe=%d, tb=%d/%d",
                           pkt->size, pkt->pts, pkt->dts, 
                           (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0,
                           ffp->m_ofmt_ctx->streams[pkt->stream_index]->time_base.num,
                           ffp->m_ofmt_ctx->streams[pkt->stream_index]->time_base.den);
                    ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, pkt);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error writing video packet: %s", av_err2str(ret));
                    } else {
                        av_log(ffp, AV_LOG_INFO, "Successfully wrote video packet of size %d", pkt->size);
                    }
                    
                    av_packet_unref(pkt);
                    av_packet_free(&pkt);
                    
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return ret;
                }
                
                // 检查是否是HEVC视频，如果是，可以直接写入包而不尝试解码
                int is_hevc = (is->viddec.avctx && is->viddec.avctx->codec_id == AV_CODEC_ID_HEVC);
                
                // 对于HEVC视频，使用直接写入方式以避免解码问题
                if (is_hevc && ffp->direct_hevc_write) {
                    av_log(ffp, AV_LOG_DEBUG, "Direct HEVC packet writing mode, bypassing decode/encode");
                    
                    AVPacket *pkt_copy = (AVPacket *)av_malloc(sizeof(AVPacket));
                    if (!pkt_copy) {
                        av_log(ffp, AV_LOG_ERROR, "Failed to allocate packet for direct HEVC writing");
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(ENOMEM);
                    }
                    
                    // 确保输出流使用正确的codec_tag
                    if (ffp->out_video_stream_index >= 0 && ffp->out_video_stream_index < ffp->m_ofmt_ctx->nb_streams) {
                        AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                        AVStream *in_stream = is->ic->streams[packet->stream_index];
                        
                        if (out_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                            // 检查输入流的codec_tag
                            uint32_t hevc_tag;
                            if (in_stream->codecpar->codec_tag != 0) {
                                // 保持与输入流相同的codec_tag
                                hevc_tag = in_stream->codecpar->codec_tag;
                                char tag_str[5] = {0};
                                tag_str[0] = (hevc_tag >> 0) & 0xFF;
                                tag_str[1] = (hevc_tag >> 8) & 0xFF;
                                tag_str[2] = (hevc_tag >> 16) & 0xFF;
                                tag_str[3] = (hevc_tag >> 24) & 0xFF;
                                av_log(ffp, AV_LOG_DEBUG, "Preserving original HEVC codec tag during packet writing: '%s' (0x%08x)", 
                                       tag_str, hevc_tag);
                            } else {
                                // 默认使用hvc1标签
                                hevc_tag = MKTAG('h','v','c','1');
                                av_log(ffp, AV_LOG_DEBUG, "Using default 'hvc1' codec tag for HEVC stream");
                            }
                            
                            out_stream->codecpar->codec_tag = hevc_tag;
                            if (out_stream->codec)
                                out_stream->codec->codec_tag = hevc_tag;
                                
                            // 确保MP4容器使用��确的标签
                            if (hevc_tag == MKTAG('h','v','c','1')) {
                                av_dict_set(&ffp->m_ofmt_ctx->metadata, "compatible_brands", "iso6mp42hvc1", 0);
                            } else if (hevc_tag == MKTAG('h','e','v','1')) {
                                av_dict_set(&ffp->m_ofmt_ctx->metadata, "compatible_brands", "iso6mp42hev1", 0);
                            }
                        }
                    }
                    
                    // 使用av_packet_alloc替代av_new_packet
                    if (0 == av_packet_ref(pkt_copy, packet)) {
                        // 确保NAL单元类型在日志中可见
                        if (pkt_copy->size >= 5) {
                            uint8_t nal_type = (pkt_copy->data[4] >> 1) & 0x3f;
                            av_log(ffp, AV_LOG_DEBUG, "HEVC NAL unit type: %d, size: %d", nal_type, pkt_copy->size);
                        }
                        // 设置流索引
                        pkt_copy->stream_index = ffp->out_video_stream_index;
                        
                        // 转换时间戳
                        AVStream *in_stream = is->ic->streams[packet->stream_index];
                        AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                         // Use the global last_video_dts for HEVC packets as well
                    
                        if (!ffp->is_first) {
                            ffp->is_first = 1;
                            
                            // 使用当前帧的PTS/DTS作为起始时间，而不是使用可能有大偏移的值
                            // 这样可以避免前几秒没有画面的问题
                            if (pkt_copy->pts != AV_NOPTS_VALUE) {
                                ffp->start_pts = pkt_copy->pts;
                            } else {
                                ffp->start_pts = 0;
                            }
                            
                            if (pkt_copy->dts != AV_NOPTS_VALUE) {
                                ffp->start_dts = pkt_copy->dts;
                            } else {
                                ffp->start_dts = ffp->start_pts;
                            }
                            
                            ffp->record_real_time = av_gettime_relative() / 1000; // 记录实际开始时间（毫秒）
                            
                            // 第一帧从0开始
                            pkt_copy->pts = 0;
                            pkt_copy->dts = 0;
                            ffp->last_video_dts = 0; // Initialize last DTS
                            av_log(ffp, AV_LOG_INFO, "First HEVC packet: pts=%"PRId64", dts=%"PRId64", real_time=%"PRId64", keyframe=%d",
                                ffp->start_pts, ffp->start_dts, ffp->record_real_time, (pkt_copy->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                        } else {
                            // 处理后续视频帧
                            if (pkt_copy->pts != AV_NOPTS_VALUE) {
                                // 计算相对于第一帧的时间差
                                int64_t pts_diff = pkt_copy->pts - ffp->start_pts;
                                
                                // 确保时间差不是负数，这会导致前几秒没有画面
                                if (pts_diff < 0) {
                                    pts_diff = 0;
                                    av_log(ffp, AV_LOG_WARNING, "Negative HEVC PTS diff detected, correcting to 0");
                                }
                                
                                // 转换到输出时基
                                pkt_copy->pts = av_rescale_q(pts_diff, in_stream->time_base, out_stream->time_base);
                                
                                // 确保PTS始终递增
                                if (pkt_copy->pts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                    pkt_copy->pts = ffp->last_video_dts + 1;
                                }
                                
                                av_log(ffp, AV_LOG_DEBUG, "HEVC packet: pts_diff=%"PRId64", new_pts=%"PRId64", keyframe=%d", 
                                    pts_diff, pkt_copy->pts, (pkt_copy->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                            }
                            
                            // 处理DTS
                            if (pkt_copy->dts != AV_NOPTS_VALUE) {
                                // 计算相对于第一帧的时间差
                                int64_t dts_diff = pkt_copy->dts - ffp->start_dts;
                                
                                // 确保时间差不是负数，这会导致前几秒没有画面
                                if (dts_diff < 0) {
                                    dts_diff = 0;
                                    av_log(ffp, AV_LOG_WARNING, "Negative HEVC DTS diff detected, correcting to 0");
                                }
                                
                                // 转换到输出时基
                                pkt_copy->dts = av_rescale_q(dts_diff, in_stream->time_base, out_stream->time_base);
                                
                                // 确保DTS始终递增
                                if (pkt_copy->dts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                    pkt_copy->dts = ffp->last_video_dts + 1;
                                }
                                
                                ffp->last_video_dts = pkt_copy->dts;
                                
                                // 确保PTS不小于DTS
                                if (pkt_copy->pts < pkt_copy->dts) {
                                    pkt_copy->pts = pkt_copy->dts;
                                }
                            } else {
                                // 如果没有DTS，使用PTS
                                if (pkt_copy->pts != AV_NOPTS_VALUE) {
                                    pkt_copy->dts = pkt_copy->pts;
                                    
                                    // 确保DTS始终递增
                                    if (pkt_copy->dts <= ffp->last_video_dts) {
                                        pkt_copy->dts = ffp->last_video_dts + 1;
                                        pkt_copy->pts = pkt_copy->dts; // 保持PTS和DTS同步
                                    }
                                    
                                    ffp->last_video_dts = pkt_copy->dts;
                                }
                            }
                        }
                        
                        // 转换持续时间
                        if (pkt_copy->duration > 0) {
                            pkt_copy->duration = av_rescale_q(pkt_copy->duration, in_stream->time_base, out_stream->time_base);
                        }
                        
                        // 写入文件
                        av_log(ffp, AV_LOG_DEBUG, "Writing HEVC packet directly, size=%d, pts=%"PRId64", dts=%"PRId64", duration=%"PRId64", keyframe=%d, tb=%d/%d",
                               pkt_copy->size, pkt_copy->pts, pkt_copy->dts, pkt_copy->duration,
                               (pkt_copy->flags & AV_PKT_FLAG_KEY) ? 1 : 0,
                               out_stream->time_base.num, out_stream->time_base.den);
                        
                        // 确保包有效载荷不为空
                        if (pkt_copy->size == 0 || !pkt_copy->data) {
                            av_log(ffp, AV_LOG_WARNING, "Empty packet detected, skipping write");
                        } else {
                            int ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, pkt_copy);
                            if (ret < 0) {
                                av_log(ffp, AV_LOG_ERROR, "Error writing direct HEVC frame: %s", av_err2str(ret));
                            } else {
                                av_log(ffp, AV_LOG_DEBUG, "Successfully wrote HEVC packet of size %d", pkt_copy->size);
                            }
                        }
                        
                        av_packet_unref(pkt_copy);
                        av_packet_free(&pkt_copy);
                        
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    } else {
                        av_log(ffp, AV_LOG_ERROR, "Failed to reference packet for HEVC direct writing");
                        av_packet_free(&pkt_copy);
                    }
                }
                
                // 标准视频转码路径
                AVFrame *frame = av_frame_alloc();
                if (!frame) {
                    av_log(ffp, AV_LOG_ERROR, "Failed to allocate video frame");
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return AVERROR(ENOMEM);
                }

                // 解码视频帧
                int got_frame = 0;
                av_log(ffp, AV_LOG_DEBUG, "Decoding video packet for recording, size=%d", packet->size);
                ret = avcodec_decode_video2(is->viddec.avctx, frame, &got_frame, packet);
                if (ret < 0) {
                    av_log(ffp, AV_LOG_ERROR, "Error decoding video frame: %s", av_err2str(ret));
                    av_frame_free(&frame);
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return ret;
                }

                if (got_frame) {
                    av_log(ffp, AV_LOG_DEBUG, "Got video frame: %dx%d, format=%d, pts=%"PRId64, 
                           frame->width, frame->height, frame->format, frame->pts);
                    
                    // 设置时间戳
                    if (!ffp->is_first) {
                        ffp->is_first = 1;
                        ffp->start_pts = frame->pts;
                        ffp->record_real_time = av_gettime_relative() / 1000; // 记录实际开始时间（毫秒）
                        frame->pts = 0;
                        av_log(ffp, AV_LOG_DEBUG, "First video frame, start_pts=%"PRId64", real_time=%"PRId64, 
                               ffp->start_pts, ffp->record_real_time);
                    } else {
                        // 计算当前实际经过的时间（毫秒）
                        int64_t elapsed_real_time = av_gettime_relative() / 1000 - ffp->record_real_time;
                        
                        // 基于实际时间计算PTS，确保正常速度
                        frame->pts = av_rescale_q(elapsed_real_time, 
                                                (AVRational){1, 1000}, // 毫秒时基
                                                ffp->video_enc_ctx->time_base);
                        
                        av_log(ffp, AV_LOG_DEBUG, "Video frame: real_time=%"PRId64"ms, new_pts=%"PRId64, 
                               elapsed_real_time, frame->pts);
                    }

                    // 确保tmp_frame已经分配
                    if (!ffp->tmp_frame) {
                        ffp->tmp_frame = av_frame_alloc();
                        if (!ffp->tmp_frame) {
                            av_log(ffp, AV_LOG_ERROR, "Failed to allocate temporary frame");
                            av_frame_free(&frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return AVERROR(ENOMEM);
                        }
                    }
                    
                    // 转换像素格式
                    av_image_fill_arrays(ffp->tmp_frame->data, ffp->tmp_frame->linesize,
                                        NULL, AV_PIX_FMT_YUV420P,
                                        ffp->video_enc_ctx->width, ffp->video_enc_ctx->height, 1);
                    
                    // 分配缓冲区
                    int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 
                                                       ffp->video_enc_ctx->width,
                                                       ffp->video_enc_ctx->height, 1);
                    uint8_t *buffer = av_malloc(size);
                    if (!buffer) {
                        av_log(ffp, AV_LOG_ERROR, "Failed to allocate buffer for video conversion");
                        av_frame_free(&frame);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return AVERROR(ENOMEM);
                    }
                    
                    av_image_fill_arrays(ffp->tmp_frame->data, ffp->tmp_frame->linesize,
                                        buffer, AV_PIX_FMT_YUV420P,
                                        ffp->video_enc_ctx->width, ffp->video_enc_ctx->height, 1);

                    // 转换像素格式
                    av_log(ffp, AV_LOG_DEBUG, "Converting video format with sws_scale");
                    sws_scale(ffp->sws_ctx, (const uint8_t * const*)frame->data,
                             frame->linesize, 0, frame->height,
                             ffp->tmp_frame->data, ffp->tmp_frame->linesize);

                    // 设置帧参数
                    ffp->tmp_frame->width = ffp->video_enc_ctx->width;
                    ffp->tmp_frame->height = ffp->video_enc_ctx->height;
                    ffp->tmp_frame->format = AV_PIX_FMT_YUV420P;
                    ffp->tmp_frame->pts = av_rescale_q(frame->pts, 
                                                     is->viddec.avctx->time_base,
                                                     ffp->video_enc_ctx->time_base);

                    // 编码帧
                    AVPacket enc_pkt;
                    av_init_packet(&enc_pkt);
                    enc_pkt.data = NULL;
                    enc_pkt.size = 0;

                    int got_packet = 0;
                    av_log(ffp, AV_LOG_DEBUG, "Encoding video frame, pts=%"PRId64, ffp->tmp_frame->pts);
                    ret = avcodec_encode_video2(ffp->video_enc_ctx, &enc_pkt, ffp->tmp_frame, &got_packet);
                    
                    av_free(buffer);
                    av_frame_free(&frame);
                    
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error encoding video frame: %s", av_err2str(ret));
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    }

                    if (got_packet) {
                        av_log(ffp, AV_LOG_DEBUG, "Got encoded video packet, size=%d, pts=%"PRId64, 
                               enc_pkt.size, enc_pkt.pts);
                        
                        // 设置流索引
                        enc_pkt.stream_index = ffp->out_video_stream_index;
                        
                        // 转换时间戳
                        av_log(ffp, AV_LOG_DEBUG, "Rescaling video timestamps, before: pts=%"PRId64", dts=%"PRId64,
                               enc_pkt.pts, enc_pkt.dts);
                               
                        enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
                                                     ffp->video_enc_ctx->time_base,
                                                     ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index]->time_base,
                                                     AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                        enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
                                                     ffp->video_enc_ctx->time_base,
                                                     ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index]->time_base,
                                                     AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                        enc_pkt.duration = av_rescale_q(enc_pkt.duration,
                                                      ffp->video_enc_ctx->time_base,
                                                      ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index]->time_base);
                        
                        av_log(ffp, AV_LOG_DEBUG, "Rescaled video timestamps, after: pts=%"PRId64", dts=%"PRId64,
                               enc_pkt.pts, enc_pkt.dts);
                        
                        // 写入文件
                        av_log(ffp, AV_LOG_DEBUG, "Writing video packet to file, size=%d", enc_pkt.size);
                        ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, &enc_pkt);
                        if (ret < 0) {
                            av_log(ffp, AV_LOG_ERROR, "Error writing video frame: %s", av_err2str(ret));
                        } else {
                            av_log(ffp, AV_LOG_DEBUG, "Successfully wrote video packet");
                        }
                        
                        av_packet_unref(&enc_pkt);
                    } else {
                        av_log(ffp, AV_LOG_DEBUG, "No packet produced from video encoding");
                    }
                } else {
                    // 添加静态计数器来跟踪解码失败次数
                    static int decode_failed_count = 0;
                    decode_failed_count++;
                    
                    av_log(ffp, AV_LOG_DEBUG, "No frame decoded from video packet (%d attempts)", decode_failed_count);
                    av_frame_free(&frame);
                    
                    // 检查是否是HEVC视频，这种情况下可能会有解码问题
                    int is_hevc = (is->viddec.avctx && is->viddec.avctx->codec_id == AV_CODEC_ID_HEVC);
                    if (is_hevc) {
                        av_log(ffp, AV_LOG_WARNING, "HEVC video detected, using special handling for decoder");
                    }
                    
                    // 如果是HEVC或解码失败次数较多，但还没有成功解码过任何帧，则不要设置错误标志
                    // 这样可以让录制继续，直到成功解码出第一帧
                    if ((is_hevc || decode_failed_count > 15) && !ffp->is_first) {
                        av_log(ffp, AV_LOG_WARNING, "Failed to decode frames after multiple attempts, but continuing to try");
                        // 不返回错误，继续尝试解码
                    }
                    
                    // 对于HEVC、RTSP/RTMP流或只有视频的流，如果解码失败，直接写入原始包
                    // 这样可以确保至少有数据写入文件
                    int is_rtsp = (is->ic && is->ic->iformat && is->ic->iformat->name && 
                                 (strstr(is->ic->iformat->name, "rtsp") || 
                                  strstr(is->ic->url, "rtsp") ||
                                  strstr(is->ic->url, "rtmp")));
                    
                    if ((ffp->video_only || is_hevc || is_rtsp) && decode_failed_count > 5) {
                        av_log(ffp, AV_LOG_INFO, "Special stream mode (%s): Writing original packet directly", 
                               is_hevc ? "HEVC" : (is_rtsp ? "RTSP/RTMP" : "video-only"));
                        
                        AVPacket *pkt_copy = (AVPacket *)av_malloc(sizeof(AVPacket));
                        if (!pkt_copy) {
                            av_log(ffp, AV_LOG_ERROR, "Failed to allocate packet for direct writing");
                            return AVERROR(ENOMEM);
                        }
                        
                        av_new_packet(pkt_copy, 0);
                        if (0 == av_packet_ref(pkt_copy, packet)) {
                            // 设置流索引
                            pkt_copy->stream_index = ffp->out_video_stream_index;
                            
                            // 转换时间戳
                            AVStream *in_stream = is->ic->streams[packet->stream_index];
                            AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                            
                            if (!ffp->is_first) {
                                ffp->is_first = 1;
                                ffp->start_pts = pkt_copy->pts;
                                ffp->start_dts = pkt_copy->dts;
                                pkt_copy->pts = 0;
                                pkt_copy->dts = 0;
                            } else {
                                pkt_copy->pts = abs(pkt_copy->pts - ffp->start_pts);
                                pkt_copy->dts = abs(pkt_copy->dts - ffp->start_dts);
                            }
                            
                            pkt_copy->pts = av_rescale_q_rnd(pkt_copy->pts, 
                                                          in_stream->time_base, 
                                                          out_stream->time_base, 
                                                          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                            pkt_copy->dts = av_rescale_q_rnd(pkt_copy->dts, 
                                                          in_stream->time_base, 
                                                          out_stream->time_base, 
                                                          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                            pkt_copy->duration = av_rescale_q(pkt_copy->duration, 
                                                           in_stream->time_base, 
                                                           out_stream->time_base);
                            pkt_copy->pos = -1;
                            
                            // 写入文件
                            int ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, pkt_copy);
                            if (ret < 0) {
                                av_log(ffp, AV_LOG_ERROR, "Error writing direct packet: %s", av_err2str(ret));
                            } else {
                                av_log(ffp, AV_LOG_DEBUG, "Successfully wrote direct packet, size=%d", pkt_copy->size);
                            }
                        }
                        
                        av_packet_unref(pkt_copy);
                        av_free(pkt_copy);
                    }
                }
            } else if (packet->stream_index == is->audio_stream && ffp->out_audio_stream_index >= 0) {
                // 音频转码
                // 检查是否是PCM音频，需要特殊处理
                int is_pcm = 0;
                if (is->auddec.avctx) {
                    enum AVCodecID codec_id = is->auddec.avctx->codec_id;
                    is_pcm = (codec_id == AV_CODEC_ID_PCM_MULAW || 
                              codec_id == AV_CODEC_ID_PCM_ALAW ||
                              codec_id == AV_CODEC_ID_PCM_S16LE ||
                              codec_id == AV_CODEC_ID_PCM_S16BE ||
                              codec_id == AV_CODEC_ID_PCM_U8);
                }
                
                if (is_pcm && strcmp(ffp->m_ofmt->name, "mp4") == 0) {
                    av_log(ffp, AV_LOG_INFO, "Handling PCM to AAC conversion for MP4 container (codec_id=%d)",
                          is->auddec.avctx ? is->auddec.avctx->codec_id : -1);
                    
                    // 确保输出流标记为AAC
                    ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->codecpar->codec_id = AV_CODEC_ID_AAC;
                    if (ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->codec) {
                        ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->codec->codec_id = AV_CODEC_ID_AAC;
                        ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->codec->codec_tag = 0;
                    }
                }
                
                AVFrame *frame = av_frame_alloc();
                if (!frame) {
                    av_log(ffp, AV_LOG_ERROR, "Failed to allocate audio frame");
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return AVERROR(ENOMEM);
                }

                // 解码音频帧
                int got_frame = 0;
                av_log(ffp, AV_LOG_DEBUG, "Decoding audio packet for recording, size=%d", packet->size);
                
                // 对于PCM音频，直接复制数据而不解码
                if (is->auddec.avctx && 
                   (is->auddec.avctx->codec_id == AV_CODEC_ID_PCM_MULAW || 
                    is->auddec.avctx->codec_id == AV_CODEC_ID_PCM_ALAW ||
                    is->auddec.avctx->codec_id == AV_CODEC_ID_PCM_S16LE ||
                    is->auddec.avctx->codec_id == AV_CODEC_ID_PCM_S16BE ||
                    is->auddec.avctx->codec_id == AV_CODEC_ID_PCM_U8)) {
                    
                    av_log(ffp, AV_LOG_INFO, "PCM audio detected, using direct frame creation instead of decoding");
                    
                    // 为PCM数据创建一个新帧
                    int sample_size = av_get_bytes_per_sample(is->auddec.avctx->sample_fmt);
                    int num_samples = packet->size / (sample_size * is->auddec.avctx->channels);
                    
                    av_log(ffp, AV_LOG_DEBUG, "Creating PCM frame: sample_size=%d, channels=%d, num_samples=%d", 
                           sample_size, is->auddec.avctx->channels, num_samples);
                    
                    // 设置帧属性
                    frame->format = is->auddec.avctx->sample_fmt;
                    frame->channel_layout = is->auddec.avctx->channel_layout;
                    if (!frame->channel_layout) {
                        frame->channel_layout = av_get_default_channel_layout(is->auddec.avctx->channels);
                    }
                    frame->channels = is->auddec.avctx->channels;
                    frame->sample_rate = is->auddec.avctx->sample_rate;
                    frame->nb_samples = num_samples;
                    
                    // 分配帧缓冲区
                    ret = av_frame_get_buffer(frame, 0);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Could not allocate frame data: %s", av_err2str(ret));
                        av_frame_free(&frame);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    }
                    
                    // 确保帧数据可写
                    ret = av_frame_make_writable(frame);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Could not make frame writable: %s", av_err2str(ret));
                        av_frame_free(&frame);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    }
                    
                    // 复制数据到帧
                    memcpy(frame->data[0], packet->data, packet->size);
                    
                    // 设置时间戳
                    frame->pts = packet->pts;
                    got_frame = 1;
                    
                    av_log(ffp, AV_LOG_DEBUG, "Successfully created PCM frame with %d samples", frame->nb_samples);
                } else {
                    // 使用send/receive API代替已弃用的avcodec_decode_audio4
                    ret = avcodec_send_packet(is->auddec.avctx, packet);
                    if (ret < 0) {
                        av_log(ffp, AV_LOG_ERROR, "Error sending packet for decoding: %s", av_err2str(ret));
                        av_frame_free(&frame);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    }
                    
                    ret = avcodec_receive_frame(is->auddec.avctx, frame);
                    if (ret == 0) {
                        got_frame = 1;
                        av_log(ffp, AV_LOG_DEBUG, "Successfully decoded audio frame with %d samples", frame->nb_samples);
                    } else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        // 需要更多输入数据或者已到达文件末尾
                        got_frame = 0;
                        ret = 0;
                        av_log(ffp, AV_LOG_DEBUG, "Need more input data for audio decoding");
                    } else {
                        av_log(ffp, AV_LOG_ERROR, "Error receiving audio frame: %s", av_err2str(ret));
                        av_frame_free(&frame);
                        pthread_mutex_unlock(&ffp->record_mutex);
                        return ret;
                    }
                }

                if (got_frame) {
                    av_log(ffp, AV_LOG_DEBUG, "Got audio frame: samples=%d, format=%d, pts=%"PRId64, 
                           frame->nb_samples, frame->format, frame->pts);
                    
                    // 设置时间戳
                    if (!ffp->is_first) {
                        ffp->is_first = 1;
                        ffp->start_pts = frame->pts;
                        frame->pts = 0;
                        av_log(ffp, AV_LOG_DEBUG, "First audio frame, start_pts=%"PRId64, ffp->start_pts);
                    } else {
                        frame->pts = abs(frame->pts - ffp->start_pts);
                    }

                    // 检查是否需要重采样
                    int do_resample = 0;
                    
                    // 如果采样率、通道数或格式不同，需要重采样
                    if (frame->sample_rate != ffp->audio_enc_ctx->sample_rate ||
                        frame->format != ffp->audio_enc_ctx->sample_fmt ||
                        frame->channel_layout != ffp->audio_enc_ctx->channel_layout) {
                        do_resample = 1;
                    }
                    
                    // 如果不需要重采样，直接使用原始帧
                    AVFrame *resampled_frame;
                    int dst_nb_samples = 0;
                    
                    if (do_resample) {
                        // 确保重采样上下文存在
                        if (!ffp->swr_ctx_record) {
                            av_log(ffp, AV_LOG_ERROR, "Resampler context not initialized");
                            av_frame_free(&frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return AVERROR(EINVAL);
                        }
                        
                        // 计算目标采样数
                        dst_nb_samples = av_rescale_rnd(
                            swr_get_delay(ffp->swr_ctx_record, frame->sample_rate) + frame->nb_samples,
                            ffp->audio_enc_ctx->sample_rate, 
                            frame->sample_rate, 
                            AV_ROUND_UP);
                        
                        av_log(ffp, AV_LOG_DEBUG, "Resampling audio: in_rate=%d, out_rate=%d, in_format=%d, out_format=%d, dst_samples=%d", 
                               frame->sample_rate, ffp->audio_enc_ctx->sample_rate, 
                               frame->format, ffp->audio_enc_ctx->sample_fmt, dst_nb_samples);
                        
                        // 分配临时帧
                        resampled_frame = av_frame_alloc();
                        if (!resampled_frame) {
                            av_log(ffp, AV_LOG_ERROR, "Failed to allocate resampled frame");
                            av_frame_free(&frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return AVERROR(ENOMEM);
                        }
                    } else {
                        // 不需要重采样，直接使用原始帧
                        av_log(ffp, AV_LOG_DEBUG, "No resampling needed, using original frame");
                        resampled_frame = frame;
                        dst_nb_samples = frame->nb_samples;
                    }
                    
                    // 如果需要重采样，设置重采样帧属性并执行重采样
                    if (do_resample) {
                        // 设置重采样帧属性
                        resampled_frame->format = ffp->audio_enc_ctx->sample_fmt;
                        resampled_frame->channel_layout = ffp->audio_enc_ctx->channel_layout;
                        resampled_frame->sample_rate = ffp->audio_enc_ctx->sample_rate;
                        resampled_frame->nb_samples = dst_nb_samples;
                        
                        // 分配重采样帧缓冲区
                        ret = av_frame_get_buffer(resampled_frame, 0);
                        if (ret < 0) {
                            av_log(ffp, AV_LOG_ERROR, "Failed to allocate buffer for resampled frame: %s", av_err2str(ret));
                            av_frame_free(&resampled_frame);
                            av_frame_free(&frame);
                            av_frame_free(&resampled_frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return ret;
                        }
                    
                        // 执行重采样
                        av_log(ffp, AV_LOG_INFO, "Resampling audio frame: in_samples=%d, out_samples=%d, in_fmt=%d, out_fmt=%d, in_ch=%d, out_ch=%d",
                            frame->nb_samples, dst_nb_samples, frame->format, resampled_frame->format,
                            frame->channels, resampled_frame->channels);
                        
                        // 确保重采样器已正确初始化
                        if (!ffp->swr_ctx_record) {
                            av_log(ffp, AV_LOG_ERROR, "Resampler context is NULL");
                            av_frame_free(&frame);
                            av_frame_free(&resampled_frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return AVERROR(EINVAL);
                        }
                        
                        // 检查输入输出格式是否匹配
                        if (frame->format == resampled_frame->format &&
                            frame->sample_rate == resampled_frame->sample_rate &&
                            frame->channel_layout == resampled_frame->channel_layout &&
                            frame->nb_samples <= dst_nb_samples) {
                            // 格式完全匹配，可以直接复制数据
                            av_log(ffp, AV_LOG_DEBUG, "Audio formats match, performing direct copy");
                            av_frame_copy(resampled_frame, frame);
                            ret = frame->nb_samples;
                        } else {
                            // 需要重采样
                            ret = swr_convert(ffp->swr_ctx_record, 
                                            resampled_frame->data, dst_nb_samples,
                                            (const uint8_t **)frame->data, frame->nb_samples);
                        }
                        
                        if (ret < 0) {
                            av_log(ffp, AV_LOG_ERROR, "Failed to resample audio: %s", av_err2str(ret));
                            av_frame_free(&frame);
                            av_frame_free(&resampled_frame);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return ret;
                        }
                        
                        av_log(ffp, AV_LOG_DEBUG, "Audio resampling successful, output samples=%d", ret);
                        
                        // 设置时间戳
                        resampled_frame->pts = av_rescale_q(frame->pts, 
                                                        is->auddec.avctx->time_base,
                                                        ffp->audio_enc_ctx->time_base);
                        
                        // 编码帧
                        AVPacket enc_pkt;
                        av_init_packet(&enc_pkt);
                        enc_pkt.data = NULL;
                        enc_pkt.size = 0;
                        
                        int got_packet = 0;
                        av_log(ffp, AV_LOG_DEBUG, "Encoding audio frame, pts=%"PRId64, resampled_frame->pts);
                        ret = avcodec_encode_audio2(ffp->audio_enc_ctx, &enc_pkt, resampled_frame, &got_packet);
                        
                        av_frame_free(&frame);
                        av_frame_free(&resampled_frame);
                        
                        if (ret < 0) {
                            av_log(ffp, AV_LOG_ERROR, "Error encoding audio frame: %s", av_err2str(ret));
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return ret;
                        }
                        
                        if (got_packet) {
                            av_log(ffp, AV_LOG_DEBUG, "Got encoded audio packet, size=%d, pts=%"PRId64, 
                                enc_pkt.size, enc_pkt.pts);
                            
                            // 设置流索引
                            enc_pkt.stream_index = ffp->out_audio_stream_index;
                            
                            // 转换时间戳
                            av_log(ffp, AV_LOG_DEBUG, "Rescaling audio timestamps, before: pts=%"PRId64", dts=%"PRId64,
                                enc_pkt.pts, enc_pkt.dts);
                                
                            enc_pkt.pts = av_rescale_q_rnd(enc_pkt.pts,
                                                        ffp->audio_enc_ctx->time_base,
                                                        ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->time_base,
                                                        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                            enc_pkt.dts = av_rescale_q_rnd(enc_pkt.dts,
                                                        ffp->audio_enc_ctx->time_base,
                                                        ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->time_base,
                                                        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
                            enc_pkt.duration = av_rescale_q(enc_pkt.duration,
                                                        ffp->audio_enc_ctx->time_base,
                                                        ffp->m_ofmt_ctx->streams[ffp->out_audio_stream_index]->time_base);
                            
                            av_log(ffp, AV_LOG_DEBUG, "Rescaled audio timestamps, after: pts=%"PRId64", dts=%"PRId64,
                                enc_pkt.pts, enc_pkt.dts);
                            
                            // 确保音频DTS严格递增
                            if (ffp->last_audio_dts != -1 && enc_pkt.dts <= ffp->last_audio_dts) {
                                av_log(ffp, AV_LOG_WARNING, "Non-monotonic audio DTS detected: %"PRId64" <= %"PRId64", fixing...", 
                                       enc_pkt.dts, ffp->last_audio_dts);
                                enc_pkt.dts = ffp->last_audio_dts + 1;
                                // 确保PTS不小于DTS
                                if (enc_pkt.pts < enc_pkt.dts) {
                                    enc_pkt.pts = enc_pkt.dts;
                                }
                            }
                            
                            // 更新最后一个音频DTS
                            ffp->last_audio_dts = enc_pkt.dts;
                            
                            // 写入文件
                            av_log(ffp, AV_LOG_DEBUG, "Writing audio packet to file, size=%d, dts=%"PRId64, enc_pkt.size, enc_pkt.dts);
                            ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, &enc_pkt);
                            if (ret < 0) {
                                av_log(ffp, AV_LOG_ERROR, "Error writing audio frame: %s", av_err2str(ret));
                            } else {
                                av_log(ffp, AV_LOG_DEBUG, "Successfully wrote audio packet");
                            }
                            
                            av_packet_unref(&enc_pkt);
                        } else {
                            av_log(ffp, AV_LOG_DEBUG, "No packet produced from audio encoding");
                        }
                    } else {
                        av_log(ffp, AV_LOG_DEBUG, "No frame decoded from audio packet");
                        av_frame_free(&frame);
                    }
                } else {
                    av_log(ffp, AV_LOG_DEBUG, "Skipping packet from stream %d (not video or audio)", packet->stream_index);
                }
            } else {
                // 原有的不需要转码的逻辑
                av_log(ffp, AV_LOG_DEBUG, "Processing packet without transcoding, stream_index=%d, size=%d", 
                    packet->stream_index, packet->size);
                // 与看直播的 AVPacket分开，不然卡屏
                AVPacket *pkt = av_packet_alloc();
                if (!pkt) {
                    av_log(ffp, AV_LOG_ERROR, "Failed to allocate packet");
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return AVERROR(ENOMEM);
                }
                
                if (av_packet_ref(pkt, packet) < 0) {
                    av_log(ffp, AV_LOG_ERROR, "Failed to reference packet");
                    av_packet_free(&pkt);
                    pthread_mutex_unlock(&ffp->record_mutex);
                    return AVERROR(ENOMEM);
                }
                
                in_stream  = is->ic->streams[pkt->stream_index];
                out_stream = ffp->m_ofmt_ctx->streams[pkt->stream_index];

                // 检查是否是视频流
                int is_video = (pkt->stream_index == is->video_stream);
                
                // 检查是否是HEVC视频
                int is_hevc = (is_video && is->viddec.avctx && is->viddec.avctx->codec_id == AV_CODEC_ID_HEVC);
                    
                // 检查是否有音频流
                int has_audio = (is->audio_stream >= 0);
                av_log(ffp, AV_LOG_DEBUG, "Original packet: pts=%"PRId64", dts=%"PRId64", is_video=%d, is_hevc=%d, has_audio=%d",
                        pkt->pts, pkt->dts, is_video, is_hevc, has_audio);
                    
                if (!ffp->is_first) { 
                        // 录制的第一帧，时间从0开始
                        // 确保第一帧是关键帧，无论是什么类型的视频
                        if (is_video && !(pkt->flags & AV_PKT_FLAG_KEY)) {
                            av_log(ffp, AV_LOG_WARNING, "First video frame is not a keyframe, waiting for keyframe");
                            av_packet_unref(pkt);
                            av_packet_free(&pkt);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return 0;
                        }
                        
                        // 如果是音频包，但还没有收到视频关键帧，也等待
                        if (!is_video && has_audio && is->video_stream >= 0) {
                            av_log(ffp, AV_LOG_DEBUG, "Received audio packet but waiting for video keyframe first");
                            av_packet_unref(pkt);
                            av_packet_free(&pkt);
                            pthread_mutex_unlock(&ffp->record_mutex);
                            return 0;
                        }
                        
                        ffp->is_first = 1;
                        
                        // 使用当前帧的PTS/DTS作为起始时间，而不是使用可能有大偏移的值
                        // 这样可以避免前几秒没有画面的问题
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            ffp->start_pts = pkt->pts;
                        } else {
                            ffp->start_pts = 0;
                        }
                        
                        if (pkt->dts != AV_NOPTS_VALUE) {
                            ffp->start_dts = pkt->dts;
                        } else {
                            ffp->start_dts = ffp->start_pts;
                        }
                        
                        ffp->record_real_time = av_gettime_relative() / 1000; // 记录实际开始时间（毫秒）
                        
                        // 第一帧从0开始
                        pkt->pts = 0;
                        pkt->dts = 0;
                        ffp->last_video_dts = 0; // 初始化最后一个DTS
                        ffp->last_audio_dts = 0;
                        av_log(ffp, AV_LOG_INFO, "First packet, start_pts=%"PRId64", start_dts=%"PRId64", is_hevc=%d, flags=%d, keyframe=%d",
                            ffp->start_pts, ffp->start_dts, is_hevc, pkt->flags, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                } else {
                    // 使用原始时间戳差值而不是实际时间
                    if (is_video) {
                        // 视频流
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t pts_diff = pkt->pts - ffp->start_pts;
                            
                            // 确保时间差不是负数，这会导致前几秒没有画面
                            if (pts_diff < 0) {
                                pts_diff = 0;
                                av_log(ffp, AV_LOG_WARNING, "Negative standard PTS diff detected, correcting to 0");
                            }
                            
                            // 转换到输出时基
                            pkt->pts = av_rescale_q(pts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保PTS始终递增
                            if (pkt->pts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                pkt->pts = ffp->last_video_dts + 1;
                            }
                            
                            av_log(ffp, AV_LOG_DEBUG, "Video packet: pts_diff=%"PRId64", new_pts=%"PRId64", keyframe=%d", 
                                pts_diff, pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
                        }
                        
                        // 处理DTS
                        if (pkt->dts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t dts_diff = pkt->dts - ffp->start_dts;
                            
                            // 确保时间差不是负数，这会导致前几秒没有画面
                            if (dts_diff < 0) {
                                dts_diff = 0;
                                av_log(ffp, AV_LOG_WARNING, "Negative DTS diff detected, correcting to 0");
                            }
                            
                            // 转换到输出时基
                            pkt->dts = av_rescale_q(dts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保DTS始终递增
                            if (pkt->dts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                pkt->dts = ffp->last_video_dts + 1;
                            }
                            
                            ffp->last_video_dts = pkt->dts;
                            
                            // 确保PTS不小于DTS
                            if (pkt->pts < pkt->dts) {
                                pkt->pts = pkt->dts;
                            }
                        } else {
                            // 如果没有DTS，使用PTS
                            if (pkt->pts != AV_NOPTS_VALUE) {
                                pkt->dts = pkt->pts;
                                
                                // 确保DTS始终递增
                                if (pkt->dts <= ffp->last_video_dts && ffp->last_video_dts > 0) {
                                    pkt->dts = ffp->last_video_dts + 1;
                                    pkt->pts = pkt->dts; // 保持PTS和DTS同步
                                }
                                
                                ffp->last_video_dts = pkt->dts;
                            }
                        }
                    } else if (pkt->stream_index == is->audio_stream) {
                        // 音频流
                        if (pkt->pts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t pts_diff = pkt->pts - ffp->start_pts;
                            
                            // 确保时间差不是负数，这会导致前几秒没有声音
                            if (pts_diff < 0) {
                                pts_diff = 0;
                                av_log(ffp, AV_LOG_WARNING, "Negative audio PTS diff detected, correcting to 0");
                            }
                            
                            // 转换到输出时基
                            pkt->pts = av_rescale_q(pts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保PTS始终递增
                            if (pkt->pts <= ffp->last_audio_dts && ffp->last_audio_dts > 0) {
                                pkt->pts = ffp->last_audio_dts + 1;
                            }
                            
                            av_log(ffp, AV_LOG_DEBUG, "Audio packet: pts_diff=%"PRId64", new_pts=%"PRId64, 
                                pts_diff, pkt->pts);
                        }
                        
                        // 处理DTS
                        if (pkt->dts != AV_NOPTS_VALUE) {
                            // 计算相对于第一帧的时间差
                            int64_t dts_diff = pkt->dts - ffp->start_dts;
                            
                            // 确保时间差不是负数，这会导致前几秒没有声音
                            if (dts_diff < 0) {
                                dts_diff = 0;
                                av_log(ffp, AV_LOG_WARNING, "Negative audio DTS diff detected, correcting to 0");
                            }
                            
                            // 转换到输出时基
                            pkt->dts = av_rescale_q(dts_diff, in_stream->time_base, out_stream->time_base);
                            
                            // 确保DTS始终递增
                            if (pkt->dts <= ffp->last_audio_dts && ffp->last_audio_dts > 0) {
                                pkt->dts = ffp->last_audio_dts + 1;
                            }
                            
                            ffp->last_audio_dts = pkt->dts;
                            
                            // 确保PTS不小于DTS
                            if (pkt->pts < pkt->dts) {
                                pkt->pts = pkt->dts;
                            }
                        } else {
                            // 如果没有DTS，使用PTS
                            if (pkt->pts != AV_NOPTS_VALUE) {
                                pkt->dts = pkt->pts;
                                
                                // 确保DTS始终递增
                                if (pkt->dts <= ffp->last_audio_dts && ffp->last_audio_dts > 0) {
                                    pkt->dts = ffp->last_audio_dts + 1;
                                    pkt->pts = pkt->dts; // 保持PTS和DTS同步
                                }
                                
                                ffp->last_audio_dts = pkt->dts;
                            }
                        }
                    } else {
                        // 其他类型的流（如字幕等）
                        pkt->pts = abs(pkt->pts - ffp->start_pts);
                        pkt->dts = abs(pkt->dts - ffp->start_dts);
                    }
                }
                // 转换持续时间
                if (pkt->duration > 0) {
                    pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
                }
                
                pkt->pos = -1;
                av_log(ffp, AV_LOG_DEBUG, "Final packet: pts=%"PRId64", dts=%"PRId64", duration=%"PRId64", is_video=%d, has_audio=%d", 
                        pkt->pts, pkt->dts, pkt->duration, is_video, has_audio);
                // 写入一个AVPacket到输出文件
                if (is_hevc) {
                    av_log(ffp, AV_LOG_INFO, "Writing HEVC video packet to file, size=%d, pts=%"PRId64", dts=%"PRId64", keyframe=%d, flags=%d", 
                           pkt->size, pkt->pts, pkt->dts, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0, pkt->flags);
                } else {
                    av_log(ffp, AV_LOG_DEBUG, "Writing packet to file, size=%d, stream_index=%d", 
                           pkt->size, pkt->stream_index);
                }
                
                if ((ret = av_interleaved_write_frame(ffp->m_ofmt_ctx, pkt)) < 0) {
                    av_log(ffp, AV_LOG_ERROR, "Error muxing packet: %s", av_err2str(ret));
                } else {
                    if (is_hevc) {
                        av_log(ffp, AV_LOG_INFO, "Successfully wrote HEVC video packet of size %d", pkt->size);
                    } else {
                        av_log(ffp, AV_LOG_DEBUG, "Successfully wrote packet");
                    }
                }
                av_packet_unref(pkt);
                av_packet_free(&pkt);
                
            }
            pthread_mutex_unlock(&ffp->record_mutex);
        }
    }
    return ret;
}

// 初始化转码器
int init_transcoder(FFPlayer *ffp, AVCodecContext *dec_ctx, AVStream *out_stream, enum AVMediaType type) {
    int ret = 0;
    AVCodecContext *enc_ctx = NULL;
    AVCodec *encoder = NULL;
    AVDictionary *opts = NULL;
    
    if (type == AVMEDIA_TYPE_VIDEO) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!encoder) {
            av_log(ffp, AV_LOG_ERROR, "Cannot find H.264 video encoder");
            return AVERROR_ENCODER_NOT_FOUND;
        }
        
        av_log(ffp, AV_LOG_DEBUG, "Using video encoder: %s", encoder->name);
        
        enc_ctx = avcodec_alloc_context3(encoder);
        if (!enc_ctx) {
            av_log(ffp, AV_LOG_ERROR, "Cannot allocate video encoder context");
            return AVERROR(ENOMEM);
        }
        
        // 设置视频编码参数
        enc_ctx->height = dec_ctx->height;
        enc_ctx->width = dec_ctx->width;
        enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
        enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;  // H.264通常使用YUV420P
        
        // 设置时间基准和帧率
        if (dec_ctx->framerate.num > 0 && dec_ctx->framerate.den > 0) {
            enc_ctx->framerate = dec_ctx->framerate;
            enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
        } else {
            enc_ctx->framerate = (AVRational){25, 1};  // 默认25fps
            enc_ctx->time_base = (AVRational){1, 25};
        }
        
        av_log(ffp, AV_LOG_DEBUG, "Video framerate set to %d/%d", 
               enc_ctx->framerate.num, enc_ctx->framerate.den);
        
        // 设置编码器选项
        enc_ctx->bit_rate = 2000000;  // 2Mbps
        enc_ctx->rc_min_rate = enc_ctx->bit_rate;
        enc_ctx->rc_max_rate = enc_ctx->bit_rate;
        enc_ctx->rc_buffer_size = enc_ctx->bit_rate;
        
        // 设置GOP（关键帧间隔）
        enc_ctx->gop_size = (int)(enc_ctx->framerate.num / enc_ctx->framerate.den) * 2;  // 约2秒一个关键帧
        enc_ctx->keyint_min = enc_ctx->gop_size / 2;
        enc_ctx->max_b_frames = 0;  // 不使用B帧，减少延迟
        
        // 设置编码质量
        enc_ctx->qmin = 10;
        enc_ctx->qmax = 51;
        
        // 设置全局头部标志（MP4需要）
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        
        // 设置线程数
        enc_ctx->thread_count = 4;
        
        // 检查输入是否是HEVC
        int is_hevc_input = (dec_ctx->codec_id == AV_CODEC_ID_HEVC);
        
        // 设置编码器特定选项
        av_dict_set(&opts, "preset", is_hevc_input ? "ultrafast" : "superfast", 0);  // 超快速预设，HEVC使用更快的设置
        av_dict_set(&opts, "tune", "zerolatency", 0);  // 零延迟调优
        av_dict_set(&opts, "profile", "baseline", 0);  // 基准配置文件，兼容性最好
        
        // 对于HEVC输入或只有视频的流，使用更保守的设置
        if (is_hevc_input || ffp->video_only) {
            av_log(ffp, AV_LOG_INFO, "Using optimized settings for %s stream", 
                   is_hevc_input ? "HEVC" : "video-only");
            
            // 降低比特率，避免过大文件
            enc_ctx->bit_rate = 1500000;  // 1.5Mbps
            
            // 设置更小的GOP，增加关键帧频率
            enc_ctx->gop_size = (int)(enc_ctx->framerate.num / enc_ctx->framerate.den);  // 约1秒一个关键帧
            
            // 设置更低的编码复杂度
            av_dict_set(&opts, "preset", "ultrafast", 0);  // 最快速预设
            
            // 设置CBR模式
            av_dict_set(&opts, "crf", "23", 0);  // 固定质量因子
            av_dict_set(&opts, "forced-idr", "1", 0);  // 强制IDR帧
            
            // 对于HEVC输入，添加额外设置以确保兼容性
            if (is_hevc_input) {
                av_log(ffp, AV_LOG_INFO, "Adding HEVC-specific optimizations");
                
                // 确保使用一致的参考帧设置
                enc_ctx->refs = 1;
                enc_ctx->slices = 1;
                
                // 设置关键帧的最小间隔更小以确保更多关键帧
                enc_ctx->keyint_min = enc_ctx->gop_size / 2;
                
                // 使用更简单的编码配置
                av_dict_set(&opts, "profile", "main", 0);  // 主要配置文件，兼容性好一些
                av_dict_set(&opts, "level", "4.0", 0);  // 限制级别以提高兼容性
            }
        }
        
        ffp->video_enc_ctx = enc_ctx;
    } else if (type == AVMEDIA_TYPE_AUDIO) {
        // 检查输入音频编解码器类型，为PCM格式提供更好的转码支持
        int is_pcm = (dec_ctx->codec_id == AV_CODEC_ID_PCM_MULAW || 
                      dec_ctx->codec_id == AV_CODEC_ID_PCM_ALAW ||
                      dec_ctx->codec_id == AV_CODEC_ID_PCM_S16LE ||
                      dec_ctx->codec_id == AV_CODEC_ID_PCM_S16BE ||
                      dec_ctx->codec_id == AV_CODEC_ID_PCM_U8);
        
        // 对于MP4容器，必须转换为AAC，因为MP4不支持许多音频格式（如PCM mu-law）
        encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            av_log(ffp, AV_LOG_ERROR, "Cannot find AAC audio encoder");
            return AVERROR_ENCODER_NOT_FOUND;
        }
        
        av_log(ffp, AV_LOG_INFO, "Converting audio to AAC for MP4 compatibility (original codec_id: %d, is_pcm: %d)", 
               dec_ctx->codec_id, is_pcm);
        
        enc_ctx = avcodec_alloc_context3(encoder);
        if (!enc_ctx) {
            av_log(ffp, AV_LOG_ERROR, "Cannot allocate audio encoder context");
            return AVERROR(ENOMEM);
        }
        
        // 设置音频编码参数
        enc_ctx->sample_rate = dec_ctx->sample_rate;
        
        // PCM音频格式可能需要特殊处理
        if (is_pcm) {
            // 记录原始PCM格式信息，帮助调试
            av_log(ffp, AV_LOG_INFO, "PCM audio: codec=%s, channels=%d, sample_rate=%d, sample_fmt=%d",
                   avcodec_get_name(dec_ctx->codec_id),
                   dec_ctx->channels,
                   dec_ctx->sample_rate,
                   dec_ctx->sample_fmt);
        }
        
        // 确保通道布局有效
        enc_ctx->channel_layout = dec_ctx->channel_layout;
        if (!enc_ctx->channel_layout) {
            enc_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
        }
        enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
        
        // 确保使用AAC编码器支持的采样格式
        enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  // AAC通常使用浮点平面格式
        
        // 检查编码器是否支持这种采样格式
        int i;
        for (i = 0; encoder->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
            if (encoder->sample_fmts[i] == enc_ctx->sample_fmt) {
                break;
            }
        }
        //如果遍历结束也没有支持的编码格式，采用第一个. 
        if (encoder->sample_fmts[i] == AV_SAMPLE_FMT_NONE) {
            // 如果不支持，使用第一个支持的格式
            enc_ctx->sample_fmt = encoder->sample_fmts[0];
            av_log(ffp, AV_LOG_WARNING, "Audio encoder doesn't support format FLTP, using %d instead", 
                   enc_ctx->sample_fmt);
        }
        
        enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
        enc_ctx->bit_rate = 128000;  // 128kbps
        
        // 设置编码器特定选项
        av_dict_set(&opts, "strict", "experimental", 0);  // 允许实验性AAC编码器
        
        // 为PCM音频特别设置
        if (dec_ctx->codec_id == AV_CODEC_ID_PCM_MULAW || 
            dec_ctx->codec_id == AV_CODEC_ID_PCM_ALAW ||
            dec_ctx->codec_id == AV_CODEC_ID_PCM_S16LE ||
            dec_ctx->codec_id == AV_CODEC_ID_PCM_S16BE ||
            dec_ctx->codec_id == AV_CODEC_ID_PCM_U8) {
            av_log(ffp, AV_LOG_INFO, "Setting special AAC encoding options for PCM audio");
            av_dict_set(&opts, "profile", "aac_low", 0);  // 使用AAC-LC配置文件
            av_dict_set(&opts, "b", "128k", 0);           // 设置比特率
        }
        
        ffp->audio_enc_ctx = enc_ctx;
     } else {
        return AVERROR(EINVAL);
    }
    
    // 打开编码器
    ret = avcodec_open2(enc_ctx, encoder, &opts);
    if (ret < 0) {
        av_log(ffp, AV_LOG_ERROR, "Cannot open %s encoder: %s", 
               type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", av_err2str(ret));
        av_dict_free(&opts);
    return ret;
    }
    
    // 检查未使用的选项
    if (av_dict_count(opts) > 0) {
        av_log(ffp, AV_LOG_WARNING, "Some encoder options were not used:");
        AVDictionaryEntry *t = NULL;
        while ((t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX))) {
            av_log(ffp, AV_LOG_WARNING, " %s=%s", t->key, t->value);
        }
    }
    
    av_dict_free(&opts);
    
    // 将编码器参数复制到输出流
    ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
    if (ret < 0) {
        av_log(ffp, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream: %s", 
               av_err2str(ret));
        return ret;
    }
    
    // 对于HEVC视频，强制使用hvc1标签，否则让容器自己选择合适的标签
    if (dec_ctx->codec_id == AV_CODEC_ID_HEVC) {
        uint32_t hvc1_tag = MKTAG('h','v','c','1');
        out_stream->codecpar->codec_tag = hvc1_tag;
        av_log(ffp, AV_LOG_INFO, "Forcing HEVC codec tag to 'hvc1' in transcoder initialization");
    } else {
        out_stream->codecpar->codec_tag = 0;
    }
    
    // 对于音频，确保MP4容器中始终使用AAC
    if (type == AVMEDIA_TYPE_AUDIO && ffp->m_ofmt && strcmp(ffp->m_ofmt->name, "mp4") == 0) {
        out_stream->codecpar->codec_id = AV_CODEC_ID_AAC;
        av_log(ffp, AV_LOG_INFO, "Forcing AAC codec for audio stream in MP4 container");
    }
    
    out_stream->time_base = enc_ctx->time_base;
    
    // 确保MP4的音频流使用正确的编解码器标签
    if (type == AVMEDIA_TYPE_AUDIO && ffp->m_ofmt && strcmp(ffp->m_ofmt->name, "mp4") == 0) {
        // 强制设置AAC编解码器ID，确保正确使用AAC标签
        out_stream->codecpar->codec_id = AV_CODEC_ID_AAC;
        av_log(ffp, AV_LOG_INFO, "Forced AAC codec ID for MP4 container compatibility");
    }
    
    av_log(ffp, AV_LOG_INFO, "Transcoder initialized for %s: %dx%d, %d channels, %d Hz, codec=%d", 
           type == AVMEDIA_TYPE_VIDEO ? "video" : "audio",
           type == AVMEDIA_TYPE_VIDEO ? enc_ctx->width : 0,
           type == AVMEDIA_TYPE_VIDEO ? enc_ctx->height : 0,
           type == AVMEDIA_TYPE_AUDIO ? enc_ctx->channels : 0,
           type == AVMEDIA_TYPE_AUDIO ? enc_ctx->sample_rate : 0,
           out_stream->codecpar->codec_id);
    
    return 0;
}

// 开始录制并转码
int ffp_start_record_transcode(FFPlayer *ffp, const char *file_name) {
    assert(ffp);
    
    VideoState *is = ffp->is;
    int ret = 0, i;
    
    av_log(ffp, AV_LOG_INFO, "Starting transcode recording to file: %s", file_name);
    
    ffp->m_ofmt_ctx = NULL;
    ffp->m_ofmt = NULL;
    ffp->is_record = 0;
    ffp->record_error = 0;
    ffp->need_transcode = 1;
    ffp->video_enc_ctx = NULL;
    ffp->audio_enc_ctx = NULL;
    ffp->sws_ctx = NULL;
    ffp->swr_ctx_record = NULL;
    ffp->record_audio_fifo = NULL;
    ffp->record_next_audio_pts = 0;
    ffp->tmp_frame = NULL;
    ffp->out_video_stream_index = -1;
    ffp->out_audio_stream_index = -1;
    ffp->is_first = 0;
    ffp->min_record_time = 5000; // Minimum recording time of 5 seconds
    ffp->record_start_time = av_gettime() / 1000; // Current time in milliseconds
    ffp->direct_hevc_write = 1; // Enable direct HEVC packet writing by default
    
    if (!file_name || !strlen(file_name)) {
        av_log(ffp, AV_LOG_ERROR, "Invalid filename provided");
        goto end;
    }
    
    if (!is || !is->ic || is->paused || is->abort_request) {
        av_log(ffp, AV_LOG_ERROR, "Invalid player state: is=%p, ic=%p, paused=%d, abort=%d", 
               is, is ? is->ic : NULL, is ? is->paused : -1, is ? is->abort_request : -1);
        goto end;
    }
    
    if (ffp->is_record) {
        av_log(ffp, AV_LOG_ERROR, "Recording has already started");
        goto end;
    }
    
    // 初始化一个用于输出的AVFormatContext结构体
    av_log(ffp, AV_LOG_DEBUG, "Allocating output context for MP4 format");
    ret = avformat_alloc_output_context2(&ffp->m_ofmt_ctx, NULL, "mp4", file_name);
    if (!ffp->m_ofmt_ctx) {
        av_log(ffp, AV_LOG_ERROR, "Could not create output context for file '%s': %s", 
               file_name, av_err2str(ret));
        goto end;
    }
    ffp->m_ofmt = ffp->m_ofmt_ctx->oformat;
    av_log(ffp, AV_LOG_DEBUG, "Output format: %s", ffp->m_ofmt->name);
    
    // 初始化时间戳追踪变量
    ffp->last_video_dts = -1;
    ffp->last_audio_dts = -1;
    
    // 创建输出流
    av_log(ffp, AV_LOG_DEBUG, "Creating output streams, input has %d streams", is->ic->nb_streams);
    for (i = 0; i < is->ic->nb_streams; i++) {
        AVStream *in_stream = is->ic->streams[i];
        AVCodecContext *dec_ctx = NULL;
        
        if (i == is->video_stream) {
            av_log(ffp, AV_LOG_DEBUG, "Processing video stream (index %d)", i);
            dec_ctx = is->viddec.avctx;
            if (!dec_ctx) {
                av_log(ffp, AV_LOG_ERROR, "No video decoder context available");
                continue;
            }
            
            av_log(ffp, AV_LOG_DEBUG, "Video decoder: %dx%d, format=%d, codec=%d", 
                   dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, dec_ctx->codec_id);
            
            AVStream *out_stream = avformat_new_stream(ffp->m_ofmt_ctx, NULL);
            if (!out_stream) {
                av_log(ffp, AV_LOG_ERROR, "Failed to allocate output video stream");
                goto end;
            }
            
            // 直接复制视频流，不进行转码
            av_log(ffp, AV_LOG_INFO, "Using video stream copy (no transcoding)");
            
            // 复制编解码器参数
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(ffp, AV_LOG_ERROR, "Failed to copy video codec parameters: %s", av_err2str(ret));
                goto end;
            }
            
            // 设置直接视频复制标志
            ffp->direct_video_copy = 1;
            av_log(ffp, AV_LOG_INFO, "Direct video copy mode enabled");
            
            // 确保正确设置codec结构体（为了兼容性）
            if (out_stream->codec && in_stream->codec) {
                ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
                if (ret < 0) {
                    av_log(ffp, AV_LOG_WARNING, "Failed to copy video codec context: %s", av_err2str(ret));
                    // 继续执行，因为codecpar已经设置好了
                }
            }
            
            // 设置时间基准
            out_stream->time_base = in_stream->time_base;
            av_log(ffp, AV_LOG_INFO, "Video stream time base: %d/%d", 
                   out_stream->time_base.num, out_stream->time_base.den);
            
            // 确保设置正确的编解码器标记
            // 对于HEVC视频，检查输入流的codec_tag并保持一致
            if (out_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                uint32_t hevc_tag;
                
                // 检查输入流的codec_tag
                if (in_stream->codecpar->codec_tag != 0) {
                    // 使用输入流的codec_tag
                    hevc_tag = in_stream->codecpar->codec_tag;
                    char tag_str[5] = {0};
                    tag_str[0] = (hevc_tag >> 0) & 0xFF;
                    tag_str[1] = (hevc_tag >> 8) & 0xFF;
                    tag_str[2] = (hevc_tag >> 16) & 0xFF;
                    tag_str[3] = (hevc_tag >> 24) & 0xFF;
                    av_log(ffp, AV_LOG_INFO, "Preserving original HEVC codec tag: '%s' (0x%08x)", tag_str, hevc_tag);
                } else {
                    // 默认使用hvc1标签
                    hevc_tag = MKTAG('h','v','c','1');
                    av_log(ffp, AV_LOG_INFO, "Input has no codec tag, forcing HEVC codec tag to 'hvc1'");
                }
                
                out_stream->codecpar->codec_tag = hevc_tag;
                if (out_stream->codec) {
                    out_stream->codec->codec_tag = hevc_tag;
                }
            } else {
                out_stream->codecpar->codec_tag = 0;
                if (out_stream->codec) {
                    out_stream->codec->codec_tag = 0;
                }
            }
            
            ffp->out_video_stream_index = out_stream->index;
            av_log(ffp, AV_LOG_DEBUG, "Video output stream index: %d (copy mode)", ffp->out_video_stream_index);
            
            // 标记为不需要视频转码
            ffp->direct_video_copy = 1;
            av_log(ffp, AV_LOG_INFO, "Direct video copy mode enabled for transcoding");
            
        } else if (i == is->audio_stream) {
            av_log(ffp, AV_LOG_DEBUG, "Processing audio stream (index %d)", i);
            dec_ctx = is->auddec.avctx;
            if (!dec_ctx) {
                av_log(ffp, AV_LOG_ERROR, "No audio decoder context available");
                continue;
            }
            
            av_log(ffp, AV_LOG_DEBUG, "Audio decoder: channels=%d, rate=%d, format=%d, codec=%d", 
                   dec_ctx->channels, dec_ctx->sample_rate, dec_ctx->sample_fmt, dec_ctx->codec_id);
                   
            // 检查音频编解码器，MP4容器只支持特定的编解码器，例如AAC
            if (strcmp(ffp->m_ofmt->name, "mp4") == 0) {
                av_log(ffp, AV_LOG_INFO, "MP4 container detected, forcing AAC audio codec for compatibility");
                // 无条件强制使用AAC编码器，解决格式兼容性问题
            }
            
            AVStream *out_stream = avformat_new_stream(ffp->m_ofmt_ctx, NULL);
            if (!out_stream) {
                av_log(ffp, AV_LOG_ERROR, "Failed to allocate output audio stream");
                goto end;
            }

            AVCodec *audio_encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
            if (!audio_encoder) {
                av_log(ffp, AV_LOG_ERROR, "AAC encoder not found");
                goto end;
            }

            ffp->audio_enc_ctx = avcodec_alloc_context3(audio_encoder);
            if (!ffp->audio_enc_ctx) {
                av_log(ffp, AV_LOG_ERROR, "Could not allocate audio encoder context");
                goto end;
            }

            ffp->audio_enc_ctx->codec = audio_encoder;
            ffp->audio_enc_ctx->sample_fmt = audio_encoder->sample_fmts ? audio_encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
            ffp->audio_enc_ctx->sample_rate = 48000;
            ffp->audio_enc_ctx->channel_layout = AV_CH_LAYOUT_MONO;
            ffp->audio_enc_ctx->channels = av_get_channel_layout_nb_channels(ffp->audio_enc_ctx->channel_layout);
            ffp->audio_enc_ctx->bit_rate = 64000; // 64 kbps for good quality mono audio
            ffp->audio_enc_ctx->time_base = (AVRational){1, ffp->audio_enc_ctx->sample_rate};
            
            // 全局头，确保MP4容器的兼容性
            if (ffp->m_ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
                ffp->audio_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }

            ret = avcodec_open2(ffp->audio_enc_ctx, audio_encoder, NULL);
            if (ret < 0) {
                av_log(ffp, AV_LOG_ERROR, "Could not open audio encoder: %s", av_err2str(ret));
                goto end;
            }

            ret = avcodec_parameters_from_context(out_stream->codecpar, ffp->audio_enc_ctx);
            if (ret < 0) {
                av_log(ffp, AV_LOG_ERROR, "Could not copy audio encoder parameters to output stream");
                goto end;
            }

            ffp->record_audio_fifo = av_audio_fifo_alloc(ffp->audio_enc_ctx->sample_fmt, ffp->audio_enc_ctx->channels, 1);
            if (!ffp->record_audio_fifo) {
                av_log(ffp, AV_LOG_ERROR, "Could not allocate audio FIFO");
                goto end;
            }

            ffp->out_audio_stream_index = out_stream->index;
            av_log(ffp, AV_LOG_DEBUG, "Audio output stream index: %d", ffp->out_audio_stream_index);
            
            // 创建音频格式转换上下文
            av_log(ffp, AV_LOG_DEBUG, "Creating audio resampler (swresample)");
            ffp->swr_ctx_record = swr_alloc();
            if (!ffp->swr_ctx_record) {
                av_log(ffp, AV_LOG_ERROR, "Could not allocate audio resampler context");
                goto end;
            }
            
            av_log(ffp, AV_LOG_DEBUG, "Configuring audio resampler: in=%d ch, %d Hz, %d fmt -> out=%d ch, %d Hz, %d fmt",
                   dec_ctx->channels, dec_ctx->sample_rate, dec_ctx->sample_fmt,
                   ffp->audio_enc_ctx->channels, ffp->audio_enc_ctx->sample_rate, ffp->audio_enc_ctx->sample_fmt);
                   
            av_opt_set_int(ffp->swr_ctx_record, "in_channel_count", dec_ctx->channels, 0);
            av_opt_set_int(ffp->swr_ctx_record, "in_sample_rate", dec_ctx->sample_rate, 0);
            av_opt_set_sample_fmt(ffp->swr_ctx_record, "in_sample_fmt", dec_ctx->sample_fmt, 0);
            
            av_opt_set_int(ffp->swr_ctx_record, "out_channel_count", ffp->audio_enc_ctx->channels, 0);
            av_opt_set_int(ffp->swr_ctx_record, "out_sample_rate", ffp->audio_enc_ctx->sample_rate, 0);
            av_opt_set_sample_fmt(ffp->swr_ctx_record, "out_sample_fmt", ffp->audio_enc_ctx->sample_fmt, 0);

            if (dec_ctx->channel_layout == 0) {
                dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
            }
             av_opt_set_int(ffp->swr_ctx_record, "in_channel_layout", dec_ctx->channel_layout, 0);
             av_opt_set_int(ffp->swr_ctx_record, "out_channel_layout", ffp->audio_enc_ctx->channel_layout, 0);
            
            ret = swr_init(ffp->swr_ctx_record);
            if (ret < 0) {
                av_log(ffp, AV_LOG_ERROR, "Failed to initialize the audio resampling context: %s", av_err2str(ret));
                goto end;
            }
            
            av_log(ffp, AV_LOG_INFO, "Audio resampling initialized: %d Hz, %d ch, fmt %d → %d Hz, %d ch, fmt %d",
                   dec_ctx->sample_rate, dec_ctx->channels, dec_ctx->sample_fmt,
                   ffp->audio_enc_ctx->sample_rate, ffp->audio_enc_ctx->channels, ffp->audio_enc_ctx->sample_fmt);
        }
    }
    
    // 检查是否有视频或音频流
    if (ffp->out_video_stream_index < 0 && ffp->out_audio_stream_index < 0) {
        av_log(ffp, AV_LOG_ERROR, "No video or audio streams available for recording");
        goto end;
    }
    
    // 记录是否只有视频流
    ffp->video_only = (ffp->out_video_stream_index >= 0 && ffp->out_audio_stream_index < 0);
    if (ffp->video_only) {
        av_log(ffp, AV_LOG_INFO, "Video-only stream detected (no audio), configuring for video-only recording");
    }
    
    // 检查是否是HEVC视频流
    int has_hevc = 0;
    if (is->viddec.avctx && is->viddec.avctx->codec_id == AV_CODEC_ID_HEVC) {
        has_hevc = 1;
        av_log(ffp, AV_LOG_INFO, "HEVC video stream detected, enabling special handling");
        
        // 为HEVC启用直接写入模式
        ffp->direct_hevc_write = 1;
        
        // 如果是RTSP流，增加额外的安全措施
        if (is->ic && is->ic->iformat && is->ic->iformat->name && 
            (strstr(is->ic->iformat->name, "rtsp") || strstr(is->ic->url, "rtsp"))) {
            av_log(ffp, AV_LOG_INFO, "RTSP+HEVC combination detected, using optimized settings");
            
            // 确保MP4元数据正确
            av_dict_set(&ffp->m_ofmt_ctx->metadata, "encoder", "IJKPlayer HEVC Direct", 0);
            
            // 为HEVC流设置正确的时间基准
            if (ffp->out_video_stream_index >= 0) {
                AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                out_stream->time_base = (AVRational){1, 90000}; // 标准HEVC时间基准
            }
        }
    }
    
    // 如果需要视频转码，才分配临时帧
    if (ffp->video_enc_ctx && !ffp->direct_video_copy) {
        av_log(ffp, AV_LOG_DEBUG, "Allocating temporary frame for video conversion");
        ffp->tmp_frame = av_frame_alloc();
        if (!ffp->tmp_frame) {
            av_log(ffp, AV_LOG_ERROR, "Failed to allocate temporary frame");
            goto end;
        }
    } else {
        // 直接复制模式不需要临时帧
        ffp->tmp_frame = NULL;
        av_log(ffp, AV_LOG_DEBUG, "Direct video copy mode, no temporary frame needed");
    }
    
    // 如果有视频编码器，设置关键参数
    if (ffp->video_enc_ctx) {
        // 设置关键帧间隔
        ffp->video_enc_ctx->gop_size = 10;
        // 设置B帧数量为0，提高实时性
        ffp->video_enc_ctx->max_b_frames = 0;
        // 设置为恒定比特率模式
        ffp->video_enc_ctx->bit_rate = 2000000; // 2 Mbps
        // 强制关键帧间隔
        ffp->video_enc_ctx->keyint_min = 10;
        // 设置时间基准
        ffp->video_enc_ctx->time_base = (AVRational){1, 25};
        
        av_log(ffp, AV_LOG_DEBUG, "Video encoder configured: %dx%d, bitrate=%d, gop=%d", 
               ffp->video_enc_ctx->width, ffp->video_enc_ctx->height,
               (int)ffp->video_enc_ctx->bit_rate, ffp->video_enc_ctx->gop_size);
    }
    
    // 如果有音频编码器，设置关键参数
    if (ffp->audio_enc_ctx) {
        // 设置音频比特率
        ffp->audio_enc_ctx->bit_rate = 128000; // 128 kbps
        av_log(ffp, AV_LOG_DEBUG, "Audio encoder configured: channels=%d, sample_rate=%d, bitrate=%d", 
               ffp->audio_enc_ctx->channels, ffp->audio_enc_ctx->sample_rate,
               (int)ffp->audio_enc_ctx->bit_rate);
    }
    
    av_dump_format(ffp->m_ofmt_ctx, 0, file_name, 1);
    
    // 打开输出文件
    if (!(ffp->m_ofmt->flags & AVFMT_NOFILE)) {
        av_log(ffp, AV_LOG_DEBUG, "Opening output file: %s", file_name);
        ret = avio_open(&ffp->m_ofmt_ctx->pb, file_name, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(ffp, AV_LOG_ERROR, "Could not open output file '%s': %s", file_name, av_err2str(ret));
            goto end;
        }
    }
    
    // 写视频文件头
    av_log(ffp, AV_LOG_DEBUG, "Writing file header");
    
    // MP4容器的特殊处理 - 确保所有音频流使用正确编解码器标签
    if (strcmp(ffp->m_ofmt->name, "mp4") == 0) {
        for (i = 0; i < ffp->m_ofmt_ctx->nb_streams; i++) {
            AVStream *stream = ffp->m_ofmt_ctx->streams[i];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                // 强制设置为AAC，不管原始格式是什么
                av_log(ffp, AV_LOG_WARNING, "MP4 container: ensuring audio stream uses AAC codec (current id=%d)",
                      stream->codecpar->codec_id);
                
                // 设置codecpar
                stream->codecpar->codec_id = AV_CODEC_ID_AAC;
                stream->codecpar->codec_tag = 0; // 让容器选择合适的tag
                
                // 同时设置codec（为了兼容性）
                if (stream->codec) {
                    stream->codec->codec_id = AV_CODEC_ID_AAC;
                    stream->codec->codec_tag = 0;
                }
            }
        }
    }
    
    ret = avformat_write_header(ffp->m_ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(ffp, AV_LOG_ERROR, "Error writing file header: %s", av_err2str(ret));
        goto end;
    }
    
    av_log(ffp, AV_LOG_INFO, "Transcode recording started successfully");
    ffp->is_record = 1;
    ffp->record_error = 0;
    
    // 设置最小录制时间（毫秒），防止录制过早停止
    ffp->min_record_time = 5000;  // 至少录制5秒
    ffp->record_start_time = av_gettime() / 1000;  // 当前时间（毫秒）
    
    // 对于只有视频的流，进行特殊处理
    if (ffp->video_only) {
        av_log(ffp, AV_LOG_INFO, "Configuring special parameters for video-only recording");
        
        // 设置更高的容错性
        ffp->m_ofmt_ctx->flags |= AVFMT_FLAG_GENPTS;  // 生成PTS
        ffp->m_ofmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;  // 不缓冲
        
        // 设置MP4特定参数
        if (strcmp(ffp->m_ofmt->name, "mp4") == 0) {
            av_log(ffp, AV_LOG_INFO, "Setting MP4-specific parameters for video-only stream");
            av_dict_set(&ffp->m_ofmt_ctx->metadata, "encoder", "IJKPlayer", 0);
            av_dict_set(&ffp->m_ofmt_ctx->metadata, "creation_time", "now", 0);
            
            // 确保视频流有正确的时间基准
            if (ffp->out_video_stream_index >= 0) {
                AVStream *out_stream = ffp->m_ofmt_ctx->streams[ffp->out_video_stream_index];
                out_stream->time_base = (AVRational){1, 90000};  // 标准MP4时间基准
            }
        }
    }
    
    // 初始化互斥锁
    if (pthread_mutex_init(&ffp->record_mutex, NULL) != 0) {
        av_log(ffp, AV_LOG_ERROR, "Failed to initialize recording mutex");
        ffp->is_record = 0;
        goto end;
    }
    
    av_log(ffp, AV_LOG_INFO, "Transcode recording started successfully with direct video copy: %d", ffp->direct_video_copy);
    return 0;
    
end:
    av_log(ffp, AV_LOG_ERROR, "Failed to start transcode recording, cleaning up resources");
    
    if (ffp->video_enc_ctx) {
        avcodec_free_context(&ffp->video_enc_ctx);
        ffp->video_enc_ctx = NULL;
    }
    
    if (ffp->audio_enc_ctx) {
        avcodec_free_context(&ffp->audio_enc_ctx);
        ffp->audio_enc_ctx = NULL;
    }
    
    if (ffp->sws_ctx) {
        sws_freeContext(ffp->sws_ctx);
        ffp->sws_ctx = NULL;
    }
    
    if (ffp->swr_ctx_record) {
        swr_free(&ffp->swr_ctx_record);
        ffp->swr_ctx_record = NULL;
    }
    
    if (ffp->tmp_frame) {
        av_frame_free(&ffp->tmp_frame);
        ffp->tmp_frame = NULL;
    }
    
    if (ffp->m_ofmt_ctx) {
        if (ffp->m_ofmt_ctx->pb)
            avio_closep(&ffp->m_ofmt_ctx->pb);
        avformat_free_context(ffp->m_ofmt_ctx);
        ffp->m_ofmt_ctx = NULL;
    }
    
    ffp->record_error = 1;
    ffp->need_transcode = 0;
    return -1;
}

