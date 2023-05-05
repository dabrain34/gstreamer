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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gsth264encoder.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/base/base.h>

GST_DEBUG_CATEGORY (gst_h264_encoder_debug);
#define GST_CAT_DEFAULT gst_h264_encoder_debug

#define H264ENC_DEFAULT_IDR_PERIOD	30

#define H264_MAX_QUALITY				63
#define H264_MIN_QUALITY				0

#define H264_DEFAULT_BITRATE			100000



typedef struct _GstH264LevelLimits
{
  const gchar *name;
  GstH264Level level_idc;
  guint32 maxMBPS;
  guint32 maxFS;
  guint32 maxDpbMbs;
  guint32 maxBR;
  guint32 maxCPB;
  guint32 minCR;
} GstH264LevelLimit;

static const GstH264LevelLimit _h264_level_limits[] = {
  /* level   idc   MaxMBPS   MaxFS   MaxDpbMbs  MaxBR   MaxCPB  MinCr */
  {"1", 10, 1485, 99, 396, 64, 175, 2},
  {"1b", 9, 1485, 99, 396, 128, 350, 2},
  {"1.1", 11, 3000, 396, 900, 192, 500, 2},
  {"1.2", 12, 6000, 396, 2376, 384, 1000, 2},
  {"1.3", 13, 11880, 396, 2376, 768, 2000, 2},
  {"2", 20, 11880, 396, 2376, 2000, 2000, 2},
  {"2.1", 21, 19800, 792, 4752, 4000, 4000, 2},
  {"2.2", 22, 20250, 1620, 8100, 4000, 4000, 2},
  {"3", 30, 40500, 1620, 8100, 10000, 10000, 2},
  {"3.1", 31, 108000, 3600, 18000, 14000, 14000, 4},
  {"3.2", 32, 216000, 5120, 20480, 20000, 20000, 4},
  {"4", 40, 245760, 8192, 32768, 20000, 25000, 4},
  {"4.1", 41, 245760, 8192, 32768, 50000, 62500, 2},
  {"4.2", 42, 522240, 8704, 34816, 50000, 62500, 2},
  {"5", 50, 589824, 22080, 110400, 135000, 135000, 2},
  {"5.1", 51, 983040, 36864, 184320, 240000, 240000, 2},
  {"5.2", 52, 2073600, 36864, 184320, 240000, 240000, 2},
  {"6", 60, 4177920, 139264, 696320, 240000, 240000, 2},
  {"6.1", 61, 8355840, 139264, 696320, 480000, 480000, 2},
  {"6.2", 62, 16711680, 139264, 696320, 800000, 800000, 2},
};

typedef struct
{
  const gchar *name;
  GstH264Profile profile;
} H264ProfileMapping;

static const H264ProfileMapping h264_profiles[] = {
  {"baseline", GST_H264_PROFILE_BASELINE},
  {"main", GST_H264_PROFILE_MAIN},
  {"high", GST_H264_PROFILE_HIGH},
  {"high-10", GST_H264_PROFILE_HIGH10},
  {"high-4:2:2", GST_H264_PROFILE_HIGH_422},
  {"high-4:4:4", GST_H264_PROFILE_HIGH_444},
  {"multiview-high", GST_H264_PROFILE_MULTIVIEW_HIGH},
  {"stereo-high", GST_H264_PROFILE_STEREO_HIGH},
  {"scalable-baseline", GST_H264_PROFILE_SCALABLE_BASELINE},
  {"scalable-high", GST_H264_PROFILE_SCALABLE_HIGH},
};

enum
{
  PROP_0,
  PROP_IDR_PERIOD,
  PROP_MAX_QUALITY,
  PROP_MIN_QUALITY,
  PROP_BITRATE,
};

struct _GstH264EncoderPrivate
{
  gint keyframe_interval;

  guint32 last_keyframe;

  guint64 targeted_bitrate;

  gint current_quality;
  guint64 used_bytes;
  guint64 nb_frames;

  struct
  {
    guint target_usage;
    guint32 rc_ctrl_mode;

    guint32 min_qp;
    guint32 max_qp;
    guint32 qp_i;
    guint32 qp_p;
    guint32 qp_b;
    /* macroblock bitrate control */
    guint32 mbbrc;
    guint target_bitrate;
    guint target_percentage;
    guint max_bitrate;
    /* bitrate (bits) */
    guint max_bitrate_bits;
    guint target_bitrate_bits;
    /* length of CPB buffer */
    guint cpb_size;
    /* length of CPB buffer (bits) */
    guint cpb_length_bits;
  } rc;
};

#define parent_class gst_h264_encoder_parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstH264Encoder, gst_h264_encoder,
    GST_TYPE_VIDEO_ENCODER,
    G_ADD_PRIVATE (GstH264Encoder);
    GST_DEBUG_CATEGORY_INIT (gst_h264_encoder_debug, "h264encoder", 0,
        "H264 Video Encoder"));

/* Get log2_max_frame_num_minus4, log2_max_pic_order_cnt_lsb_minus4
 * value, shall be in the range of 0 to 12, inclusive. */
static guint
_get_log2_max_num (guint num)
{
  guint ret = 0;

  while (num) {
    ++ret;
    num >>= 1;
  }

  /* shall be in the range of 0+4 to 12+4, inclusive. */
  if (ret < 4) {
    ret = 4;
  } else if (ret > 16) {
    ret = 16;
  }
  return ret;
}

// TODO: Move this code to GStreamer base class (codec-utils, codec-parsers)
GstH264Profile
gst_h264_encoder_get_profile_from_str (const gchar * profile)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (h264_profiles); i++) {
    if (g_str_equal (profile, h264_profiles[i].name))
      return h264_profiles[i].profile;
  }

  return -1;
}

static void
_generate_gop_structure (GstH264Encoder * self)
{
  /* If not set, generate a idr every second */
  if (self->gop.idr_period == 0) {
    self->gop.idr_period = (GST_VIDEO_INFO_FPS_N (&self->input_state->info)
        + GST_VIDEO_INFO_FPS_D (&self->input_state->info) - 1) /
        GST_VIDEO_INFO_FPS_D (&self->input_state->info);
  }

  /* Do not use a too huge GOP size. */
  if (self->gop.idr_period > 1024) {
    self->gop.idr_period = 1024;
    GST_INFO_OBJECT (self, "Lowering the GOP size to %d", self->gop.idr_period);
  }
  // FIXME: add b-frames support

  self->gop.log2_max_frame_num = _get_log2_max_num (self->gop.idr_period);
  self->gop.max_frame_num = (1 << self->gop.log2_max_frame_num);
  self->gop.log2_max_pic_order_cnt = self->gop.log2_max_frame_num + 1;
  self->gop.max_pic_order_cnt = (1 << self->gop.log2_max_pic_order_cnt);


}

static void
gst_h264_encoder_init (GstH264Encoder * self)
{
  self->priv = gst_h264_encoder_get_instance_private (self);
}

static void
gst_h264_encoder_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_h264_encoder_start (GstVideoEncoder * encoder)
{
  GstH264Encoder *self = GST_H264_ENCODER (encoder);
  GstH264EncoderPrivate *priv = self->priv;

  priv->last_keyframe = 0;
  priv->current_quality = self->prop.min_quality;
  priv->used_bytes = 0;
  priv->nb_frames = 0;

  self->width = 0;
  self->height = 0;

  return TRUE;
}

static gboolean
gst_h264_encoder_stop (GstVideoEncoder * encoder)
{
  return TRUE;
}

static gboolean
gst_h264_encoder_reset (GstVideoEncoder * encoder, gboolean hard)
{
  GstH264Encoder *self = GST_H264_ENCODER (encoder);

  self->gop.idr_period = self->prop.idr_period;
  self->gop.total_idr_count = 0;
  self->gop.num_iframes = 0;
  self->gop.num_ref_frames = 0;
  self->gop.cur_frame_index = 0;
  self->gop.max_pic_order_cnt = 0;

  return TRUE;
}

static gboolean
gst_h264_encoder_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstH264Encoder *self = GST_H264_ENCODER (encoder);
  GstH264EncoderClass *base_class = GST_H264_ENCODER_GET_CLASS (self);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  self->input_state = gst_video_codec_state_ref (state);

  self->width = GST_VIDEO_INFO_WIDTH (&self->input_state->info);
  self->height = GST_VIDEO_INFO_HEIGHT (&self->input_state->info);

  self->mb_width = GST_ROUND_UP_16 (self->width) / 16;
  self->mb_height = GST_ROUND_UP_16 (self->height) / 16;

  if (base_class->set_format) {
    base_class->set_format (self, state);
  }

  self->frame_duration =
      gst_util_uint64_scale (GST_SECOND, self->input_state->info.fps_d,
      self->input_state->info.fps_n);

  _generate_gop_structure (self);

  return TRUE;
}

static GstFlowReturn
gst_h264_encoder_set_quality (GstH264Encoder * self, GstH264Frame * h264_frame)
{
  GstH264EncoderPrivate *priv = self->priv;
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (self);
  GstVideoCodecState *output_state =
      gst_video_encoder_get_output_state (encoder);
  gint qp = priv->current_quality;
  guint64 bitrate = 0;
  guint fps_n = 30, fps_d = 1;

  if (output_state == NULL || !priv->nb_frames)
    return qp;

  if (GST_VIDEO_INFO_FPS_N (&output_state->info) != 0) {
    fps_n = GST_VIDEO_INFO_FPS_N (&output_state->info);
    fps_d = GST_VIDEO_INFO_FPS_D (&output_state->info);
  }
  gst_video_codec_state_unref (output_state);

  bitrate = (priv->used_bytes * 8 * fps_n) / (priv->nb_frames * fps_d);
  if (bitrate > priv->targeted_bitrate) {
    qp++;
  }

  if (bitrate < priv->targeted_bitrate) {
    qp--;
  }

  if (qp > self->prop.max_quality)
    qp = self->prop.max_quality;
  if (qp < self->prop.min_quality)
    qp = self->prop.min_quality;

  h264_frame->quality = qp;

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_h264_encoder_set_frame_type (GstH264Encoder * self,
    GstH264Frame * h264_frame)
{
  GstH264EncoderPrivate *priv = self->priv;
  GstVideoCodecFrame *frame = h264_frame->frame;

  if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame)) {
    h264_frame->type = GST_H264_FRAME_KEY;
    return GST_FLOW_OK;
  }

  if ((frame->system_frame_number - priv->last_keyframe) >
      priv->keyframe_interval || frame->system_frame_number == 0) {
    /* Generate a keyframe */
    GST_DEBUG_OBJECT (self, "Generate a keyframe");
    h264_frame->type = GST_H264_FRAME_KEY;
    return GST_FLOW_OK;
  }

  /* Generate a interframe */
  GST_DEBUG_OBJECT (self, "Generate a interframe");
  h264_frame->type = GST_H264_FRAME_INTER;
  return GST_FLOW_OK;
}

static void
gst_h264_encoder_mark_frame (GstH264Encoder * self, GstH264Frame * h264_frame)
{
  GstVideoCodecFrame *frame = h264_frame->frame;
  GstH264EncoderPrivate *priv = self->priv;

  if (h264_frame->type == GST_H264_FRAME_KEY)
    priv->last_keyframe = frame->system_frame_number;

  priv->current_quality = h264_frame->quality;
  //priv->used_bytes += gst_buffer_get_size (frame->output_buffer);
  priv->nb_frames++;
}

static GstFlowReturn
_push_buffer_to_downstream (GstH264Encoder * self, GstVideoCodecFrame * frame)
{
  GstH264EncoderClass *base_class = GST_H264_ENCODER_GET_CLASS (self);

  if (base_class->prepare_output)
    base_class->prepare_output (self, frame);

  GST_LOG_OBJECT (self, "Push to downstream: frame system_frame_number: %d,"
      " pts: %" GST_TIME_FORMAT ", dts: %" GST_TIME_FORMAT
      " duration: %" GST_TIME_FORMAT ", buffer size: %" G_GSIZE_FORMAT,
      frame->system_frame_number, GST_TIME_ARGS (frame->pts),
      GST_TIME_ARGS (frame->dts), GST_TIME_ARGS (frame->duration),
      gst_buffer_get_size (frame->output_buffer));

  return gst_video_encoder_finish_frame (GST_VIDEO_ENCODER (self), frame);
}

static GstFlowReturn
_push_out_one_buffer (GstH264Encoder * self)
{
  GstVideoCodecFrame *frame_out;
  GstFlowReturn ret;
  guint32 system_frame_number;

  frame_out = g_queue_pop_head (&self->output_list);
  gst_video_codec_frame_unref (frame_out);

  system_frame_number = frame_out->system_frame_number;

  ret = _push_buffer_to_downstream (self, frame_out);

  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "fails to push one buffer, system_frame_number "
        "%d: %s", system_frame_number, gst_flow_get_name (ret));
  }

  return ret;
}

static GstFlowReturn
gst_h264_encoder_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstH264Encoder *self = GST_H264_ENCODER (encoder);
  GstH264EncoderClass *klass = GST_H264_ENCODER_GET_CLASS (self);
  GstFlowReturn ret = GST_FLOW_OK;
  GstH264Frame *h264_frame = gst_h264_frame_new (frame);

  ret = gst_h264_encoder_set_frame_type (self, h264_frame);
  if (ret != GST_FLOW_OK)
    return ret;

  ret = gst_h264_encoder_set_quality (self, h264_frame);
  if (ret != GST_FLOW_OK)
    return ret;

  if (!klass->new_frame (self, frame))
    goto error_new_frame;

  /* TODO: add encoding parameters management here
   * for now just send the frame to encode */
  if (klass->encode_frame) {
    ret = klass->encode_frame (self, h264_frame->frame, FALSE);
    if (ret == GST_FLOW_OK)
      gst_h264_encoder_mark_frame (self, h264_frame);
  }

  while (g_queue_get_length (&self->output_list) > 0)
    ret = _push_out_one_buffer (self);

  gst_h264_frame_unref (h264_frame);

  return ret;
error_new_frame:
  {
    GST_ELEMENT_ERROR (encoder, STREAM, ENCODE,
        ("Failed to create the input frame."), (NULL));
    gst_clear_buffer (&frame->output_buffer);
    gst_video_encoder_finish_frame (encoder, frame);
    return GST_FLOW_ERROR;
  }
}

static void
gst_h264_encoder_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstH264Encoder *self = GST_H264_ENCODER (object);
  GstH264EncoderPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_IDR_PERIOD:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->prop.idr_period);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_MAX_QUALITY:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->prop.max_quality);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_MIN_QUALITY:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->prop.min_quality);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_BITRATE:
      GST_OBJECT_LOCK (self);
      g_value_set_uint64 (value, priv->targeted_bitrate);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_h264_encoder_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH264Encoder *self = GST_H264_ENCODER (object);
  GstH264EncoderPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_IDR_PERIOD:
      GST_OBJECT_LOCK (self);
      self->prop.idr_period = g_value_get_int (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_MAX_QUALITY:
      GST_OBJECT_LOCK (self);
      self->prop.max_quality = g_value_get_int (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_MIN_QUALITY:
      GST_OBJECT_LOCK (self);
      self->prop.min_quality = g_value_get_int (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_BITRATE:
      GST_OBJECT_LOCK (self);
      priv->targeted_bitrate = g_value_get_uint64 (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
gst_h264_encoder_class_init (GstH264EncoderClass * klass)
{
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = GST_DEBUG_FUNCPTR (gst_h264_encoder_finalize);
  object_class->get_property = gst_h264_encoder_get_property;
  object_class->set_property = gst_h264_encoder_set_property;

  encoder_class->start = GST_DEBUG_FUNCPTR (gst_h264_encoder_start);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_h264_encoder_stop);
  encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_h264_encoder_set_format);
  encoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_h264_encoder_handle_frame);
  encoder_class->reset = GST_DEBUG_FUNCPTR (gst_h264_encoder_reset);
  /**
   * GstH264Encoder:idr-period:
   *
   *
   * Since: 1.2x
   */
  g_object_class_install_property (object_class, PROP_IDR_PERIOD,
      g_param_spec_int ("idr-period", "IDR period",
          "Interval between keyframes",
          0, G_MAXINT, H264ENC_DEFAULT_IDR_PERIOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
  /**
   * GstH264Encoder:max-quality:
   *
   *
   * Since: 1.24
   */
  g_object_class_install_property (object_class, PROP_MAX_QUALITY,
      g_param_spec_int ("max-quality", "Max Quality Level",
          "Set upper quality limit (lower number equates to higher quality but more bits)",
          H264_MIN_QUALITY, H264_MAX_QUALITY, H264_MAX_QUALITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
   /**
   * GstH264Encoder:min-quality:
   *
   *
   * Since: 1.24
   */
  g_object_class_install_property (object_class, PROP_MIN_QUALITY,
      g_param_spec_int ("min-quality", "Min Quality Level",
          "Set lower quality limit (lower number equates to higher quality but more bits)",
          H264_MIN_QUALITY, H264_MAX_QUALITY, H264_MIN_QUALITY,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));

   /**
   * GstH264Encoder:bitrate:
   *
   *
   * Since: 1.24
   */
  g_object_class_install_property (object_class, PROP_BITRATE,
      g_param_spec_uint64 ("bitrate", "Targeted bitrate",
          "Set bitrate target",
          0, UINT_MAX, H264_DEFAULT_BITRATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
}

static guint
_get_h264_cpb_nal_factor (GstH264Profile profile)
{
  guint f;

  /* Table A-2 */
  switch (profile) {
    case GST_H264_PROFILE_HIGH:
      f = 1500;
      break;
    case GST_H264_PROFILE_BASELINE:
    case GST_H264_PROFILE_MAIN:
      f = 1200;
      break;
    case GST_H264_PROFILE_MULTIVIEW_HIGH:
    case GST_H264_PROFILE_STEREO_HIGH:
      f = 1500;                 /* H.10.2.1 (r) */
      break;
    default:
      g_assert_not_reached ();
      f = 1200;
      break;
  }
  return f;
}

//TODO: Need to handle the rate control and the gop properly
GstH264Level
gst_h264_encoder_get_level_limit (GstH264Encoder * self)
{
  GstH264EncoderPrivate *priv = self->priv;
  const guint cpb_factor = _get_h264_cpb_nal_factor (self->profile);
  guint i, picSizeMbs, maxDpbMbs, maxMBPS;

  picSizeMbs = self->mb_width * self->mb_height;
  maxDpbMbs = picSizeMbs * (self->gop.num_ref_frames + 1);
  maxMBPS = gst_util_uint64_scale_int_ceil (picSizeMbs,
      GST_VIDEO_INFO_FPS_N (&self->input_state->info),
      GST_VIDEO_INFO_FPS_D (&self->input_state->info));

  for (i = 0; i < G_N_ELEMENTS (_h264_level_limits); i++) {
    const GstH264LevelLimit *level = &_h264_level_limits[i];
    if (picSizeMbs <= level->maxFS && maxDpbMbs <= level->maxDpbMbs
        && maxMBPS <= level->maxMBPS && (!priv->rc.max_bitrate_bits
            || priv->rc.max_bitrate_bits <= (level->maxBR * 1000 * cpb_factor))
        && (!priv->rc.cpb_length_bits
            || priv->rc.cpb_length_bits <=
            (level->maxCPB * 1000 * cpb_factor))) {

      return level->level_idc;
    }
  }

  GST_ERROR_OBJECT (self,
      "failed to find a suitable level matching codec config");
  return -1;
}
