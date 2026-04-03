#include "video_decoder.h"
#include "core/common/logging.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

struct VideoDecoder {
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    struct SwsContext *sws_ctx;
    int               stream_idx;
    AVFrame          *frame;
    AVFrame          *frame_rgb;
    AVPacket         *pkt;
    uint8_t          *rgb_buffer;
    int               width;
    int               height;
    double            fps;
    int               total_frames;
    double            duration_ms;
    int               current_frame;
    char              codec_name[64];
};

VPError video_decoder_open(VideoDecoder **out, const char *path) {
    if (!out || !path) return VP_ERR_INVALID_ARG;

    VideoDecoder *dec = calloc(1, sizeof(VideoDecoder));
    if (!dec) return VP_ERR_OUT_OF_MEMORY;

    /* Open file */
    if (avformat_open_input(&dec->fmt_ctx, path, NULL, NULL) < 0) {
        LOG_ERROR(NULL, "decoder", "Failed to open: %s", path);
        free(dec);
        return VP_ERR_FILE_NOT_FOUND;
    }

    if (avformat_find_stream_info(dec->fmt_ctx, NULL) < 0) {
        LOG_ERROR(NULL, "decoder", "Could not find stream info: %s", path);
        avformat_close_input(&dec->fmt_ctx);
        free(dec);
        return VP_ERR_FILE_CORRUPT;
    }

    /* Find video stream */
    dec->stream_idx = -1;
    for (unsigned i = 0; i < dec->fmt_ctx->nb_streams; i++) {
        if (dec->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            dec->stream_idx = (int)i;
            break;
        }
    }
    if (dec->stream_idx < 0) {
        LOG_ERROR(NULL, "decoder", "No video stream found: %s", path);
        avformat_close_input(&dec->fmt_ctx);
        free(dec);
        return VP_ERR_CODEC_UNSUPPORTED;
    }

    AVStream *vstream = dec->fmt_ctx->streams[dec->stream_idx];
    AVCodecParameters *par = vstream->codecpar;

    /* Find and open codec */
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        LOG_ERROR(NULL, "decoder", "Unsupported codec id=%d", par->codec_id);
        avformat_close_input(&dec->fmt_ctx);
        free(dec);
        return VP_ERR_CODEC_UNSUPPORTED;
    }

    dec->codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec->codec_ctx, par);

    if (avcodec_open2(dec->codec_ctx, codec, NULL) < 0) {
        LOG_ERROR(NULL, "decoder", "Could not open codec: %s", codec->name);
        avcodec_free_context(&dec->codec_ctx);
        avformat_close_input(&dec->fmt_ctx);
        free(dec);
        return VP_ERR_CODEC_UNSUPPORTED;
    }

    /* Metadata */
    dec->width  = dec->codec_ctx->width;
    dec->height = dec->codec_ctx->height;

    if (vstream->avg_frame_rate.den > 0)
        dec->fps = av_q2d(vstream->avg_frame_rate);
    else if (vstream->r_frame_rate.den > 0)
        dec->fps = av_q2d(vstream->r_frame_rate);
    else
        dec->fps = 25.0;

    dec->total_frames = (int)vstream->nb_frames;
    if (dec->total_frames <= 0 && dec->fmt_ctx->duration > 0) {
        dec->total_frames = (int)(dec->fps * (double)dec->fmt_ctx->duration / AV_TIME_BASE);
    }
    dec->duration_ms = (dec->fmt_ctx->duration > 0)
        ? (double)dec->fmt_ctx->duration / 1000.0
        : 0.0;

    strncpy(dec->codec_name, codec->name, sizeof(dec->codec_name) - 1);

    /* Allocate frames */
    dec->frame     = av_frame_alloc();
    dec->frame_rgb = av_frame_alloc();
    dec->pkt       = av_packet_alloc();

    /* RGB buffer */
    int buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dec->width, dec->height, 1);
    dec->rgb_buffer = av_malloc(buf_size);
    av_image_fill_arrays(dec->frame_rgb->data, dec->frame_rgb->linesize,
                         dec->rgb_buffer, AV_PIX_FMT_RGB24,
                         dec->width, dec->height, 1);

    /* SWS context for pixel format conversion */
    dec->sws_ctx = sws_getContext(
        dec->width, dec->height, dec->codec_ctx->pix_fmt,
        dec->width, dec->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    if (!dec->sws_ctx) {
        LOG_ERROR(NULL, "decoder", "Failed to create sws context");
        video_decoder_close(dec);
        return VP_ERR_INTERNAL;
    }

    dec->current_frame = 0;

    LOG_INFO(NULL, "decoder", "Opened %s: %dx%d, %.2f fps, %d frames, codec=%s",
             path, dec->width, dec->height, dec->fps, dec->total_frames, dec->codec_name);

    *out = dec;
    return VP_OK;
}

int video_decoder_width(const VideoDecoder *dec)  { return dec ? dec->width : 0; }
int video_decoder_height(const VideoDecoder *dec) { return dec ? dec->height : 0; }
double video_decoder_fps(const VideoDecoder *dec) { return dec ? dec->fps : 0.0; }
int video_decoder_total_frames(const VideoDecoder *dec) { return dec ? dec->total_frames : 0; }
double video_decoder_duration_ms(const VideoDecoder *dec) { return dec ? dec->duration_ms : 0.0; }
const char *video_decoder_codec_name(const VideoDecoder *dec) { return dec ? dec->codec_name : ""; }

VPError video_decoder_next_frame(VideoDecoder *dec, FrameBuffer *frame) {
    if (!dec || !frame) return VP_ERR_INVALID_ARG;

    while (1) {
        int ret = av_read_frame(dec->fmt_ctx, dec->pkt);
        if (ret < 0) return VP_ERR_DECODE_FAILED; /* EOF or error */

        if (dec->pkt->stream_index != dec->stream_idx) {
            av_packet_unref(dec->pkt);
            continue;
        }

        ret = avcodec_send_packet(dec->codec_ctx, dec->pkt);
        av_packet_unref(dec->pkt);
        if (ret < 0) continue;

        ret = avcodec_receive_frame(dec->codec_ctx, dec->frame);
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) return VP_ERR_DECODE_FAILED;

        /* Convert to RGB24 */
        sws_scale(dec->sws_ctx,
                  (const uint8_t *const *)dec->frame->data,
                  dec->frame->linesize,
                  0, dec->height,
                  dec->frame_rgb->data,
                  dec->frame_rgb->linesize);

        /* Fill output */
        frame->width        = dec->width;
        frame->height       = dec->height;
        frame->channels     = 3;
        frame->stride       = dec->frame_rgb->linesize[0];
        frame->frame_number = dec->current_frame;

        /* Timestamp from pts */
        AVStream *vs = dec->fmt_ctx->streams[dec->stream_idx];
        if (dec->frame->pts != AV_NOPTS_VALUE) {
            frame->timestamp_ms = (double)dec->frame->pts * av_q2d(vs->time_base) * 1000.0;
        } else {
            frame->timestamp_ms = (double)dec->current_frame / dec->fps * 1000.0;
        }

        /* Copy pixel data */
        size_t row_bytes = (size_t)dec->width * 3;
        if (!frame->data) {
            frame->data = malloc((size_t)dec->height * row_bytes);
            if (!frame->data) return VP_ERR_OUT_OF_MEMORY;
        }
        for (int y = 0; y < dec->height; y++) {
            memcpy(frame->data + y * row_bytes,
                   dec->frame_rgb->data[0] + y * dec->frame_rgb->linesize[0],
                   row_bytes);
        }
        frame->stride = (int)row_bytes;

        dec->current_frame++;
        return VP_OK;
    }
}

VPError video_decoder_seek(VideoDecoder *dec, int frame_number) {
    if (!dec) return VP_ERR_INVALID_ARG;
    AVStream *vs = dec->fmt_ctx->streams[dec->stream_idx];
    int64_t ts = (int64_t)((double)frame_number / dec->fps / av_q2d(vs->time_base));
    if (av_seek_frame(dec->fmt_ctx, dec->stream_idx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        return VP_ERR_DECODE_FAILED;
    }
    avcodec_flush_buffers(dec->codec_ctx);
    dec->current_frame = frame_number;
    return VP_OK;
}

void video_decoder_close(VideoDecoder *dec) {
    if (!dec) return;
    if (dec->sws_ctx)   sws_freeContext(dec->sws_ctx);
    if (dec->frame)     av_frame_free(&dec->frame);
    if (dec->frame_rgb) av_frame_free(&dec->frame_rgb);
    if (dec->pkt)       av_packet_free(&dec->pkt);
    if (dec->rgb_buffer) av_free(dec->rgb_buffer);
    if (dec->codec_ctx) avcodec_free_context(&dec->codec_ctx);
    if (dec->fmt_ctx)   avformat_close_input(&dec->fmt_ctx);
    free(dec);
}
