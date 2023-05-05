/* GStreamer
 * Copyright (C) 2023 Stéphane Cerveau <scerveau@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_H264_ENCODER_H__
#define __GST_H264_ENCODER_H__

#include <gst/codecs/codecs-prelude.h>
#include <gst/codecparsers/gsth264parser.h>
#include <gst/video/video.h>
#include <gst/video/gstvideoencoder.h>

#include "gsth264frame.h"


G_BEGIN_DECLS
#define GST_TYPE_H264_ENCODER            (gst_h264_encoder_get_type())
#define GST_H264_ENCODER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_H264_ENCODER,GstH264Encoder))
#define GST_H264_ENCODER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_H264_ENCODER,GstH264EncoderClass))
#define GST_H264_ENCODER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_H264_ENCODER,GstH264EncoderClass))
#define GST_IS_H264_ENCODER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_H264_ENCODER))
#define GST_IS_H264_ENCODER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_H264_ENCODER))
#define GST_H264_ENCODER_CAST(obj)       ((GstH264Encoder*)obj)
typedef struct _GstH264Encoder GstH264Encoder;
typedef struct _GstH264EncoderClass GstH264EncoderClass;
typedef struct _GstH264EncoderPrivate GstH264EncoderPrivate;

#define MAX_H264_GOP_SIZE  1024

/**
 * GstH264Encoder:
 *
 * The opaque #GstH264Encoder data structure.
 */
struct _GstH264Encoder
{
  /*< private > */
  GstVideoEncoder parent;

  GstVideoCodecState * input_state;

  gint width;
  gint height;

  guint mb_width;
  guint mb_height;

  GstH264Profile profile;

  struct
  {
    /* frames between two IDR [idr, ...., idr) */
    guint32 idr_period;
    /* How may IDRs we have encoded */
    guint32 total_idr_count;
    /* frames between I/P and P frames [I, B, B, .., B, P) */
    guint32 ip_period;
    /* frames between I frames [I, B, B, .., B, P, ..., I), open GOP */
    guint32 i_period;
    /* B frames between I/P and P. */
    guint32 num_bframes;
    /* Use B pyramid structure in the GOP. */
    gboolean b_pyramid;
    /* Level 0 is the simple B not acting as ref. */
    guint32 highest_pyramid_level;
    /* If open GOP, I frames within a GOP. */
    guint32 num_iframes;
    /* A map of all frames types within a GOP. */
    struct
    {
      guint8 slice_type;
      gboolean is_ref;
      guint8 pyramid_level;
      /* Only for b pyramid */
      gint left_ref_poc_diff;
      gint right_ref_poc_diff;
    } frame_types[MAX_H264_GOP_SIZE];
    /* current index in the frames types map. */
    guint cur_frame_index;
    /* Number of ref frames within current GOP. H264's frame num. */
    gint cur_frame_num;
    /* Max frame num within a GOP. */
    guint32 max_frame_num;
    guint32 log2_max_frame_num;
    /* Max poc within a GOP. */
    guint32 max_pic_order_cnt;
    guint32 log2_max_pic_order_cnt;

    /* Total ref frames of list0 and list1. */
    guint32 num_ref_frames;
    guint32 ref_num_list0;
    guint32 ref_num_list1;

    guint num_reorder_frames;
  } gop;

  struct
  {
    guint32 idr_period;
    gint max_quality;
    gint min_quality;
  } prop;

  GQueue ref_list;
  GQueue reorder_list;
  GQueue output_list;

  GstClockTime start_pts;
  GstClockTime frame_duration;
  /* Total frames we handled since reconfig. */
  guint input_frame_count;
  guint output_frame_count;

  /*< private > */
  GstH264EncoderPrivate *priv;
  gpointer padding[GST_PADDING_LARGE];
};

/**
 * GstH264EncoderClass:
 */
struct _GstH264EncoderClass
{
  GstVideoEncoderClass parent_class;
  gboolean (*new_frame)      (GstH264Encoder * encoder,
                            GstVideoCodecFrame * frame);
  gboolean (*reorder_frame)  (GstH264Encoder * base,
                              GstVideoCodecFrame * frame,
                              gboolean bump_all,
                              GstVideoCodecFrame ** out_frame);
  /**
   * GstH264EncoderClass::encode_frame:
   * @encoder: a #GstH264Encoder
   * @frame: a #GstH264Frame
   *
   * Provide the frame to be encoded with the encode parameters (to be defined)
   */
  GstFlowReturn (*encode_frame) (GstH264Encoder * encoder,
    GstVideoCodecFrame * frame, gboolean last);

  void     (*prepare_output) (GstH264Encoder * encoder,
                              GstVideoCodecFrame * frame);

  gboolean (*set_format)     (GstH264Encoder * encoder, GstVideoCodecState * state);

  gboolean (*max_num_reference) (GstH264Encoder * encoder, guint32 * list0, guint32 * list1);
  /*< private > */
  gpointer padding[GST_PADDING_LARGE];
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstH264Encoder, gst_object_unref)
     GST_CODECS_API GType gst_h264_encoder_get_type (void);


GST_CODECS_API
GstH264Level    gst_h264_encoder_get_level_limit      (GstH264Encoder * encoder);

GST_CODECS_API
GstH264Profile  gst_h264_encoder_get_profile_from_str (const gchar * profile);

GST_CODECS_API
const gchar *   gst_h264_encoder_slice_type_name      (GstH264SliceType type);

G_END_DECLS
#endif /* __GST_H264_ENCODER_H__ */
