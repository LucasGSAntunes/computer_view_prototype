#ifndef VP_VIDEO_DECODER_H
#define VP_VIDEO_DECODER_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct VideoDecoder VideoDecoder;

/* Open video file and prepare decoder. */
VPError video_decoder_open(VideoDecoder **dec, const char *path);

/* Get video metadata. */
int     video_decoder_width(const VideoDecoder *dec);
int     video_decoder_height(const VideoDecoder *dec);
double  video_decoder_fps(const VideoDecoder *dec);
int     video_decoder_total_frames(const VideoDecoder *dec);
double  video_decoder_duration_ms(const VideoDecoder *dec);
const char *video_decoder_codec_name(const VideoDecoder *dec);

/* Decode next frame into FrameBuffer (RGB24).
 * Returns VP_OK on success, VP_ERR_DECODE_FAILED on EOF/error. */
VPError video_decoder_next_frame(VideoDecoder *dec, FrameBuffer *frame);

/* Seek to frame number (best-effort). */
VPError video_decoder_seek(VideoDecoder *dec, int frame_number);

/* Close and free resources. */
void    video_decoder_close(VideoDecoder *dec);

#endif /* VP_VIDEO_DECODER_H */
