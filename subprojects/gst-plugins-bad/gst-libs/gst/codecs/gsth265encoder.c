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

#include "gsth265encoder.h"

#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/base/base.h>

GST_DEBUG_CATEGORY (gst_h265_encoder_debug);
#define GST_CAT_DEFAULT gst_h265_encoder_debug

#define H265ENC_DEFAULT_IDR_PERIOD	30

#define H265_MAX_QUALITY				63
#define H265_MIN_QUALITY				0

#define H265_DEFAULT_BITRATE			100000



typedef struct _GstH265LevelLimits
{
  const gchar *level_name;
  guint8 level_idc;
  guint32 MaxLumaPs;
  guint32 MaxCPBTierMain;
  guint32 MaxCPBTierHigh;
  guint32 MaxSliceSegPic;
  guint32 MaxTileRows;
  guint32 MaxTileColumns;
  guint32 MaxLumaSr;
  guint32 MaxBRTierMain;
  guint32 MaxBRTierHigh;
  guint32 MinCr;
} GstH265LevelLimit;

static const GstH265LevelLimit _h265_level_limits[] = {
  /* level   idc   MaxMBPS   MaxFS   MaxDpbMbs  MaxBR   MaxCPB  MinCr */
  {"1", GST_H265_LEVEL_L1, 36864, 350, 0, 16, 1, 1, 552960, 128, 0, 2},
  {"2", GST_H265_LEVEL_L2, 122880, 1500, 0, 16, 1, 1, 3686400, 1500, 0, 2},
  {"2.1", GST_H265_LEVEL_L2_1, 245760, 3000, 0, 20, 1, 1, 7372800, 3000, 0, 2},
  {"3", GST_H265_LEVEL_L3, 552960, 6000, 0, 30, 2, 2, 16588800, 6000, 0, 2},
  {"3.1", GST_H265_LEVEL_L3_1, 983040, 10000, 0, 40, 3, 3, 33177600, 10000, 0,
      2},
  {"4", GST_H265_LEVEL_L4, 2228224, 12000, 30000, 75, 5, 5, 66846720, 12000,
      30000, 4},
  {"4.1", GST_H265_LEVEL_L4_1, 2228224, 20000, 50000, 75, 5, 5, 133693440,
      20000, 50000, 4},
  {"5", GST_H265_LEVEL_L5, 8912896, 25000, 100000, 200, 11, 10, 267386880,
      25000, 100000, 6},
  {"5.1", GST_H265_LEVEL_L5_1, 8912896, 40000, 160000, 200, 11, 10, 534773760,
        40000, 160000,
      8},
  {"5.2", GST_H265_LEVEL_L5_2, 8912896, 60000, 240000, 200, 11, 10, 1069547520,
        60000, 240000,
      8},
  {"6", GST_H265_LEVEL_L6, 35651584, 60000, 240000, 600, 22, 20, 1069547520,
        60000, 240000,
      8},
  {"6.1", GST_H265_LEVEL_L6_1, 35651584, 120000, 480000, 600, 22, 20,
        2139095040, 120000,
      480000, 8},
  {"6.2", GST_H265_LEVEL_L6_2, 35651584, 240000, 800000, 600, 22, 20,
        4278190080, 240000,
      800000, 6},
};

typedef struct
{
  GstH265Profile profile;
  const gchar *name;
} H265ProfileMapping;

static const H265ProfileMapping h265_profiles[] = {
  {GST_H265_PROFILE_MAIN, "main"},
  {GST_H265_PROFILE_MAIN_10, "main-10"},
  {GST_H265_PROFILE_MAIN_STILL_PICTURE, "main-still-picture"},
  {GST_H265_PROFILE_MONOCHROME, "monochrome"},
  {GST_H265_PROFILE_MONOCHROME_12, "monochrome-12"},
  {GST_H265_PROFILE_MONOCHROME_16, "monochrome-16"},
  {GST_H265_PROFILE_MAIN_12, "main-12"},
  {GST_H265_PROFILE_MAIN_422_10, "main-422-10"},
  {GST_H265_PROFILE_MAIN_422_12, "main-422-12"},
  {GST_H265_PROFILE_MAIN_444, "main-444"},
  {GST_H265_PROFILE_MAIN_444_10, "main-444-10"},
  {GST_H265_PROFILE_MAIN_444_12, "main-444-12"},
  {GST_H265_PROFILE_MAIN_INTRA, "main-intra"},
  {GST_H265_PROFILE_MAIN_10_INTRA, "main-10-intra"},
  {GST_H265_PROFILE_MAIN_12_INTRA, "main-12-intra"},
  {GST_H265_PROFILE_MAIN_422_10_INTRA, "main-422-10-intra"},
  {GST_H265_PROFILE_MAIN_422_12_INTRA, "main-422-12-intra"},
  {GST_H265_PROFILE_MAIN_444_INTRA, "main-444-intra"},
  {GST_H265_PROFILE_MAIN_444_10_INTRA, "main-444-10-intra"},
  {GST_H265_PROFILE_MAIN_444_12_INTRA, "main-444-12-intra"},
  {GST_H265_PROFILE_MAIN_444_16_INTRA, "main-444-16-intra"},
  {GST_H265_PROFILE_MAIN_444_STILL_PICTURE, "main-444-still-picture"},
  {GST_H265_PROFILE_MAIN_444_16_STILL_PICTURE, "main-444-16-still-picture"},
  {GST_H265_PROFILE_MONOCHROME_10, "monochrome-10"},
  {GST_H265_PROFILE_HIGH_THROUGHPUT_444, "high-throughput-444"},
  {GST_H265_PROFILE_HIGH_THROUGHPUT_444_10, "high-throughput-444-10"},
  {GST_H265_PROFILE_HIGH_THROUGHPUT_444_14, "high-throughput-444-14"},
  {GST_H265_PROFILE_HIGH_THROUGHPUT_444_16_INTRA,
      "high-throughput-444-16-intra"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_MAIN, "screen-extended-main"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_MAIN_10, "screen-extended-main-10"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_MAIN_444, "screen-extended-main-444"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_MAIN_444_10, "screen-extended-main-444-10"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_HIGH_THROUGHPUT_444,
      "screen-extended-high-throughput-444"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_HIGH_THROUGHPUT_444_10,
      "screen-extended-high-throughput-444-10"},
  {GST_H265_PROFILE_SCREEN_EXTENDED_HIGH_THROUGHPUT_444_14,
      "screen-extended-high-throughput-444-14"},
  {GST_H265_PROFILE_MULTIVIEW_MAIN, "multiview-main"},
  {GST_H265_PROFILE_SCALABLE_MAIN, "scalable-main"},
  {GST_H265_PROFILE_SCALABLE_MAIN_10, "scalable-main-10"},
  {GST_H265_PROFILE_SCALABLE_MONOCHROME, "scalable-monochrome"},
  {GST_H265_PROFILE_SCALABLE_MONOCHROME_12, "scalable-monochrome-12"},
  {GST_H265_PROFILE_SCALABLE_MONOCHROME_16, "scalable-monochrome-16"},
  {GST_H265_PROFILE_SCALABLE_MAIN_444, "scalable-main-444"},
  {GST_H265_PROFILE_3D_MAIN, "3d-main"},
};

enum
{
  PROP_0,
  PROP_IDR_PERIOD,
  PROP_BITRATE,
};

struct _GstH265EncoderPrivate
{
  guint64 targeted_bitrate;

  gint current_quality;
  guint64 used_bytes;
  guint64 nb_frames;

  struct
  {
    guint32 min_qp;
    guint32 max_qp;
    guint32 qp_i;
    guint32 qp_p;
    guint32 qp_b;
    guint max_bitrate;
    /* bitrate (bits) */
    guint max_bitrate_bits;
    /* length of CPB buffer (bits) */
    guint cpb_length_bits;
  } rc;
};

#define parent_class gst_h265_encoder_parent_class
G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstH265Encoder, gst_h265_encoder,
    GST_TYPE_VIDEO_ENCODER,
    G_ADD_PRIVATE (GstH265Encoder);
    GST_DEBUG_CATEGORY_INIT (gst_h265_encoder_debug, "h265encoder", 0,
        "H265 Video Encoder"));

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
GstH265Profile
gst_h265_encoder_get_profile_from_str (const gchar * profile)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (h265_profiles); i++) {
    if (g_str_equal (profile, h265_profiles[i].name))
      return h265_profiles[i].profile;
  }

  return -1;
}

const gchar *
gst_h265_encoder_get_profile_name (GstH265Profile profile)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (h265_profiles); i++) {
    if (profile == h265_profiles[i].profile)
      return h265_profiles[i].name;
  }

  return "undefined";
}

struct PyramidInfo
{
  guint level;
  gint left_ref_poc_diff;
  gint right_ref_poc_diff;
};

static void
_set_pyramid_info (struct PyramidInfo *info, guint len,
    guint current_level, guint highest_level)
{
  guint index;

  g_assert (len >= 1);

  if (current_level == highest_level || len == 1) {
    for (index = 0; index < len; index++) {
      info[index].level = current_level;
      info[index].left_ref_poc_diff = (index + 1) * -2;
      info[index].right_ref_poc_diff = (len - index) * 2;
    }

    return;
  }

  index = len / 2;
  info[index].level = current_level;
  info[index].left_ref_poc_diff = (index + 1) * -2;
  info[index].right_ref_poc_diff = (len - index) * 2;

  current_level++;

  if (index > 0)
    _set_pyramid_info (info, index, current_level, highest_level);

  if (index + 1 < len)
    _set_pyramid_info (&info[index + 1], len - (index + 1),
        current_level, highest_level);
}

static void
_create_gop_frame_types (GstH265Encoder * self)
{
  guint i;
  guint i_frames = self->gop.num_iframes;
  struct PyramidInfo pyramid_info[31] = { 0, };

  if (self->gop.highest_pyramid_level > 0) {
    g_assert (self->gop.num_bframes > 0);
    _set_pyramid_info (pyramid_info, self->gop.num_bframes,
        0, self->gop.highest_pyramid_level);
  }

  g_assert (self->gop.idr_period <= MAX_H265_GOP_SIZE);
  for (i = 0; i < self->gop.idr_period; i++) {
    if (i == 0) {
      self->gop.frame_types[i].slice_type = GST_H265_I_SLICE;
      self->gop.frame_types[i].is_ref = TRUE;
      continue;
    }

    /* Intra only stream. */
    if (self->gop.ip_period == 0) {
      self->gop.frame_types[i].slice_type = GST_H265_I_SLICE;
      self->gop.frame_types[i].is_ref = FALSE;
      continue;
    }

    if (i % self->gop.ip_period) {
      guint pyramid_index =
          i % self->gop.ip_period - 1 /* The first P or IDR */ ;

      self->gop.frame_types[i].slice_type = GST_H265_B_SLICE;
      self->gop.frame_types[i].pyramid_level =
          pyramid_info[pyramid_index].level;
      self->gop.frame_types[i].is_ref =
          (self->gop.frame_types[i].pyramid_level <
          self->gop.highest_pyramid_level);
      self->gop.frame_types[i].left_ref_poc_diff =
          pyramid_info[pyramid_index].left_ref_poc_diff;
      self->gop.frame_types[i].right_ref_poc_diff =
          pyramid_info[pyramid_index].right_ref_poc_diff;
      continue;
    }

    if (self->gop.i_period && i % self->gop.i_period == 0 && i_frames > 0) {
      /* Replace P with I. */
      self->gop.frame_types[i].slice_type = GST_H265_I_SLICE;
      self->gop.frame_types[i].is_ref = TRUE;
      i_frames--;
      continue;
    }

    self->gop.frame_types[i].slice_type = GST_H265_P_SLICE;
    self->gop.frame_types[i].is_ref = TRUE;
  }

  /* Force the last one to be a P */
  if (self->gop.idr_period > 1 && self->gop.ip_period > 0) {
    self->gop.frame_types[self->gop.idr_period - 1].slice_type =
        GST_H265_P_SLICE;
    self->gop.frame_types[self->gop.idr_period - 1].is_ref = TRUE;
  }
}

const gchar *
gst_h265_encoder_slice_type_name (GstH265SliceType type)
{
  switch (type) {
    case GST_H265_P_SLICE:
      return "P";
    case GST_H265_B_SLICE:
      return "B";
    case GST_H265_I_SLICE:
      return "I";
    default:
      g_assert_not_reached ();
  }

  return NULL;
}

static void
_print_gop_structure (GstH265Encoder * self)
{
#ifndef GST_DISABLE_GST_DEBUG
  GString *str;
  gint i;

  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) < GST_LEVEL_INFO)
    return;

  str = g_string_new (NULL);

  g_string_append_printf (str, "[ ");

  for (i = 0; i < self->gop.idr_period; i++) {
    if (i == 0) {
      g_string_append_printf (str, "IDR");
      continue;
    } else {
      g_string_append_printf (str, ", ");
    }

    g_string_append_printf (str, "%s",
        gst_h265_encoder_slice_type_name (self->gop.frame_types[i].slice_type));

    if (self->gop.b_pyramid
        && self->gop.frame_types[i].slice_type == GST_H265_B_SLICE) {
      g_string_append_printf (str, "<L%d (%d, %d)>",
          self->gop.frame_types[i].pyramid_level,
          self->gop.frame_types[i].left_ref_poc_diff,
          self->gop.frame_types[i].right_ref_poc_diff);
    }

    if (self->gop.frame_types[i].is_ref) {
      g_string_append_printf (str, "(ref)");
    }

  }

  g_string_append_printf (str, " ]");

  GST_INFO_OBJECT (self, "GOP size: %d, forward reference %d, backward"
      " reference %d, GOP structure: %s", self->gop.idr_period,
      self->gop.ref_num_list0, self->gop.ref_num_list1, str->str);

  g_string_free (str, TRUE);
#endif
}

static gboolean
_calculate_tier_level (GstH265Encoder * self)
{
  GstH265EncoderPrivate *priv = self->priv;
  guint i, PicSizeInSamplesY, LumaSr;
  guint32 tier_max_bitrate;

  PicSizeInSamplesY = self->luma_width * self->luma_height;
  LumaSr = gst_util_uint64_scale_int_ceil (PicSizeInSamplesY,
      GST_VIDEO_INFO_FPS_N (&self->input_state->info),
      GST_VIDEO_INFO_FPS_D (&self->input_state->info));

  for (i = 0; i < G_N_ELEMENTS (_h265_level_limits); i++) {
    const GstH265LevelLimit *const limits = &_h265_level_limits[i];

    /* Choose level by luma picture size and luma sample rate */
    if (PicSizeInSamplesY <= limits->MaxLumaPs && LumaSr <= limits->MaxLumaSr)
      break;
  }

  if (i == G_N_ELEMENTS (_h265_level_limits))
    goto error_unsupported_level;

  self->level_idc = _h265_level_limits[i].level_idc;
  self->level_str = _h265_level_limits[i].level_name;
  self->min_cr = _h265_level_limits[i].MinCr;


  if (_h265_level_limits[i].MaxBRTierHigh == 0 ||
      priv->rc.max_bitrate <= _h265_level_limits[i].MaxBRTierMain) {
    self->tier_flag = FALSE;
  } else {
    self->tier_flag = TRUE;
  }


  tier_max_bitrate = self->tier_flag ? _h265_level_limits[i].MaxBRTierHigh :
      _h265_level_limits[i].MaxBRTierMain;

  if (priv->rc.max_bitrate > tier_max_bitrate) {
    GST_INFO_OBJECT (self, "The max bitrate of the stream is %u kbps, still"
        " larger than %s profile %s level %s tier's max bit rate %d kbps",
        priv->rc.max_bitrate, gst_h265_encoder_get_profile_name (self->profile),
        _h265_level_limits[i].level_name,
        (self->tier_flag ? "high" : "main"), tier_max_bitrate);
  }

  GST_DEBUG_OBJECT (self, "profile: %s, level: %s, tier :%s, MinCr: %d",
      gst_h265_encoder_get_profile_name (self->profile),
      _h265_level_limits[i].level_name, (self->tier_flag ? "high" : "main"),
      self->min_cr);

  return TRUE;

error_unsupported_level:
  {
    GST_ERROR_OBJECT (self,
        "failed to find a suitable level matching codec config");
    return FALSE;
  }
}

static void
_generate_gop_structure (GstH265Encoder * self)
{
  GstH265EncoderClass *base_class = GST_H265_ENCODER_GET_CLASS (self);
  guint32 list0, list1, gop_ref_num;
  gint32 p_frames;
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

  if (self->gop.idr_period > 8) {
    if (self->gop.num_bframes > (self->gop.idr_period - 1) / 2) {
      self->gop.num_bframes = (self->gop.idr_period - 1) / 2;
      GST_INFO_OBJECT (self, "Lowering the number of num_bframes to %d",
          self->gop.num_bframes);
    }
  } else {
    /* beign and end should be ref */
    if (self->gop.num_bframes > self->gop.idr_period - 1 - 1) {
      if (self->gop.idr_period > 1) {
        self->gop.num_bframes = self->gop.idr_period - 1 - 1;
      } else {
        self->gop.num_bframes = 0;
      }
      GST_INFO_OBJECT (self, "Lowering the number of num_bframes to %d",
          self->gop.num_bframes);
    }
  }

  if (!base_class->max_num_reference
      || !base_class->max_num_reference (self, &list0, &list1)) {
    GST_INFO_OBJECT (self, "Failed to get the max num reference");
    list0 = 1;
    list1 = 0;
  }

  if (list0 > self->gop.num_ref_frames)
    list0 = self->gop.num_ref_frames;
  if (list1 > self->gop.num_ref_frames)
    list1 = self->gop.num_ref_frames;

  if (list0 == 0) {
    GST_INFO_OBJECT (self,
        "No reference support, fallback to intra only stream");

    /* It does not make sense that if only the list1 exists. */
    self->gop.num_ref_frames = 0;

    self->gop.ip_period = 0;
    self->gop.num_bframes = 0;
    self->gop.b_pyramid = FALSE;
    self->gop.highest_pyramid_level = 0;
    self->gop.num_iframes = self->gop.idr_period - 1 /* The idr */ ;
    self->gop.ref_num_list0 = 0;
    self->gop.ref_num_list1 = 0;
    goto create_poc;
  }
  if (self->gop.num_ref_frames <= 1) {
    GST_INFO_OBJECT (self, "The number of reference frames is only %d,"
        " no B frame allowed, fallback to I/P mode", self->gop.num_ref_frames);
    self->gop.num_bframes = 0;
    list1 = 0;
  }

  /* b_pyramid needs at least 1 ref for B, besides the I/P */
  if (self->gop.b_pyramid && self->gop.num_ref_frames <= 2) {
    GST_INFO_OBJECT (self, "The number of reference frames is only %d,"
        " not enough for b_pyramid", self->gop.num_ref_frames);
    self->gop.b_pyramid = FALSE;
  }

  if (list1 == 0 && self->gop.num_bframes > 0) {
    GST_INFO_OBJECT (self,
        "No hw reference support for list 1, fallback to I/P mode");
    self->gop.num_bframes = 0;
    self->gop.b_pyramid = FALSE;
  }

  /* I/P mode, no list1 needed. */
  if (self->gop.num_bframes == 0)
    list1 = 0;

  /* Not enough B frame, no need for b_pyramid. */
  if (self->gop.num_bframes <= 1)
    self->gop.b_pyramid = FALSE;

  /* b pyramid has only one backward ref. */
  if (self->gop.b_pyramid)
    list1 = 1;

  if (self->gop.num_ref_frames > list0 + list1) {
    self->gop.num_ref_frames = list0 + list1;
    GST_INFO_OBJECT (self, "HW limits, lowering the number of reference"
        " frames to %d", self->gop.num_ref_frames);
  }

  /* How many possible refs within a GOP. */
  gop_ref_num = (self->gop.idr_period + self->gop.num_bframes) /
      (self->gop.num_bframes + 1);
  /* The end ref */
  if (self->gop.num_bframes > 0
      /* frame_num % (self->gop.num_bframes + 1) happens to be the end P */
      && (self->gop.idr_period % (self->gop.num_bframes + 1) != 1))
    gop_ref_num++;

  /* Adjust reference num based on B frames and B pyramid. */
  if (self->gop.num_bframes == 0) {
    self->gop.b_pyramid = FALSE;
    self->gop.ref_num_list0 = self->gop.num_ref_frames;
    self->gop.ref_num_list1 = 0;
  } else if (self->gop.b_pyramid) {
    guint b_frames = self->gop.num_bframes;
    guint b_refs;

    /* b pyramid has only one backward ref. */
    g_assert (list1 == 1);
    self->gop.ref_num_list1 = list1;
    self->gop.ref_num_list0 =
        self->gop.num_ref_frames - self->gop.ref_num_list1;

    b_frames = b_frames / 2;
    b_refs = 0;
    while (b_frames) {
      /* At least 1 B ref for each level, plus begin and end 2 P/I */
      b_refs += 1;
      if (b_refs + 2 > self->gop.num_ref_frames)
        break;

      self->gop.highest_pyramid_level++;
      b_frames = b_frames / 2;
    }

    GST_INFO_OBJECT (self, "pyramid level is %d",
        self->gop.highest_pyramid_level);
  } else {
    /* We prefer list0. Backward refs have more latency. */
    self->gop.ref_num_list1 = 1;
    self->gop.ref_num_list0 =
        self->gop.num_ref_frames - self->gop.ref_num_list1;
    /* Balance the forward and backward refs, but not cause a big latency. */
    while ((self->gop.num_bframes * self->gop.ref_num_list1 <= 16)
        && (self->gop.ref_num_list1 <= gop_ref_num)
        && (self->gop.ref_num_list1 < list1)
        && (self->gop.ref_num_list0 / self->gop.ref_num_list1 > 4)) {
      self->gop.ref_num_list0--;
      self->gop.ref_num_list1++;
    }

    if (self->gop.ref_num_list0 > list0)
      self->gop.ref_num_list0 = list0;
  }

  /* It's OK, keep slots for GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME frame. */
  if (self->gop.ref_num_list0 > gop_ref_num)
    GST_DEBUG_OBJECT (self, "num_ref_frames %d is bigger than gop_ref_num %d",
        self->gop.ref_num_list0, gop_ref_num);

  /* Include the ref picture itself. */
  self->gop.ip_period = 1 + self->gop.num_bframes;

  p_frames = gop_ref_num - 1 /* IDR */ ;
  if (p_frames < 0)
    p_frames = 0;
  if (self->gop.num_iframes > p_frames) {
    self->gop.num_iframes = p_frames;
    GST_INFO_OBJECT (self, "Too many I frames insertion, lowering it to %d",
        self->gop.num_iframes);
  }

  if (self->gop.num_iframes > 0) {
    guint total_i_frames = self->gop.num_iframes + 1 /* IDR */ ;
    self->gop.i_period =
        (gop_ref_num / total_i_frames) * (self->gop.num_bframes + 1);
  }

create_poc:
  self->gop.log2_max_frame_num = _get_log2_max_num (self->gop.idr_period);
  self->gop.max_frame_num = (1 << self->gop.log2_max_frame_num);
  self->gop.log2_max_pic_order_cnt = self->gop.log2_max_frame_num + 1;
  self->gop.max_pic_order_cnt = (1 << self->gop.log2_max_pic_order_cnt);

  _create_gop_frame_types (self);
  _print_gop_structure (self);
}


static void
gst_h265_encoder_init (GstH265Encoder * self)
{
  self->priv = gst_h265_encoder_get_instance_private (self);
}

static void
gst_h265_encoder_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_h265_encoder_start (GstVideoEncoder * encoder)
{
  GstH265Encoder *self = GST_H265_ENCODER (encoder);
  GstH265EncoderPrivate *priv = self->priv;

  priv->current_quality = self->prop.min_quality;
  priv->used_bytes = 0;
  priv->nb_frames = 0;

  self->width = 0;
  self->height = 0;

  return TRUE;
}

static gboolean
gst_h265_encoder_stop (GstVideoEncoder * encoder)
{
  return TRUE;
}

static gboolean
gst_h265_encoder_reset (GstVideoEncoder * encoder, gboolean hard)
{
  GstH265Encoder *self = GST_H265_ENCODER (encoder);

  self->gop.idr_period = self->prop.idr_period;
  self->gop.total_idr_count = 0;
  self->gop.num_iframes = 0;
  self->gop.num_ref_frames = 0;
  self->gop.cur_frame_index = 0;
  self->gop.max_pic_order_cnt = 0;

  return TRUE;
}

static gboolean
gst_h265_encoder_set_format (GstVideoEncoder * encoder,
    GstVideoCodecState * state)
{
  GstH265Encoder *self = GST_H265_ENCODER (encoder);
  GstH265EncoderClass *base_class = GST_H265_ENCODER_GET_CLASS (self);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  self->input_state = gst_video_codec_state_ref (state);

  self->width = GST_VIDEO_INFO_WIDTH (&self->input_state->info);
  self->height = GST_VIDEO_INFO_HEIGHT (&self->input_state->info);

  self->luma_width = GST_ROUND_UP_16 (self->width);
  self->luma_height = GST_ROUND_UP_16 (self->height);

  if (base_class->set_format) {
    base_class->set_format (self, state);
  }

  self->frame_duration =
      gst_util_uint64_scale (GST_SECOND, self->input_state->info.fps_d,
      self->input_state->info.fps_n);

  if (!_calculate_tier_level (self))
    return FALSE;

  _generate_gop_structure (self);

  return TRUE;
}

static GstFlowReturn
gst_h265_encoder_set_quality (GstH265Encoder * self, GstH265Frame * h265_frame)
{
  GstH265EncoderPrivate *priv = self->priv;
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

  h265_frame->quality = qp;

  return GST_FLOW_OK;
}

static void
gst_h265_encoder_mark_frame (GstH265Encoder * self, GstH265Frame * h265_frame)
{
  GstVideoCodecFrame *frame = h265_frame->frame;
  GstH265EncoderPrivate *priv = self->priv;

  priv->current_quality = h265_frame->quality;

  if (frame->output_buffer)
    priv->used_bytes += gst_buffer_get_size (frame->output_buffer);

  priv->nb_frames++;
}

static GstFlowReturn
_push_buffer_to_downstream (GstH265Encoder * self, GstVideoCodecFrame * frame)
{
  GstH265EncoderClass *base_class = GST_H265_ENCODER_GET_CLASS (self);

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
_push_out_one_buffer (GstH265Encoder * self)
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
gst_h265_encoder_handle_frame (GstVideoEncoder * encoder,
    GstVideoCodecFrame * frame)
{
  GstH265Encoder *self = GST_H265_ENCODER (encoder);
  GstH265EncoderClass *klass = GST_H265_ENCODER_GET_CLASS (self);
  GstFlowReturn ret = GST_FLOW_OK;
  GstH265Frame *h265_frame = gst_h265_frame_new (frame);
  GstVideoCodecFrame *frame_encode = NULL;

  ret = gst_h265_encoder_set_quality (self, h265_frame);
  if (ret != GST_FLOW_OK)
    return ret;

  if (!klass->new_frame (self, frame))
    goto error_new_frame;

  if (!klass->reorder_frame (self, frame, FALSE, &frame_encode))
    goto error_reorder;

  /* TODO: add encoding parameters management here
   * for now just send the frame to encode */
  while (frame_encode) {
    ret = klass->encode_frame (self, h265_frame->frame, FALSE);
    if (ret == GST_FLOW_OK)
      gst_h265_encoder_mark_frame (self, h265_frame);
    else
      goto error_encode;

    frame_encode = NULL;
    if (!klass->reorder_frame (self, NULL, FALSE, &frame_encode))
      goto error_reorder;
    while (g_queue_get_length (&self->output_list) > 0)
      ret = _push_out_one_buffer (self);

  }

  return ret;
error_new_frame:
  {
    GST_ELEMENT_ERROR (encoder, STREAM, ENCODE,
        ("Failed to create the input frame."), (NULL));
    gst_clear_buffer (&frame->output_buffer);
    gst_video_encoder_finish_frame (encoder, frame);
    return GST_FLOW_ERROR;
  }
error_reorder:
  {
    GST_ELEMENT_ERROR (encoder, STREAM, ENCODE,
        ("Failed to reorder the input frame."), (NULL));
    if (frame) {
      gst_clear_buffer (&frame->output_buffer);
      gst_video_encoder_finish_frame (encoder, frame);
    }
    return GST_FLOW_ERROR;
  }
error_encode:
  {
    GST_ELEMENT_ERROR (encoder, STREAM, ENCODE,
        ("Failed to encode the frame %s.", gst_flow_get_name (ret)), (NULL));
    gst_clear_buffer (&frame_encode->output_buffer);
    gst_video_encoder_finish_frame (encoder, frame_encode);
    return ret;
  }
}

static void
gst_h265_encoder_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstH265Encoder *self = GST_H265_ENCODER (object);
  GstH265EncoderPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_IDR_PERIOD:
      GST_OBJECT_LOCK (self);
      g_value_set_int (value, self->prop.idr_period);
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
gst_h265_encoder_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstH265Encoder *self = GST_H265_ENCODER (object);
  GstH265EncoderPrivate *priv = self->priv;

  switch (property_id) {
    case PROP_IDR_PERIOD:
      GST_OBJECT_LOCK (self);
      self->prop.idr_period = g_value_get_int (value);
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
gst_h265_encoder_class_init (GstH265EncoderClass * klass)
{
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = GST_DEBUG_FUNCPTR (gst_h265_encoder_finalize);
  object_class->get_property = gst_h265_encoder_get_property;
  object_class->set_property = gst_h265_encoder_set_property;

  encoder_class->start = GST_DEBUG_FUNCPTR (gst_h265_encoder_start);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_h265_encoder_stop);
  encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_h265_encoder_set_format);
  encoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_h265_encoder_handle_frame);
  encoder_class->reset = GST_DEBUG_FUNCPTR (gst_h265_encoder_reset);
  /**
   * GstH265Encoder:idr-period:
   *
   *
   * Since: 1.24
   */
  g_object_class_install_property (object_class, PROP_IDR_PERIOD,
      g_param_spec_int ("idr-period", "IDR period",
          "Interval between keyframes",
          0, G_MAXINT, H265ENC_DEFAULT_IDR_PERIOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
   /**
   * GstH265Encoder:bitrate:
   *
   *
   * Since: 1.24
   */
  g_object_class_install_property (object_class, PROP_BITRATE,
      g_param_spec_uint64 ("bitrate", "Targeted bitrate",
          "Set bitrate target",
          0, UINT_MAX, H265_DEFAULT_BITRATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT));
}
