/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 */

#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/types.h>
#ifndef _WIN32
#  include <dirent.h>
#else
#  include <io.h>
#endif

#include "BLI_path_util.h"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_scene_types.h"

#include "MEM_guardedalloc.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "IMB_colormanagement.hh"
#include "IMB_colormanagement_intern.hh"

#include "IMB_anim.hh"
#include "IMB_indexer.hh"
#include "IMB_metadata.hh"

#ifdef WITH_FFMPEG
#  include "BKE_writeffmpeg.hh"

extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libavutil/imgutils.h>
#  include <libavutil/rational.h>
#  include <libswscale/swscale.h>

#  include "ffmpeg_compat.h"
}

#endif /* WITH_FFMPEG */

#ifdef WITH_FFMPEG
static void free_anim_ffmpeg(ImBufAnim *anim);
#endif

void IMB_free_anim(ImBufAnim *anim)
{
  if (anim == nullptr) {
    printf("free anim, anim == nullptr\n");
    return;
  }

#ifdef WITH_FFMPEG
  free_anim_ffmpeg(anim);
#endif
  IMB_free_indices(anim);
  IMB_metadata_free(anim->metadata);

  MEM_freeN(anim);
}

void IMB_close_anim(ImBufAnim *anim)
{
  if (anim == nullptr) {
    return;
  }

  IMB_free_anim(anim);
}

void IMB_close_anim_proxies(ImBufAnim *anim)
{
  if (anim == nullptr) {
    return;
  }

  IMB_free_indices(anim);
}

IDProperty *IMB_anim_load_metadata(ImBufAnim *anim)
{
  if (anim->state == ImBufAnim::State::Valid) {
#ifdef WITH_FFMPEG
    BLI_assert(anim->pFormatCtx != nullptr);
    av_log(anim->pFormatCtx, AV_LOG_DEBUG, "METADATA FETCH\n");

    AVDictionaryEntry *entry = nullptr;
    while (true) {
      entry = av_dict_get(anim->pFormatCtx->metadata, "", entry, AV_DICT_IGNORE_SUFFIX);
      if (entry == nullptr) {
        break;
      }

      /* Delay creation of the property group until there is actual metadata to put in there. */
      IMB_metadata_ensure(&anim->metadata);
      IMB_metadata_set_field(anim->metadata, entry->key, entry->value);
    }
#endif
  }
  return anim->metadata;
}

ImBufAnim *IMB_open_anim(const char *filepath,
                         int ib_flags,
                         int streamindex,
                         char colorspace[IM_MAX_SPACE])
{
  ImBufAnim *anim;

  BLI_assert(!BLI_path_is_rel(filepath));

  anim = (ImBufAnim *)MEM_callocN(sizeof(ImBufAnim), "anim struct");
  if (anim != nullptr) {
    if (colorspace) {
      colorspace_set_default_role(colorspace, IM_MAX_SPACE, COLOR_ROLE_DEFAULT_BYTE);
      STRNCPY(anim->colorspace, colorspace);
    }
    else {
      colorspace_set_default_role(
          anim->colorspace, sizeof(anim->colorspace), COLOR_ROLE_DEFAULT_BYTE);
    }

    STRNCPY(anim->filepath, filepath);
    anim->ib_flags = ib_flags;
    anim->streamindex = streamindex;
  }
  return anim;
}

bool IMB_anim_can_produce_frames(const ImBufAnim *anim)
{
#if !defined(WITH_FFMPEG)
  UNUSED_VARS(anim);
#endif

#ifdef WITH_FFMPEG
  if (anim->pCodecCtx != nullptr) {
    return true;
  }
#endif
  return false;
}

void IMB_suffix_anim(ImBufAnim *anim, const char *suffix)
{
  STRNCPY(anim->suffix, suffix);
}

#ifdef WITH_FFMPEG

static int startffmpeg(ImBufAnim *anim)
{
  int i, video_stream_index;

  const AVCodec *pCodec;
  AVFormatContext *pFormatCtx = nullptr;
  AVCodecContext *pCodecCtx;
  AVRational frame_rate;
  AVStream *video_stream;
  int frs_num;
  double frs_den;
  int streamcount;

  /* The following for color space determination */
  int srcRange, dstRange, brightness, contrast, saturation;
  int *table;
  const int *inv_table;

  if (anim == nullptr) {
    return (-1);
  }

  streamcount = anim->streamindex;

  if (avformat_open_input(&pFormatCtx, anim->filepath, nullptr, nullptr) != 0) {
    return -1;
  }

  if (avformat_find_stream_info(pFormatCtx, nullptr) < 0) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  av_dump_format(pFormatCtx, 0, anim->filepath, 0);

  /* Find the video stream */
  video_stream_index = -1;

  for (i = 0; i < pFormatCtx->nb_streams; i++) {
    if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (streamcount > 0) {
        streamcount--;
        continue;
      }
      video_stream_index = i;
      break;
    }
  }

  if (video_stream_index == -1) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  video_stream = pFormatCtx->streams[video_stream_index];

  /* Find the decoder for the video stream */
  pCodec = avcodec_find_decoder(video_stream->codecpar->codec_id);
  if (pCodec == nullptr) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  pCodecCtx = avcodec_alloc_context3(nullptr);
  avcodec_parameters_to_context(pCodecCtx, video_stream->codecpar);
  pCodecCtx->workaround_bugs = FF_BUG_AUTODETECT;

  if (pCodec->capabilities & AV_CODEC_CAP_OTHER_THREADS) {
    pCodecCtx->thread_count = 0;
  }
  else {
    pCodecCtx->thread_count = BLI_system_thread_count();
  }

  if (pCodec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
    pCodecCtx->thread_type = FF_THREAD_FRAME;
  }
  else if (pCodec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
    pCodecCtx->thread_type = FF_THREAD_SLICE;
  }

  if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0) {
    avformat_close_input(&pFormatCtx);
    return -1;
  }
  if (pCodecCtx->pix_fmt == AV_PIX_FMT_NONE) {
    avcodec_free_context(&anim->pCodecCtx);
    avformat_close_input(&pFormatCtx);
    return -1;
  }

  double video_start = 0;
  double pts_time_base = av_q2d(video_stream->time_base);

  if (video_stream->start_time != AV_NOPTS_VALUE) {
    video_start = video_stream->start_time * pts_time_base;
  }

  frame_rate = av_guess_frame_rate(pFormatCtx, video_stream, nullptr);
  anim->duration_in_frames = 0;

  /* Take from the stream if we can. */
  if (video_stream->nb_frames != 0) {
    anim->duration_in_frames = video_stream->nb_frames;

    /* Sanity check on the detected duration. This is to work around corruption like reported in
     * #68091. */
    if (frame_rate.den != 0 && pFormatCtx->duration > 0) {
      double stream_sec = anim->duration_in_frames * av_q2d(frame_rate);
      double container_sec = pFormatCtx->duration / double(AV_TIME_BASE);
      if (stream_sec > 4.0 * container_sec) {
        /* The stream is significantly longer than the container duration, which is
         * suspicious. */
        anim->duration_in_frames = 0;
      }
    }
  }

  if (anim->duration_in_frames != 0) {
    /* Pass (already valid). */
  }
  else if (pFormatCtx->duration == AV_NOPTS_VALUE) {
    /* The duration has not been set, happens for single JPEG2000 images.
     * NOTE: Leave the duration zeroed, although it could set to 1 so the file is recognized
     * as a movie with 1 frame, leave as-is since image loading code-paths are preferred
     * in this case. */
  }
  else {
    /* Fall back to manually estimating the video stream duration.
     * This is because the video stream duration can be shorter than the `pFormatCtx->duration`. */
    BLI_assert(anim->duration_in_frames == 0);
    double stream_dur;

    if (video_stream->duration != AV_NOPTS_VALUE) {
      stream_dur = video_stream->duration * pts_time_base;
    }
    else {
      double audio_start = 0;

      /* Find audio stream to guess the duration of the video.
       * Sometimes the audio AND the video stream have a start offset.
       * The difference between these is the offset we want to use to
       * calculate the video duration.
       */
      for (i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
          AVStream *audio_stream = pFormatCtx->streams[i];
          if (audio_stream->start_time != AV_NOPTS_VALUE) {
            audio_start = audio_stream->start_time * av_q2d(audio_stream->time_base);
          }
          break;
        }
      }

      if (video_start > audio_start) {
        stream_dur = double(pFormatCtx->duration) / AV_TIME_BASE - (video_start - audio_start);
      }
      else {
        /* The video stream starts before or at the same time as the audio stream!
         * We have to assume that the video stream is as long as the full pFormatCtx->duration.
         */
        stream_dur = double(pFormatCtx->duration) / AV_TIME_BASE;
      }
    }
    anim->duration_in_frames = int(stream_dur * av_q2d(frame_rate) + 0.5f);
  }

  frs_num = frame_rate.num;
  frs_den = frame_rate.den;

  frs_den *= AV_TIME_BASE;

  while (frs_num % 10 == 0 && frs_den >= 2.0 && frs_num > 10) {
    frs_num /= 10;
    frs_den /= 10;
  }

  anim->frs_sec = frs_num;
  anim->frs_sec_base = frs_den;
  /* Save the relative start time for the video. IE the start time in relation to where playback
   * starts. */
  anim->start_offset = video_start;

  anim->x = pCodecCtx->width;
  anim->y = pCodecCtx->height;

  anim->pFormatCtx = pFormatCtx;
  anim->pCodecCtx = pCodecCtx;
  anim->pCodec = pCodec;
  anim->videoStream = video_stream_index;

  anim->cur_position = 0;
  anim->cur_pts = -1;
  anim->cur_key_frame_pts = -1;
  anim->cur_packet = av_packet_alloc();
  anim->cur_packet->stream_index = -1;

  anim->pFrame = av_frame_alloc();
  anim->pFrame_backup = av_frame_alloc();
  anim->pFrame_backup_complete = false;
  anim->pFrame_complete = false;
  anim->pFrameDeinterlaced = av_frame_alloc();
  anim->pFrameRGB = av_frame_alloc();
  anim->pFrameRGB->format = AV_PIX_FMT_RGBA;
  anim->pFrameRGB->width = anim->x;
  anim->pFrameRGB->height = anim->y;

  if (av_frame_get_buffer(anim->pFrameRGB, 0) < 0) {
    fprintf(stderr, "Could not allocate frame data.\n");
    avcodec_free_context(&anim->pCodecCtx);
    avformat_close_input(&anim->pFormatCtx);
    av_packet_free(&anim->cur_packet);
    av_frame_free(&anim->pFrameRGB);
    av_frame_free(&anim->pFrameDeinterlaced);
    av_frame_free(&anim->pFrame);
    av_frame_free(&anim->pFrame_backup);
    anim->pCodecCtx = nullptr;
    return -1;
  }

  if (av_image_get_buffer_size(AV_PIX_FMT_RGBA, anim->x, anim->y, 1) != anim->x * anim->y * 4) {
    fprintf(stderr, "ffmpeg has changed alloc scheme ... ARGHHH!\n");
    avcodec_free_context(&anim->pCodecCtx);
    avformat_close_input(&anim->pFormatCtx);
    av_packet_free(&anim->cur_packet);
    av_frame_free(&anim->pFrameRGB);
    av_frame_free(&anim->pFrameDeinterlaced);
    av_frame_free(&anim->pFrame);
    av_frame_free(&anim->pFrame_backup);
    anim->pCodecCtx = nullptr;
    return -1;
  }

  if (anim->ib_flags & IB_animdeinterlace) {
    av_image_fill_arrays(
        anim->pFrameDeinterlaced->data,
        anim->pFrameDeinterlaced->linesize,
        static_cast<const uint8_t *>(MEM_callocN(
            av_image_get_buffer_size(
                anim->pCodecCtx->pix_fmt, anim->pCodecCtx->width, anim->pCodecCtx->height, 1),
            "ffmpeg deinterlace")),
        anim->pCodecCtx->pix_fmt,
        anim->pCodecCtx->width,
        anim->pCodecCtx->height,
        1);
  }

  anim->img_convert_ctx = BKE_ffmpeg_sws_get_context(anim->x,
                                                     anim->y,
                                                     anim->pCodecCtx->pix_fmt,
                                                     AV_PIX_FMT_RGBA,
                                                     SWS_BILINEAR | SWS_PRINT_INFO |
                                                         SWS_FULL_CHR_H_INT);

  if (!anim->img_convert_ctx) {
    fprintf(stderr, "Can't transform color space??? Bailing out...\n");
    avcodec_free_context(&anim->pCodecCtx);
    avformat_close_input(&anim->pFormatCtx);
    av_packet_free(&anim->cur_packet);
    av_frame_free(&anim->pFrameRGB);
    av_frame_free(&anim->pFrameDeinterlaced);
    av_frame_free(&anim->pFrame);
    av_frame_free(&anim->pFrame_backup);
    anim->pCodecCtx = nullptr;
    return -1;
  }

  /* Try do detect if input has 0-255 YCbCR range (JFIF, JPEG, Motion-JPEG). */
  if (!sws_getColorspaceDetails(anim->img_convert_ctx,
                                (int **)&inv_table,
                                &srcRange,
                                &table,
                                &dstRange,
                                &brightness,
                                &contrast,
                                &saturation))
  {
    srcRange = srcRange || anim->pCodecCtx->color_range == AVCOL_RANGE_JPEG;
    inv_table = sws_getCoefficients(anim->pCodecCtx->colorspace);

    if (sws_setColorspaceDetails(anim->img_convert_ctx,
                                 (int *)inv_table,
                                 srcRange,
                                 table,
                                 dstRange,
                                 brightness,
                                 contrast,
                                 saturation))
    {
      fprintf(stderr, "Warning: Could not set libswscale colorspace details.\n");
    }
  }
  else {
    fprintf(stderr, "Warning: Could not set libswscale colorspace details.\n");
  }

  return 0;
}

static double ffmpeg_steps_per_frame_get(ImBufAnim *anim)
{
  AVStream *v_st = anim->pFormatCtx->streams[anim->videoStream];
  AVRational time_base = v_st->time_base;
  AVRational frame_rate = av_guess_frame_rate(anim->pFormatCtx, v_st, nullptr);
  return av_q2d(av_inv_q(av_mul_q(frame_rate, time_base)));
}

/* Store backup frame.
 * With VFR movies, if PTS is not matched perfectly, scanning continues to look for next PTS.
 * It is likely to overshoot and scanning stops. Having previous frame backed up, it is possible
 * to use it when overshoot happens.
 */
static void ffmpeg_double_buffer_backup_frame_store(ImBufAnim *anim, int64_t pts_to_search)
{
  /* `anim->pFrame` is beyond `pts_to_search`. Don't store it. */
  if (anim->pFrame_backup_complete && anim->cur_pts >= pts_to_search) {
    return;
  }
  if (!anim->pFrame_complete) {
    return;
  }

  if (anim->pFrame_backup_complete) {
    av_frame_unref(anim->pFrame_backup);
  }

  av_frame_move_ref(anim->pFrame_backup, anim->pFrame);
  anim->pFrame_backup_complete = true;
}

/* Free stored backup frame. */
static void ffmpeg_double_buffer_backup_frame_clear(ImBufAnim *anim)
{
  if (anim->pFrame_backup_complete) {
    av_frame_unref(anim->pFrame_backup);
  }
  anim->pFrame_backup_complete = false;
}

/* Return recently decoded frame. If it does not exist, return frame from backup buffer. */
static AVFrame *ffmpeg_double_buffer_frame_fallback_get(ImBufAnim *anim)
{
  av_log(anim->pFormatCtx, AV_LOG_ERROR, "DECODE UNHAPPY: PTS not matched!\n");

  if (anim->pFrame_complete) {
    return anim->pFrame;
  }
  if (anim->pFrame_backup_complete) {
    return anim->pFrame_backup;
  }
  return nullptr;
}

/**
 * Postprocess the image in anim->pFrame and do color conversion and de-interlacing stuff.
 *
 * \param ibuf: The frame just read by `ffmpeg_fetchibuf`, processed in-place.
 */
static void ffmpeg_postprocess(ImBufAnim *anim, AVFrame *input, ImBuf *ibuf)
{
  int filter_y = 0;

  /* This means the data wasn't read properly,
   * this check stops crashing */
  if (input->data[0] == nullptr && input->data[1] == nullptr && input->data[2] == nullptr &&
      input->data[3] == nullptr)
  {
    fprintf(stderr,
            "ffmpeg_fetchibuf: "
            "data not read properly...\n");
    return;
  }

  av_log(anim->pFormatCtx,
         AV_LOG_DEBUG,
         "  POSTPROC: AVFrame planes: %p %p %p %p\n",
         input->data[0],
         input->data[1],
         input->data[2],
         input->data[3]);

  if (anim->ib_flags & IB_animdeinterlace) {
    if (av_image_deinterlace(anim->pFrameDeinterlaced,
                             anim->pFrame,
                             anim->pCodecCtx->pix_fmt,
                             anim->pCodecCtx->width,
                             anim->pCodecCtx->height) < 0)
    {
      filter_y = true;
    }
    else {
      input = anim->pFrameDeinterlaced;
    }
  }

  /* If final destination image layout matches that of decoded RGB frame (including
   * any line padding done by ffmpeg for SIMD alignment), we can directly
   * decode into that, doing the vertical flip in the same step. Otherwise have
   * to do a separate flip. */
  const int ibuf_linesize = ibuf->x * 4;
  const int rgb_linesize = anim->pFrameRGB->linesize[0];
  bool scale_to_ibuf = (rgb_linesize == ibuf_linesize);
  /* swscale on arm64 before ffmpeg 6.0 (libswscale major version 7)
   * could not handle negative line sizes. That has been fixed in all major
   * ffmpeg releases in early 2023, but easier to just check for "below 7". */
#  if (defined(__aarch64__) || defined(_M_ARM64)) && (LIBSWSCALE_VERSION_MAJOR < 7)
  scale_to_ibuf = false;
#  endif
  uint8_t *rgb_data = anim->pFrameRGB->data[0];

  if (scale_to_ibuf) {
    /* Decode RGB and do vertical flip directly into destination image, by using negative
     * line size. */
    anim->pFrameRGB->linesize[0] = -ibuf_linesize;
    anim->pFrameRGB->data[0] = ibuf->byte_buffer.data + (ibuf->y - 1) * ibuf_linesize;

    BKE_ffmpeg_sws_scale_frame(anim->img_convert_ctx, anim->pFrameRGB, input);

    anim->pFrameRGB->linesize[0] = rgb_linesize;
    anim->pFrameRGB->data[0] = rgb_data;
  }
  else {
    /* Decode, then do vertical flip into destination. */
    BKE_ffmpeg_sws_scale_frame(anim->img_convert_ctx, anim->pFrameRGB, input);

    /* Use negative line size to do vertical image flip. */
    const int src_linesize[4] = {-rgb_linesize, 0, 0, 0};
    const uint8_t *const src[4] = {
        rgb_data + (anim->y - 1) * rgb_linesize, nullptr, nullptr, nullptr};
    int dst_size = av_image_get_buffer_size(AVPixelFormat(anim->pFrameRGB->format),
                                            anim->pFrameRGB->width,
                                            anim->pFrameRGB->height,
                                            1);
    av_image_copy_to_buffer(
        ibuf->byte_buffer.data, dst_size, src, src_linesize, AV_PIX_FMT_RGBA, anim->x, anim->y, 1);
  }

  if (filter_y) {
    IMB_filtery(ibuf);
  }
}

static void final_frame_log(ImBufAnim *anim,
                            int64_t frame_pts_start,
                            int64_t frame_pts_end,
                            const char *str)
{
  av_log(anim->pFormatCtx,
         AV_LOG_INFO,
         "DECODE HAPPY: %s frame PTS range %" PRId64 " - %" PRId64 ".\n",
         str,
         frame_pts_start,
         frame_pts_end);
}

static bool ffmpeg_pts_isect(int64_t pts_start, int64_t pts_end, int64_t pts_to_search)
{
  return pts_start <= pts_to_search && pts_to_search < pts_end;
}

/* Return frame that matches `pts_to_search`, nullptr if matching frame does not exist. */
static AVFrame *ffmpeg_frame_by_pts_get(ImBufAnim *anim, int64_t pts_to_search)
{
  /* NOTE: `frame->pts + frame->pkt_duration` does not always match pts of next frame.
   * See footage from #86361. Here it is OK to use, because PTS must match current or backup frame.
   * If there is no current frame, return nullptr.
   */
  if (!anim->pFrame_complete) {
    return nullptr;
  }

  const bool backup_frame_ready = anim->pFrame_backup_complete;
  const int64_t recent_start = av_get_pts_from_frame(anim->pFrame);
  const int64_t recent_end = recent_start + av_get_frame_duration_in_pts_units(anim->pFrame);
  const int64_t backup_start = backup_frame_ready ? av_get_pts_from_frame(anim->pFrame_backup) : 0;

  AVFrame *best_frame = nullptr;
  if (ffmpeg_pts_isect(recent_start, recent_end, pts_to_search)) {
    final_frame_log(anim, recent_start, recent_end, "Recent");
    best_frame = anim->pFrame;
  }
  else if (backup_frame_ready && ffmpeg_pts_isect(backup_start, recent_start, pts_to_search)) {
    final_frame_log(anim, backup_start, recent_start, "Backup");
    best_frame = anim->pFrame_backup;
  }
  return best_frame;
}

static void ffmpeg_decode_store_frame_pts(ImBufAnim *anim)
{
  anim->cur_pts = av_get_pts_from_frame(anim->pFrame);

  if (anim->pFrame->key_frame) {
    anim->cur_key_frame_pts = anim->cur_pts;
  }

  av_log(anim->pFormatCtx,
         AV_LOG_DEBUG,
         "  FRAME DONE: cur_pts=%" PRId64 ", guessed_pts=%" PRId64 "\n",
         av_get_pts_from_frame(anim->pFrame),
         int64_t(anim->cur_pts));
}

static int ffmpeg_read_video_frame(ImBufAnim *anim, AVPacket *packet)
{
  int ret = 0;
  while ((ret = av_read_frame(anim->pFormatCtx, packet)) >= 0) {
    if (packet->stream_index == anim->videoStream) {
      break;
    }
    av_packet_unref(packet);
    packet->stream_index = -1;
  }

  return ret;
}

/* decode one video frame also considering the packet read into cur_packet */
static int ffmpeg_decode_video_frame(ImBufAnim *anim)
{
  av_log(anim->pFormatCtx, AV_LOG_DEBUG, "  DECODE VIDEO FRAME\n");

  /* Sometimes, decoder returns more than one frame per sent packet. Check if frames are available.
   * This frames must be read, otherwise decoding will fail. See #91405. */
  anim->pFrame_complete = avcodec_receive_frame(anim->pCodecCtx, anim->pFrame) == 0;
  if (anim->pFrame_complete) {
    av_log(anim->pFormatCtx, AV_LOG_DEBUG, "  DECODE FROM CODEC BUFFER\n");
    ffmpeg_decode_store_frame_pts(anim);
    return 1;
  }

  int rval = 0;
  if (anim->cur_packet->stream_index == anim->videoStream) {
    av_packet_unref(anim->cur_packet);
    anim->cur_packet->stream_index = -1;
  }

  while ((rval = ffmpeg_read_video_frame(anim, anim->cur_packet)) >= 0) {
    if (anim->cur_packet->stream_index != anim->videoStream) {
      continue;
    }

    av_log(anim->pFormatCtx,
           AV_LOG_DEBUG,
           "READ: strID=%d dts=%" PRId64 " pts=%" PRId64 " %s\n",
           anim->cur_packet->stream_index,
           (anim->cur_packet->dts == AV_NOPTS_VALUE) ? -1 : int64_t(anim->cur_packet->dts),
           (anim->cur_packet->pts == AV_NOPTS_VALUE) ? -1 : int64_t(anim->cur_packet->pts),
           (anim->cur_packet->flags & AV_PKT_FLAG_KEY) ? " KEY" : "");

    avcodec_send_packet(anim->pCodecCtx, anim->cur_packet);
    anim->pFrame_complete = avcodec_receive_frame(anim->pCodecCtx, anim->pFrame) == 0;

    if (anim->pFrame_complete) {
      ffmpeg_decode_store_frame_pts(anim);
      break;
    }
    av_packet_unref(anim->cur_packet);
    anim->cur_packet->stream_index = -1;
  }

  if (rval == AVERROR_EOF) {
    /* Flush any remaining frames out of the decoder. */
    avcodec_send_packet(anim->pCodecCtx, nullptr);
    anim->pFrame_complete = avcodec_receive_frame(anim->pCodecCtx, anim->pFrame) == 0;

    if (anim->pFrame_complete) {
      ffmpeg_decode_store_frame_pts(anim);
      rval = 0;
    }
  }

  if (rval < 0) {
    av_packet_unref(anim->cur_packet);
    anim->cur_packet->stream_index = -1;

    char error_str[AV_ERROR_MAX_STRING_SIZE];
    av_make_error_string(error_str, AV_ERROR_MAX_STRING_SIZE, rval);

    av_log(anim->pFormatCtx,
           AV_LOG_ERROR,
           "  DECODE READ FAILED: av_read_frame() "
           "returned error: %s\n",
           error_str);
  }

  return (rval >= 0);
}

static int match_format(const char *name, AVFormatContext *pFormatCtx)
{
  const char *p;
  int len, namelen;

  const char *names = pFormatCtx->iformat->name;

  if (!name || !names) {
    return 0;
  }

  namelen = strlen(name);
  while ((p = strchr(names, ','))) {
    len = std::max(int(p - names), namelen);
    if (!BLI_strncasecmp(name, names, len)) {
      return 1;
    }
    names = p + 1;
  }
  return !BLI_strcasecmp(name, names);
}

static int ffmpeg_seek_by_byte(AVFormatContext *pFormatCtx)
{
  static const char *byte_seek_list[] = {"mpegts", nullptr};
  const char **p;

  if (pFormatCtx->iformat->flags & AVFMT_TS_DISCONT) {
    return true;
  }

  p = byte_seek_list;

  while (*p) {
    if (match_format(*p++, pFormatCtx)) {
      return true;
    }
  }

  return false;
}

static int64_t ffmpeg_get_seek_pts(ImBufAnim *anim, int64_t pts_to_search)
{
  /* FFMPEG seeks internally using DTS values instead of PTS. In some files DTS and PTS values are
   * offset and sometimes FFMPEG fails to take this into account when seeking.
   * Therefore we need to seek backwards a certain offset to make sure the frame we want is in
   * front of us. It is not possible to determine the exact needed offset,
   * this value is determined experimentally.
   * NOTE: Too big offset can impact performance. Current 3 frame offset has no measurable impact.
   */
  int64_t seek_pts = pts_to_search - (ffmpeg_steps_per_frame_get(anim) * 3);

  if (seek_pts < 0) {
    seek_pts = 0;
  }
  return seek_pts;
}

/* This gives us an estimate of which pts our requested frame will have.
 * Note that this might be off a bit in certain video files, but it should still be close enough.
 */
static int64_t ffmpeg_get_pts_to_search(ImBufAnim *anim, ImBufAnimIndex *tc_index, int position)
{
  int64_t pts_to_search;

  if (tc_index) {
    int new_frame_index = IMB_indexer_get_frame_index(tc_index, position);
    pts_to_search = IMB_indexer_get_pts(tc_index, new_frame_index);
  }
  else {
    AVStream *v_st = anim->pFormatCtx->streams[anim->videoStream];
    int64_t start_pts = v_st->start_time;

    pts_to_search = round(position * ffmpeg_steps_per_frame_get(anim));

    if (start_pts != AV_NOPTS_VALUE) {
      pts_to_search += start_pts;
    }
  }
  return pts_to_search;
}

static bool ffmpeg_is_first_frame_decode(ImBufAnim *anim)
{
  return anim->pFrame_complete == false;
}

static void ffmpeg_scan_log(ImBufAnim *anim, int64_t pts_to_search)
{
  int64_t frame_pts_start = av_get_pts_from_frame(anim->pFrame);
  int64_t frame_pts_end = frame_pts_start + av_get_frame_duration_in_pts_units(anim->pFrame);
  av_log(anim->pFormatCtx,
         AV_LOG_DEBUG,
         "  SCAN WHILE: PTS range %" PRId64 " - %" PRId64 " in search of %" PRId64 "\n",
         frame_pts_start,
         frame_pts_end,
         pts_to_search);
}

/* Decode frames one by one until its PTS matches pts_to_search. */
static void ffmpeg_decode_video_frame_scan(ImBufAnim *anim, int64_t pts_to_search)
{
  const int64_t start_gop_frame = anim->cur_key_frame_pts;
  bool decode_error = false;

  while (!decode_error && anim->cur_pts < pts_to_search) {
    ffmpeg_scan_log(anim, pts_to_search);
    ffmpeg_double_buffer_backup_frame_store(anim, pts_to_search);
    decode_error = ffmpeg_decode_video_frame(anim) < 1;

    /* We should not get a new GOP keyframe while scanning if seeking is working as intended.
     * If this condition triggers, there may be and error in our seeking code.
     * NOTE: This seems to happen if DTS value is used for seeking in ffmpeg internally. There
     * seems to be no good way to handle such case. */
    if (anim->seek_before_decode && start_gop_frame != anim->cur_key_frame_pts) {
      av_log(anim->pFormatCtx, AV_LOG_ERROR, "SCAN: Frame belongs to an unexpected GOP!\n");
    }
  }
}

/* Wrapper over av_seek_frame(), for formats that doesn't have its own read_seek() or
 * read_seek2() functions defined. When seeking in these formats, rule to seek to last
 * necessary I-frame is not honored. It is not even guaranteed that I-frame, that must be
 * decoded will be read. See https://trac.ffmpeg.org/ticket/1607 & #86944. */
static int ffmpeg_generic_seek_workaround(ImBufAnim *anim,
                                          int64_t *requested_pts,
                                          int64_t pts_to_search)
{
  int64_t current_pts = *requested_pts;
  int64_t offset = 0;

  int64_t cur_pts, prev_pts = -1;

  /* Step backward frame by frame until we find the key frame we are looking for. */
  while (current_pts != 0) {
    current_pts = *requested_pts - int64_t(round(offset * ffmpeg_steps_per_frame_get(anim)));
    current_pts = std::max(current_pts, int64_t(0));

    /* Seek to timestamp. */
    if (av_seek_frame(anim->pFormatCtx, anim->videoStream, current_pts, AVSEEK_FLAG_BACKWARD) < 0)
    {
      break;
    }

    /* Read first video stream packet. */
    AVPacket *read_packet = av_packet_alloc();
    while (av_read_frame(anim->pFormatCtx, read_packet) >= 0) {
      if (read_packet->stream_index == anim->videoStream) {
        break;
      }
      av_packet_unref(read_packet);
    }

    /* If this packet contains an I-frame, this could be the frame that we need. */
    bool is_key_frame = read_packet->flags & AV_PKT_FLAG_KEY;
    /* We need to check the packet timestamp as the key frame could be for a GOP forward in the
     * video stream. So if it has a larger timestamp than the frame we want, ignore it.
     */
    cur_pts = timestamp_from_pts_or_dts(read_packet->pts, read_packet->dts);
    av_packet_free(&read_packet);

    if (is_key_frame) {
      if (cur_pts <= pts_to_search) {
        /* We found the I-frame we were looking for! */
        break;
      }
    }

    if (cur_pts == prev_pts) {
      /* We got the same key frame packet twice.
       * This probably means that we have hit the beginning of the stream. */
      break;
    }

    prev_pts = cur_pts;
    offset++;
  }

  *requested_pts = current_pts;

  /* Re-seek to timestamp that gave I-frame, so it can be read by decode function. */
  return av_seek_frame(anim->pFormatCtx, anim->videoStream, current_pts, AVSEEK_FLAG_BACKWARD);
}

/* Read packet until timestamp matches `anim->cur_packet`, thus recovering internal `anim` stream
 * position state. */
static void ffmpeg_seek_recover_stream_position(ImBufAnim *anim)
{
  AVPacket *temp_packet = av_packet_alloc();
  while (ffmpeg_read_video_frame(anim, temp_packet) >= 0) {
    int64_t current_pts = timestamp_from_pts_or_dts(anim->cur_packet->pts, anim->cur_packet->dts);
    int64_t temp_pts = timestamp_from_pts_or_dts(temp_packet->pts, temp_packet->dts);
    av_packet_unref(temp_packet);

    if (current_pts == temp_pts) {
      break;
    }
  }
  av_packet_free(&temp_packet);
}

/* Check if seeking and mainly flushing codec buffers is needed. */
static bool ffmpeg_seek_buffers_need_flushing(ImBufAnim *anim, int position, int64_t seek_pos)
{
  /* Get timestamp of packet read after seeking. */
  AVPacket *temp_packet = av_packet_alloc();
  ffmpeg_read_video_frame(anim, temp_packet);
  int64_t gop_pts = timestamp_from_pts_or_dts(temp_packet->pts, temp_packet->dts);
  av_packet_unref(temp_packet);
  av_packet_free(&temp_packet);

  /* Seeking gives packet, that is currently read. No seeking was necessary, so buffers don't have
   * to be flushed. */
  if (gop_pts == timestamp_from_pts_or_dts(anim->cur_packet->pts, anim->cur_packet->dts)) {
    return false;
  }

  /* Packet after seeking is same key frame as current, and further in time. No seeking was
   * necessary, so buffers don't have to be flushed. But stream position has to be recovered. */
  if (gop_pts == anim->cur_key_frame_pts && position > anim->cur_position) {
    ffmpeg_seek_recover_stream_position(anim);
    return false;
  }

  /* Seeking was necessary, but we have read packets. Therefore we must seek again. */
  av_seek_frame(anim->pFormatCtx, anim->videoStream, seek_pos, AVSEEK_FLAG_BACKWARD);
  anim->cur_key_frame_pts = gop_pts;
  return true;
}

/* Seek to last necessary key frame. */
static int ffmpeg_seek_to_key_frame(ImBufAnim *anim,
                                    int position,
                                    ImBufAnimIndex *tc_index,
                                    int64_t pts_to_search)
{
  int64_t seek_pos;
  int ret;

  if (tc_index) {
    /* We can use timestamps generated from our indexer to seek. */
    int new_frame_index = IMB_indexer_get_frame_index(tc_index, position);
    int old_frame_index = IMB_indexer_get_frame_index(tc_index, anim->cur_position);

    if (IMB_indexer_can_scan(tc_index, old_frame_index, new_frame_index)) {
      /* No need to seek, return early. */
      return 0;
    }
    uint64_t pts;
    uint64_t dts;

    seek_pos = IMB_indexer_get_seek_pos(tc_index, new_frame_index);
    pts = IMB_indexer_get_seek_pos_pts(tc_index, new_frame_index);
    dts = IMB_indexer_get_seek_pos_dts(tc_index, new_frame_index);

    anim->cur_key_frame_pts = timestamp_from_pts_or_dts(pts, dts);

    av_log(anim->pFormatCtx, AV_LOG_DEBUG, "TC INDEX seek seek_pos = %" PRId64 "\n", seek_pos);
    av_log(anim->pFormatCtx, AV_LOG_DEBUG, "TC INDEX seek pts = %" PRIu64 "\n", pts);
    av_log(anim->pFormatCtx, AV_LOG_DEBUG, "TC INDEX seek dts = %" PRIu64 "\n", dts);

    if (ffmpeg_seek_by_byte(anim->pFormatCtx)) {
      av_log(anim->pFormatCtx, AV_LOG_DEBUG, "... using BYTE seek_pos\n");

      ret = av_seek_frame(anim->pFormatCtx, -1, seek_pos, AVSEEK_FLAG_BYTE);
    }
    else {
      av_log(anim->pFormatCtx, AV_LOG_DEBUG, "... using PTS seek_pos\n");
      ret = av_seek_frame(
          anim->pFormatCtx, anim->videoStream, anim->cur_key_frame_pts, AVSEEK_FLAG_BACKWARD);
    }
  }
  else {
    /* We have to manually seek with ffmpeg to get to the key frame we want to start decoding from.
     */
    seek_pos = ffmpeg_get_seek_pts(anim, pts_to_search);
    av_log(
        anim->pFormatCtx, AV_LOG_DEBUG, "NO INDEX final seek seek_pos = %" PRId64 "\n", seek_pos);

    AVFormatContext *format_ctx = anim->pFormatCtx;

    if (format_ctx->iformat->read_seek2 || format_ctx->iformat->read_seek) {
      ret = av_seek_frame(anim->pFormatCtx, anim->videoStream, seek_pos, AVSEEK_FLAG_BACKWARD);
    }
    else {
      ret = ffmpeg_generic_seek_workaround(anim, &seek_pos, pts_to_search);
      av_log(anim->pFormatCtx,
             AV_LOG_DEBUG,
             "Adjusted final seek seek_pos = %" PRId64 "\n",
             seek_pos);
    }

    if (ret <= 0 && !ffmpeg_seek_buffers_need_flushing(anim, position, seek_pos)) {
      return 0;
    }
  }

  if (ret < 0) {
    av_log(anim->pFormatCtx,
           AV_LOG_ERROR,
           "FETCH: "
           "error while seeking to DTS = %" PRId64 " (frameno = %d, PTS = %" PRId64
           "): errcode = %d\n",
           seek_pos,
           position,
           pts_to_search,
           ret);
  }
  /* Flush the internal buffers of ffmpeg. This needs to be done after seeking to avoid decoding
   * errors. */
  avcodec_flush_buffers(anim->pCodecCtx);
  ffmpeg_double_buffer_backup_frame_clear(anim);

  anim->cur_pts = -1;

  if (anim->cur_packet->stream_index == anim->videoStream) {
    av_packet_unref(anim->cur_packet);
    anim->cur_packet->stream_index = -1;
  }

  return ret;
}

static bool ffmpeg_must_seek(ImBufAnim *anim, int position)
{
  bool must_seek = position != anim->cur_position + 1 || ffmpeg_is_first_frame_decode(anim);
  anim->seek_before_decode = must_seek;
  return must_seek;
}

static ImBuf *ffmpeg_fetchibuf(ImBufAnim *anim, int position, IMB_Timecode_Type tc)
{
  if (anim == nullptr) {
    return nullptr;
  }

  av_log(anim->pFormatCtx, AV_LOG_DEBUG, "FETCH: seek_pos=%d\n", position);

  ImBufAnimIndex *tc_index = IMB_anim_open_index(anim, tc);
  int64_t pts_to_search = ffmpeg_get_pts_to_search(anim, tc_index, position);
  AVStream *v_st = anim->pFormatCtx->streams[anim->videoStream];
  double frame_rate = av_q2d(v_st->r_frame_rate);
  double pts_time_base = av_q2d(v_st->time_base);
  int64_t start_pts = v_st->start_time;

  av_log(anim->pFormatCtx,
         AV_LOG_DEBUG,
         "FETCH: looking for PTS=%" PRId64 " (pts_timebase=%g, frame_rate=%g, start_pts=%" PRId64
         ")\n",
         int64_t(pts_to_search),
         pts_time_base,
         frame_rate,
         start_pts);

  if (ffmpeg_must_seek(anim, position)) {
    ffmpeg_seek_to_key_frame(anim, position, tc_index, pts_to_search);
  }

  ffmpeg_decode_video_frame_scan(anim, pts_to_search);

  /* Update resolution as it can change per-frame with WebM. See #100741 & #100081. */
  anim->x = anim->pCodecCtx->width;
  anim->y = anim->pCodecCtx->height;

  /* Certain versions of FFmpeg have a bug in libswscale which ends up in crash
   * when destination buffer is not properly aligned. For example, this happens
   * in FFmpeg 4.3.1. It got fixed later on, but for compatibility reasons is
   * still best to avoid crash.
   *
   * This is achieved by using own allocation call rather than relying on
   * IMB_allocImBuf() to do so since the IMB_allocImBuf() is not guaranteed
   * to perform aligned allocation.
   *
   * In theory this could give better performance, since SIMD operations on
   * aligned data are usually faster.
   *
   * Note that even though sometimes vertical flip is required it does not
   * affect on alignment of data passed to sws_scale because if the X dimension
   * is not 32 byte aligned special intermediate buffer is allocated.
   *
   * The issue was reported to FFmpeg under ticket #8747 in the FFmpeg tracker
   * and is fixed in the newer versions than 4.3.1. */

  const AVPixFmtDescriptor *pix_fmt_descriptor = av_pix_fmt_desc_get(anim->pCodecCtx->pix_fmt);

  int planes = R_IMF_PLANES_RGBA;
  if ((pix_fmt_descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) == 0) {
    planes = R_IMF_PLANES_RGB;
  }

  ImBuf *cur_frame_final = IMB_allocImBuf(anim->x, anim->y, planes, 0);

  /* Allocate the storage explicitly to ensure the memory is aligned. */
  uint8_t *buffer_data = static_cast<uint8_t *>(
      MEM_mallocN_aligned(size_t(4) * anim->x * anim->y, 32, "ffmpeg ibuf"));
  IMB_assign_byte_buffer(cur_frame_final, buffer_data, IB_TAKE_OWNERSHIP);

  cur_frame_final->byte_buffer.colorspace = colormanage_colorspace_get_named(anim->colorspace);

  AVFrame *final_frame = ffmpeg_frame_by_pts_get(anim, pts_to_search);
  if (final_frame == nullptr) {
    /* No valid frame was decoded for requested PTS, fall back on most recent decoded frame, even
     * if it is incorrect. */
    final_frame = ffmpeg_double_buffer_frame_fallback_get(anim);
  }

  /* Even with the fallback from above it is possible that the current decode frame is nullptr. In
   * this case skip post-processing and return current image buffer. */
  if (final_frame != nullptr) {
    ffmpeg_postprocess(anim, final_frame, cur_frame_final);
  }

  anim->cur_position = position;

  return cur_frame_final;
}

static void free_anim_ffmpeg(ImBufAnim *anim)
{
  if (anim == nullptr) {
    return;
  }

  if (anim->pCodecCtx) {
    avcodec_free_context(&anim->pCodecCtx);
    avformat_close_input(&anim->pFormatCtx);
    av_packet_free(&anim->cur_packet);

    av_frame_free(&anim->pFrame);
    av_frame_free(&anim->pFrame_backup);
    av_frame_free(&anim->pFrameRGB);
    av_frame_free(&anim->pFrameDeinterlaced);
    BKE_ffmpeg_sws_release_context(anim->img_convert_ctx);
  }
  anim->duration_in_frames = 0;
}

#endif

/**
 * Try to initialize the #anim struct.
 * Returns true on success.
 */
static bool anim_getnew(ImBufAnim *anim)
{
  if (anim == nullptr) {
    /* Nothing to initialize. */
    return false;
  }

  BLI_assert(anim->state == ImBufAnim::State::Uninitialized);

#ifdef WITH_FFMPEG
  free_anim_ffmpeg(anim);
  if (startffmpeg(anim)) {
    anim->state = ImBufAnim::State::Failed;
    return false;
  }
#endif
  anim->state = ImBufAnim::State::Valid;
  return true;
}

ImBuf *IMB_anim_previewframe(ImBufAnim *anim)
{
  ImBuf *ibuf = nullptr;
  int position = 0;

  ibuf = IMB_anim_absolute(anim, 0, IMB_TC_NONE, IMB_PROXY_NONE);
  if (ibuf) {
    IMB_freeImBuf(ibuf);
    position = anim->duration_in_frames / 2;
    ibuf = IMB_anim_absolute(anim, position, IMB_TC_NONE, IMB_PROXY_NONE);

    char value[128];
    IMB_metadata_ensure(&ibuf->metadata);
    SNPRINTF(value, "%i", anim->x);
    IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::Width", value);
    SNPRINTF(value, "%i", anim->y);
    IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::Height", value);
    SNPRINTF(value, "%i", anim->duration_in_frames);
    IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::Frames", value);

#ifdef WITH_FFMPEG
    if (anim->pFormatCtx) {
      AVStream *v_st = anim->pFormatCtx->streams[anim->videoStream];
      AVRational frame_rate = av_guess_frame_rate(anim->pFormatCtx, v_st, nullptr);
      if (frame_rate.num != 0) {
        double duration = anim->duration_in_frames / av_q2d(frame_rate);
        SNPRINTF(value, "%g", av_q2d(frame_rate));
        IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::FPS", value);
        SNPRINTF(value, "%g", duration);
        IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::Duration", value);
        IMB_metadata_set_field(ibuf->metadata, "Thumb::Video::Codec", anim->pCodec->long_name);
      }
    }
#endif
  }
  return ibuf;
}

ImBuf *IMB_anim_absolute(ImBufAnim *anim,
                         int position,
                         IMB_Timecode_Type tc,
                         IMB_Proxy_Size preview_size)
{
  ImBuf *ibuf = nullptr;
  if (anim == nullptr) {
    return nullptr;
  }

  if (preview_size == IMB_PROXY_NONE) {
    if (anim->state == ImBufAnim::State::Uninitialized) {
      if (!anim_getnew(anim)) {
        return nullptr;
      }
    }

    if (position < 0) {
      return nullptr;
    }
    if (position >= anim->duration_in_frames) {
      return nullptr;
    }
  }
  else {
    ImBufAnim *proxy = IMB_anim_open_proxy(anim, preview_size);

    if (proxy) {
      position = IMB_anim_index_get_frame_index(anim, tc, position);

      return IMB_anim_absolute(proxy, position, IMB_TC_NONE, IMB_PROXY_NONE);
    }
  }

#ifdef WITH_FFMPEG
  if (anim->state == ImBufAnim::State::Valid) {
    ibuf = ffmpeg_fetchibuf(anim, position, tc);
    if (ibuf) {
      anim->cur_position = position;
    }
  }
#endif

  if (ibuf) {
    SNPRINTF(ibuf->filepath, "%s.%04d", anim->filepath, anim->cur_position + 1);
  }
  return ibuf;
}

/***/

int IMB_anim_get_duration(ImBufAnim *anim, IMB_Timecode_Type tc)
{
  ImBufAnimIndex *idx;
  if (tc == IMB_TC_NONE) {
    return anim->duration_in_frames;
  }

  idx = IMB_anim_open_index(anim, tc);
  if (!idx) {
    return anim->duration_in_frames;
  }

  return IMB_indexer_get_duration(idx);
}

double IMD_anim_get_offset(ImBufAnim *anim)
{
  return anim->start_offset;
}

bool IMB_anim_get_fps(const ImBufAnim *anim,
                      bool no_av_base,
                      short *r_frs_sec,
                      float *r_frs_sec_base)
{
  double frs_sec_base_double;
  if (anim->frs_sec) {
    if (anim->frs_sec > SHRT_MAX) {
      /* We cannot store original rational in our short/float format,
       * we need to approximate it as best as we can... */
      *r_frs_sec = SHRT_MAX;
      frs_sec_base_double = anim->frs_sec_base * double(SHRT_MAX) / double(anim->frs_sec);
    }
    else {
      *r_frs_sec = anim->frs_sec;
      frs_sec_base_double = anim->frs_sec_base;
    }
#ifdef WITH_FFMPEG
    if (no_av_base) {
      *r_frs_sec_base = float(frs_sec_base_double / AV_TIME_BASE);
    }
    else {
      *r_frs_sec_base = float(frs_sec_base_double);
    }
#else
    UNUSED_VARS(no_av_base);
    *r_frs_sec_base = float(frs_sec_base_double);
#endif
    BLI_assert(*r_frs_sec > 0);
    BLI_assert(*r_frs_sec_base > 0.0f);

    return true;
  }
  return false;
}

int IMB_anim_get_image_width(ImBufAnim *anim)
{
  return anim->x;
}

int IMB_anim_get_image_height(ImBufAnim *anim)
{
  return anim->y;
}
