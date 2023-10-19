/* GStreamer
 * Copyright (C) 2023 Igalia, S.L.
 *     Author: Stéphane Cerveau<scerveau@igalia.com>
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

/**
 * SECTION:element-vkh265enc
 * @title: vkh265enc
 * @short_description: A Vulkan based H265 video encoder
 *
 * vkh265enc encodes raw video surfaces into H.264 bitstreams using
 * Vulkan
 *
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 videotestsrc num-buffers=60 ! timeoverlay ! vulkanupload ! vulkanh265enc ! h265parse ! mp4mux ! filesink location=test.mp4
 * ```
 *
 * Since: 1.24
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vkh265enc.h"

#include <gst/codecparsers/gsth265bitwriter.h>

#include "gstvulkanelements.h"

#include <gst/vulkan/gstvkdevice.h>
#include <gst/vulkan/gstvkencoder.h>

static GstStaticPadTemplate gst_vulkan_h265enc_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_VULKAN_IMAGE, "NV12")));

static GstStaticPadTemplate gst_vulkan_h265enc_src_template =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265, "
        "profile = { (string) main, (string) main-10, (string) main-still-picture } ,"
        "stream-format = { (string) byte-stream }, "
        "alignment = (string) au"));

typedef struct _GstVulkanH265EncodeFrame GstVulkanH265EncodeFrame;

/* Scale factor for bitrate (HRD bit_rate_scale: min = 6) */
#define SX_BITRATE 6
/* Scale factor for CPB size (HRD cpb_size_scale: min = 4) */
#define SX_CPB_SIZE 4
/* Maximum sizes for common headers (in bits) */
#define MAX_SPS_HDR_SIZE  16473
#define MAX_VUI_PARAMS_SIZE  210
#define MAX_HRD_PARAMS_SIZE  4103
#define MAX_PPS_HDR_SIZE  101
#define MAX_SLICE_HDR_SIZE  397 + 2572 + 6670 + 2402

#define H265_MB_SIZE_ALIGNMENT 16

#define DEFAULT_H265_AVERAGE_BIRATE 10000000

enum
{
  PROP_0,
  PROP_RATE_CONTROL,
  PROP_VIDEO_USAGE,
  PROP_VIDEO_CONTENT,
  PROP_TUNING_MODE,
  PROP_NUM_SLICES,
  PROP_MIN_QP,
  PROP_MAX_QP,
  PROP_QP_I,
  PROP_QP_P,
  PROP_QP_B,
  PROP_NUM_REF_FRAMES,
  PROP_AUD,
  PROP_CC,
  PROP_AVERAGE_BITRATE,
  PROP_QUALITY_LEVEL,
  PROP_MAX
};

static GParamSpec *properties[PROP_MAX];

#define GST_TYPE_VULKAN_H265_RATE_CONTROL (gst_vulkan_h265_enc_rate_control_get_type ())
static GType
gst_vulkan_h265_enc_rate_control_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR, "default", "default"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR,
            "Rate control is disabled",
          "disabled"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR,
            "Constant bitrate mode rate control mode",
          "cbr"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR,
            "Variable bitrate mode rate control mode",
          "vbr"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstVulkanH265EncRateControl", values);
  }
  return qtype;
}

#define GST_TYPE_VULKAN_H265_ENCODE_USAGE (gst_vulkan_h265_enc_usage_get_type ())
static GType
gst_vulkan_h265_enc_usage_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR, "default", "default"},
      {VK_VIDEO_ENCODE_USAGE_TRANSCODING_BIT_KHR, "Encode usage transcoding",
          "transcoding"},
      {VK_VIDEO_ENCODE_USAGE_STREAMING_BIT_KHR, "Encode usage streaming",
          "streaming"},
      {VK_VIDEO_ENCODE_USAGE_RECORDING_BIT_KHR, "Encode usage recording",
          "recording"},
      {VK_VIDEO_ENCODE_USAGE_CONFERENCING_BIT_KHR, "Encode usage conferencing",
          "conferencing"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstVulkanH265EncUsage", values);
  }
  return qtype;
}

#define GST_TYPE_VULKAN_H265_ENCODE_CONTENT (gst_vulkan_h265_enc_content_get_type ())
static GType
gst_vulkan_h265_enc_content_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR, "default", "default"},
      {VK_VIDEO_ENCODE_CONTENT_CAMERA_BIT_KHR, "Encode content camera",
          "camera"},
      {VK_VIDEO_ENCODE_CONTENT_DESKTOP_BIT_KHR, "Encode content desktop",
          "desktop"},
      {VK_VIDEO_ENCODE_CONTENT_RENDERED_BIT_KHR, "Encode content rendered",
          "rendered"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstVulkanH265EncContent", values);
  }
  return qtype;
}

#define GST_TYPE_VULKAN_H265_ENCODE_TUNING_MODE (gst_vulkan_h265_enc_tuning_mode_get_type ())
static GType
gst_vulkan_h265_enc_tuning_mode_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR, "default", "default"},
      {VK_VIDEO_ENCODE_TUNING_MODE_HIGH_QUALITY_KHR, "Tuning mode high quality",
          "high-quality"},
      {VK_VIDEO_ENCODE_TUNING_MODE_LOW_LATENCY_KHR, "Tuning mode low latency",
          "low-latency"},
      {VK_VIDEO_ENCODE_TUNING_MODE_ULTRA_LOW_LATENCY_KHR,
          "Tuning mode ultra low latency", "ultra-low-latency"},
      {VK_VIDEO_ENCODE_TUNING_MODE_LOSSLESS_KHR, "Tuning mode lossless",
          "lossless"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstVulkanH265EncTuningMode", values);
  }
  return qtype;
}

typedef struct _VkH265Params
{
  StdVideoH265VideoParameterSet vps;
  StdVideoH265SequenceParameterSet sps;
  StdVideoH265PictureParameterSet pps;

  StdVideoH265HrdParameters hrd;
  StdVideoH265SequenceParameterSetVui vui;
  StdVideoH265ProfileTierLevel profile_tier_level;
  StdVideoH265ScalingLists scaling_lists;
  int32_t offset_for_ref_frame[255];
} VkH265Params;


struct _GstVulkanH265Enc
{
  /*< private > */
  GstH265Encoder parent;

  GstVideoCodecState *output_state;
  gint width;
  gint height;

  GstVulkanInstance *instance;
  GstVulkanDevice *device;
  GstVulkanQueue *queue;
  GstVulkanEncoder *encoder;

  gint dpb_size;

  GstVulkanVideoProfile profile;
  VkVideoEncodeH265CapabilitiesEXT caps;
  VkVideoEncodeH265RateControlInfoEXT rate_control;

  VkH265Params session_params;

  /* H265 fields */
  guint8 level_idc;
  const gchar *level_str;
  /* Minimum Compression Ratio (A.3.1) */
  guint min_cr;
  gboolean use_cabac;
  gboolean use_dct8x8;
  gboolean aud;
  gboolean cc;
  guint32 num_slices;
  guint32 packed_headers;
  struct
  {
    guint32 min_qp;
    guint32 max_qp;
    guint32 qp_i;
    guint32 qp_p;
    guint32 qp_b;
    guint32 num_slices;
    guint32 num_ref_frames;
    guint32 quality_level;
    gboolean aud;
    gboolean cc;
  } prop;

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

  GstH265SPS sequence_hdr;
  GstH265PPS picture_hdr;
};

struct _GstVulkanH265EncodeFrame
{
  GstVulkanEncodePicture *picture;
  GstH265SliceType type;
  gboolean is_ref;
  guint pyramid_level;
  /* Only for b pyramid */
  gint left_ref_poc_diff;
  gint right_ref_poc_diff;

  gint poc;
  gint frame_num;
  /* The pic_num will be marked as unused_for_reference, which is
   * replaced by this frame. -1 if we do not need to care about it
   * explicitly. */
  gint unused_for_reference_pic_num;

  /* The total frame count we handled. */
  guint total_frame_count;

  gboolean last_frame;

  StdVideoEncodeH265WeightTable slice_wt;
  StdVideoEncodeH265SliceSegmentHeader slice_hdr;
  VkVideoEncodeH265NaluSliceSegmentInfoEXT slice_info;
  VkVideoEncodeH265RateControlInfoEXT rc_info;
  VkVideoEncodeH265RateControlLayerInfoEXT rc_layer_info;
  VkVideoEncodeH265PictureInfoEXT enc_pic_info;
  VkVideoEncodeH265DpbSlotInfoEXT dpb_slot_info;
  VkVideoEncodeH265QualityLevelPropertiesEXT quality_level;

  StdVideoEncodeH265PictureInfo pic_info;
  StdVideoEncodeH265ReferenceInfo ref_info;
  StdVideoEncodeH265ReferenceListsInfo ref_list_info;
};

GST_DEBUG_CATEGORY_STATIC (gst_vulkan_h265enc_debug);
#define GST_CAT_DEFAULT gst_vulkan_h265enc_debug

#define gst_vulkan_h265enc_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanH265Enc, gst_vulkan_h265enc,
    GST_TYPE_H265_ENCODER, GST_DEBUG_CATEGORY_INIT (gst_vulkan_h265enc_debug,
        "vulkanh265enc", 0, "Vulkan H.264 enccoder"));
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (vulkanh265enc, "vulkanh265enc",
    GST_RANK_NONE, GST_TYPE_VULKAN_H265ENC, vulkan_element_init (plugin));

static GstVulkanH265EncodeFrame *
gst_vulkan_h265_encode_frame_new (void)
{
  GstVulkanH265EncodeFrame *frame;

  frame = g_new (GstVulkanH265EncodeFrame, 1);
  frame->type = GST_H265_I_SLICE;
  frame->is_ref = TRUE;
  frame->frame_num = 0;
  frame->unused_for_reference_pic_num = -1;
  frame->picture = NULL;
  frame->total_frame_count = 0;
  frame->last_frame = FALSE;

  return frame;
}

static void
gst_vulkan_h265_encode_frame_free (gpointer pframe)
{
  GstVulkanH265EncodeFrame *frame = pframe;
  g_clear_pointer (&frame->picture, gst_vulkan_encode_picture_free);
  g_free (frame);
}

static inline GstVulkanH265EncodeFrame *
_enc_frame (GstVideoCodecFrame * frame)
{
  GstVulkanH265EncodeFrame *enc_frame =
      gst_video_codec_frame_get_user_data (frame);
  g_assert (enc_frame);
  return enc_frame;
}

static inline unsigned
_get_component_bit_depth (VkVideoComponentBitDepthFlagsKHR bit_depth)
{
  switch (bit_depth) {
    case VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR:
      return 8;
    case VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR:
      return 10;
    case VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR:
      return 12;
    default:
      return 0;
  }
}

static void
gst_vulkan_h265enc_init_std_vps (GstVulkanH265Enc * self, gint vps_id)
{
  StdVideoH265VideoParameterSet *vkvps = &self->session_params.vps;
  memset (vkvps, 0, sizeof (StdVideoH265VideoParameterSet));
  vkvps->flags.vps_temporal_id_nesting_flag = 1;        // FIXME: This parameter blocks the session init with STD_VIDEO_H265_PROFILE_IDC_MAIN not with STD_VIDEO_H265_PROFILE_IDC_HIGH
  vkvps->flags.vps_sub_layer_ordering_info_present_flag = 1;
  vkvps->vps_video_parameter_set_id = vps_id;
}

static void
gst_vulkan_h265enc_init_std_sps (GstVulkanH265Enc * self, guint vps_id,
    guint sps_id)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  VkVideoChromaSubsamplingFlagBitsKHR chroma_format;
  VkVideoComponentBitDepthFlagsKHR bit_depth_luma, bit_depth_chroma;

  StdVideoH265SequenceParameterSet *vksps = &self->session_params.sps;
  StdVideoH265SequenceParameterSetVui *vkvui = &self->session_params.vui;
  StdVideoH265ProfileTierLevel *vk_profile_tier_level =
      &self->session_params.profile_tier_level;
  memset (vksps, 0, sizeof (StdVideoH265SequenceParameterSet));
  memset (vkvui, 0, sizeof (StdVideoH265SequenceParameterSet));
  memset (vk_profile_tier_level, 0, sizeof (StdVideoH265ProfileTierLevel));

  gst_vulkan_video_get_chroma_info_from_format (base->input_state->info.
      finfo->format, &chroma_format, &bit_depth_luma, &bit_depth_chroma);

  const uint32_t mbAlignedWidth =
      (self->width + H265_MB_SIZE_ALIGNMENT - 1) & ~(H265_MB_SIZE_ALIGNMENT -
      1);
  const uint32_t mbAlignedHeight =
      (self->height + H265_MB_SIZE_ALIGNMENT - 1) & ~(H265_MB_SIZE_ALIGNMENT -
      1);

  vksps->flags.sps_temporal_id_nesting_flag = 1;
  vksps->flags.sps_sub_layer_ordering_info_present_flag = 1;
  vksps->flags.sample_adaptive_offset_enabled_flag = 1;
  vksps->flags.sps_temporal_mvp_enabled_flag = 1;
  vksps->flags.strong_intra_smoothing_enabled_flag = 1;
  vksps->flags.vui_parameters_present_flag = 1;

  vksps->sps_video_parameter_set_id = vps_id;
  vksps->sps_seq_parameter_set_id = sps_id;

  vk_profile_tier_level->flags.general_tier_flag = 0;
  vk_profile_tier_level->flags.general_progressive_source_flag = 1;
  vk_profile_tier_level->flags.general_interlaced_source_flag = 0;
  vk_profile_tier_level->flags.general_non_packed_constraint_flag = 0;
  vk_profile_tier_level->flags.general_frame_only_constraint_flag = 1;

  vk_profile_tier_level->general_profile_idc =
      self->profile.codec.h265enc.stdProfileIdc;
  vk_profile_tier_level->general_level_idc =
      gst_vulkan_video_h265_level_idc (base->level_idc);

  vksps->pProfileTierLevel = vk_profile_tier_level;

  vksps->chroma_format_idc =
      gst_vulkan_video_h265_chromat_from_format (base->input_state->info.
      finfo->format);

  vksps->bit_depth_luma_minus8 = _get_component_bit_depth (bit_depth_luma) - 8;
  vksps->bit_depth_chroma_minus8 =
      _get_component_bit_depth (bit_depth_chroma) - 8;


  vksps->pic_width_in_luma_samples =
      mbAlignedWidth / H265_MB_SIZE_ALIGNMENT - 1;
  vksps->pic_height_in_luma_samples =
      mbAlignedHeight / H265_MB_SIZE_ALIGNMENT - 1;

  // This allows for picture order count values in the range [0, 255].
  vksps->log2_max_pic_order_cnt_lsb_minus4 = 4u;
  vksps->log2_diff_max_min_luma_coding_block_size = 2u;
  vksps->log2_diff_max_min_luma_transform_block_size = 2u;

  /* *INDENT-OFF* */
  vkvui->flags = (StdVideoH265SpsVuiFlags) {
    .video_signal_type_present_flag = TRUE,
    .vui_timing_info_present_flag = TRUE,
  };
  /* *INDENT-ON* */

  // Set the VUI parameters
  vkvui->aspect_ratio_idc = STD_VIDEO_H265_ASPECT_RATIO_IDC_UNSPECIFIED;
  vkvui->sar_width = GST_VIDEO_INFO_PAR_N (&base->input_state->info);
  vkvui->sar_height = GST_VIDEO_INFO_PAR_D (&base->input_state->info);
  vkvui->video_format = 1;      // PAL Table E.2
  vkvui->vui_num_units_in_tick =
      GST_VIDEO_INFO_FPS_D (&base->input_state->info);
  vkvui->vui_time_scale = GST_VIDEO_INFO_FPS_N (&base->input_state->info) * 2;

  vksps->flags.vui_parameters_present_flag = TRUE;
  vksps->pSequenceParameterSetVui = vkvui;
}

static void
gst_vulkan_h265enc_init_std_pps (GstVulkanH265Enc * self, guint vps_id,
    guint sps_id, guint pps_id)
{
  StdVideoH265PictureParameterSet *vkpps = &self->session_params.pps;
  memset (vkpps, 0, sizeof (StdVideoH265PictureParameterSet));

  vkpps->flags.cu_qp_delta_enabled_flag = 0u;   //1u; // FIXME: This parameter blocks the session init with dedicated profile_idc
  vkpps->flags.deblocking_filter_control_present_flag = 0u;
  vkpps->flags.pps_loop_filter_across_slices_enabled_flag = 1u;

  vkpps->sps_video_parameter_set_id = vps_id;
  vkpps->pps_seq_parameter_set_id = sps_id;
  vkpps->pps_pic_parameter_set_id = pps_id;

  vkpps->num_ref_idx_l0_default_active_minus1 = 0u;
}

static gboolean
_init_packed_headers (GstVulkanH265Enc * self)
{
  // FIXME: Check the capability to generate packed headers from the implementation
  // See
  self->packed_headers = VULKAN_PACKED_HEADER_TYPE_SPS
      | VULKAN_PACKED_HEADER_TYPE_PPS;

  return TRUE;
}


static gboolean
gst_vulkan_h265enc_get_session_params (GstVulkanH265Enc * self,
    void **packed_params, gsize * size, gint vps_id, gint sps_id, gint pps_id)
{
  VkVideoEncodeH265SessionParametersGetInfoEXT video_codec_session_info;
  gsize header_size;

  video_codec_session_info = (VkVideoEncodeH265SessionParametersGetInfoEXT) {
    /* *INDENT-OFF* */
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_EXT,
    .pNext = NULL,
    .writeStdVPS = (vps_id >= 0) ,
    .writeStdSPS = (sps_id >= 0) ,
    .writeStdPPS = (pps_id >= 0),
    .stdVPSId = vps_id,
    .stdSPSId = sps_id,
    .stdPPSId = pps_id,
    /* *INDENT-ON* */
  };

  if (!gst_vulkan_encoder_get_session_params (self->encoder,
          &video_codec_session_info, &header_size, NULL) || header_size == 0)
    return FALSE;

  if (*size > 0 && header_size > *size)
    *packed_params = g_realloc (packed_params, header_size);
  else
    *packed_params = g_malloc0 (header_size);

  gst_vulkan_encoder_get_session_params (self->encoder,
      &video_codec_session_info, &header_size, *packed_params);
  *size = header_size;
  return (header_size != 0);
}

static void
gst_vulkan_h265enc_reset (GstVulkanH265Enc * self)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  self->num_slices = self->prop.num_slices;
  self->rc.min_qp = self->prop.min_qp;
  self->rc.max_qp = self->prop.max_qp;
  self->rc.qp_i = self->prop.qp_i;
  self->rc.qp_p = self->prop.qp_p;
  self->rc.qp_b = self->prop.qp_b;
  base->gop.num_ref_frames = self->prop.num_ref_frames;
  self->aud = self->prop.aud;
  self->cc = self->prop.cc;
}

static gboolean
gst_vulkan_h265enc_init_session (GstVulkanH265Enc * self)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (self);
  GError *err = NULL;
  GstVulkanEncoderParameters enc_params;
  VkVideoEncodeH265SessionParametersAddInfoEXT params_add;
  VkVideoChromaSubsamplingFlagBitsKHR chroma_format;
  VkVideoComponentBitDepthFlagsKHR bit_depth_luma, bit_depth_chroma;
  GstVideoCodecState *output_state =
      gst_video_encoder_get_output_state (encoder);

  if (!gst_vulkan_video_get_chroma_info_from_format (base->input_state->
          info.finfo->format, &chroma_format, &bit_depth_luma,
          &bit_depth_chroma)) {
    GST_WARNING_OBJECT (self,
        "unable to retrieve chroma info from input format");
    return FALSE;
  }

  base->profile =
      gst_h265_encoder_get_profile_from_str
      (gst_vulkan_video_get_profile_from_caps (output_state->caps));

  /* *INDENT-OFF* */
  self->profile = (GstVulkanVideoProfile) {
  .profile = (VkVideoProfileInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
      .pNext = &self->profile.codec.h265enc,
      .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT,
      .chromaSubsampling = chroma_format,
      .chromaBitDepth = bit_depth_luma,
      .lumaBitDepth = bit_depth_chroma,
  },
  .codec.h265enc = (VkVideoEncodeH265ProfileInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_EXT,
      .pNext = NULL,
      .stdProfileIdc = gst_vulkan_video_h265_profile_type (base->profile),
    }
  };

  self->level_idc = base->level_idc;

  self->caps = (VkVideoEncodeH265CapabilitiesEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_EXT
  };
  /* *INDENT-ON* */

  gst_vulkan_h265enc_init_std_vps (self, 0);
  gst_vulkan_h265enc_init_std_sps (self, 0, 0);
  gst_vulkan_h265enc_init_std_pps (self, 0, 0, 0);

  /* *INDENT-OFF* */
  params_add = (VkVideoEncodeH265SessionParametersAddInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT,
        .pStdVPSs = &self->session_params.vps,
        .stdVPSCount = 1,
        .pStdSPSs = &self->session_params.sps,
        .stdSPSCount = 1,
        .pStdPPSs = &self->session_params.pps,
        .stdPPSCount = 1,
  };
  enc_params.create.h265 = (VkVideoEncodeH265SessionParametersCreateInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT,
        .maxStdVPSCount = 1,
        .maxStdSPSCount = 1,
        .maxStdPPSCount = 1,
        .pParametersAddInfo = &params_add,
  };
   /* *INDENT-ON* */

  if (!gst_vulkan_encoder_start (self->encoder, &self->profile, &enc_params,
          &err)) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Unable to start vulkan encoder with error %s", err->message), (NULL));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_vulkan_h265enc_open (GstVideoEncoder * encoder)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (encoder);

  if (!gst_vulkan_ensure_element_data (GST_ELEMENT (encoder), NULL,
          &self->instance)) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Failed to retrieve vulkan instance"), (NULL));
    return FALSE;
  }

  if (!self->queue) {
    self->queue =
        gst_vulkan_select_queue (self->instance, VK_QUEUE_VIDEO_ENCODE_BIT_KHR);

    if (!self->queue) {
      gst_clear_object (&self->device);
      gst_clear_object (&self->instance);

      return FALSE;
    }
    self->device = gst_object_ref (self->queue->device);
  }

  self->encoder =
      gst_vulkan_queue_create_encoder (self->queue,
      VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT);

  if (!self->encoder) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Failed to instanciate the encoder"), (NULL));
  }

  return TRUE;
}

static gboolean
gst_vulkan_h265enc_close (GstVideoEncoder * encoder)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (encoder);

  gst_clear_object (&self->encoder);
  gst_clear_object (&self->queue);
  gst_clear_object (&self->device);
  gst_clear_object (&self->instance);

  return TRUE;
}

static gboolean
gst_vulkan_h265enc_stop (GstVideoEncoder * encoder)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (encoder);
  gst_vulkan_encoder_stop (self->encoder);
  return GST_VIDEO_ENCODER_CLASS (parent_class)->stop (encoder);
}

static gboolean
_query_context (GstVulkanH265Enc * self, GstQuery * query)
{
  if (gst_vulkan_handle_context_query (GST_ELEMENT (self), query, NULL,
          self->instance, self->device))
    return TRUE;

  if (gst_vulkan_queue_handle_context_query (GST_ELEMENT (self), query,
          self->queue))
    return TRUE;

  return FALSE;
}

static gboolean
gst_vulkan_h265enc_src_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = _query_context (GST_VULKAN_H265ENC (encoder), query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->src_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_vulkan_h265enc_sink_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = _query_context (GST_VULKAN_H265ENC (encoder), query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_vulkan_h265enc_set_format (GstH265Encoder * h265enc,
    GstVideoCodecState * state)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (h265enc);
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (self);
  GstCaps *outcaps;

  GST_DEBUG_OBJECT (self, "Set format");

  if (h265enc->input_state)
    gst_video_codec_state_unref (h265enc->input_state);
  h265enc->input_state = gst_video_codec_state_ref (state);

  self->width = state->info.width;
  self->height = state->info.height;


  if (self->output_state)
    gst_video_codec_state_unref (self->output_state);

  outcaps = gst_pad_get_pad_template_caps (encoder->srcpad);
  outcaps = gst_caps_fixate (outcaps);
  GST_INFO_OBJECT (self, "output caps: %" GST_PTR_FORMAT, outcaps);

  self->output_state =
      gst_video_encoder_set_output_state (GST_VIDEO_ENCODER (self),
      outcaps, state);

  GST_INFO_OBJECT (self, "output caps: %" GST_PTR_FORMAT, state->caps);

  gst_vulkan_h265enc_reset (self);

  _init_packed_headers (self);

  if (GST_VIDEO_ENCODER_CLASS (parent_class)->negotiate (encoder)) {
    return gst_vulkan_h265enc_init_session (self);
  }

  return FALSE;
}

/* STOP SCE IMPORT*/

static gboolean
_add_vulkan_params_header (GstVulkanH265Enc * self,
    GstVulkanH265EncodeFrame * frame)
{
  void *header = NULL;
  gsize header_size = 0;

  gst_vulkan_h265enc_get_session_params (self, &header, &header_size, 0, 0, 0);
  GST_LOG_OBJECT (self, "Adding params header of size %lu", header_size);
  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          header, header_size)) {
    GST_ERROR_OBJECT (self, "Failed to add the packed params header");
    return FALSE;
  }

  return TRUE;
}

static gint
_poc_asc_compare (const GstVulkanH265EncodeFrame ** a,
    const GstVulkanH265EncodeFrame ** b)
{
  return (*a)->poc - (*b)->poc;
}

static gint
_poc_des_compare (const GstVulkanH265EncodeFrame ** a,
    const GstVulkanH265EncodeFrame ** b)
{
  return (*b)->poc - (*a)->poc;
}

static gboolean
_add_aud (GstVulkanH265Enc * self, GstVulkanH265EncodeFrame * frame)
{
  guint8 aud_data[8] = { 0, };
  guint size;
  guint8 primary_pic_type = 0;

  switch (frame->type) {
    case GST_H265_I_SLICE:
      primary_pic_type = 0;
      break;
    case GST_H265_P_SLICE:
      primary_pic_type = 1;
      break;
    case GST_H265_B_SLICE:
      primary_pic_type = 2;
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  size = sizeof (aud_data);
  if (gst_h265_bit_writer_aud (primary_pic_type, TRUE, aud_data,
          &size) != GST_H265_BIT_WRITER_OK) {
    GST_ERROR_OBJECT (self, "Failed to generate the AUD");
    return FALSE;
  }

  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          aud_data, size)) {
    GST_ERROR_OBJECT (self, "Failed to add the AUD");
    return FALSE;
  }

  return TRUE;
}

static void
_create_sei_cc_message (GstVideoCaptionMeta * cc_meta,
    GstH265SEIMessage * sei_msg)
{
  guint8 *data;
  GstH265RegisteredUserData *user_data;

  sei_msg->payloadType = GST_H265_SEI_REGISTERED_USER_DATA;

  user_data = &sei_msg->payload.registered_user_data;

  user_data->country_code = 181;
  user_data->size = 10 + cc_meta->size;

  data = g_malloc (user_data->size);

  /* 16-bits itu_t_t35_provider_code */
  data[0] = 0;
  data[1] = 49;
  /* 32-bits ATSC_user_identifier */
  data[2] = 'G';
  data[3] = 'A';
  data[4] = '9';
  data[5] = '4';
  /* 8-bits ATSC1_data_user_data_type_code */
  data[6] = 3;
  /* 8-bits:
   * 1 bit process_em_data_flag (0)
   * 1 bit process_cc_data_flag (1)
   * 1 bit additional_data_flag (0)
   * 5-bits cc_count
   */
  data[7] = ((cc_meta->size / 3) & 0x1f) | 0x40;
  /* 8 bits em_data, unused */
  data[8] = 255;

  memcpy (data + 9, cc_meta->data, cc_meta->size);

  /* 8 marker bits */
  data[user_data->size - 1] = 255;

  user_data->data = data;
}

static gboolean
_create_sei_cc_data (GPtrArray * cc_list, GstH265NalUnitType nal_type,
    guint8 * sei_data, guint * data_size)
{
  GArray *msg_list = NULL;
  GstH265BitWriterResult ret;
  gint i;

  msg_list = g_array_new (TRUE, TRUE, sizeof (GstH265SEIMessage));
  g_array_set_clear_func (msg_list, (GDestroyNotify) gst_h265_sei_free);
  g_array_set_size (msg_list, cc_list->len);

  for (i = 0; i < cc_list->len; i++) {
    GstH265SEIMessage *msg = &g_array_index (msg_list, GstH265SEIMessage, i);
    _create_sei_cc_message (g_ptr_array_index (cc_list, i), msg);
  }

  ret = gst_h265_bit_writer_sei (msg_list, nal_type, TRUE, sei_data, data_size);

  g_array_unref (msg_list);

  return (ret == GST_H265_BIT_WRITER_OK);
}

static void
_add_sei_cc (GstVulkanH265Enc * self, GstVideoCodecFrame * gst_frame)
{
  GstVulkanH265EncodeFrame *frame;
  GPtrArray *cc_list = NULL;
  GstVideoCaptionMeta *cc_meta;
  gpointer iter = NULL;
  guint8 *packed_sei = NULL;
  guint sei_size = 0;

  frame = _enc_frame (gst_frame);

  /* SEI header size */
  sei_size = 6;
  while ((cc_meta = (GstVideoCaptionMeta *)
          gst_buffer_iterate_meta_filtered (gst_frame->input_buffer, &iter,
              GST_VIDEO_CAPTION_META_API_TYPE))) {
    if (cc_meta->caption_type != GST_VIDEO_CAPTION_TYPE_CEA708_RAW)
      continue;

    if (!cc_list)
      cc_list = g_ptr_array_new ();

    g_ptr_array_add (cc_list, cc_meta);
    /* Add enough SEI message size for bitwriter. */
    sei_size += cc_meta->size + 50;
  }

  if (!cc_list)
    goto out;

  packed_sei = g_malloc0 (sei_size);

  if (!_create_sei_cc_data (cc_list, frame->type, packed_sei, &sei_size)) {
    GST_WARNING_OBJECT (self, "Failed to write the SEI CC data");
    goto out;
  }

  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          packed_sei, sei_size)) {
    GST_WARNING_OBJECT (self, "Failed to add SEI CC data");
    goto out;
  }

out:
  g_clear_pointer (&cc_list, g_ptr_array_unref);
  if (packed_sei)
    g_free (packed_sei);
}


static gboolean
_encode_one_vulkan_frame (GstVulkanH265Enc * self,
    GstVulkanH265EncodeFrame * frame, GstVulkanH265EncodeFrame ** list0,
    guint list0_num, GstVulkanH265EncodeFrame ** list1, guint list1_num)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  GstVulkanVideoCapabilites enc_caps;
  GstVulkanEncodePicture *ref_pics[16] = { NULL, };
  gint i, ref_pics_num = 0;

  if (!gst_vulkan_encoder_vk_caps (self->encoder, &enc_caps))
    return FALSE;

   /* *INDENT-OFF* */
  frame->slice_wt = (StdVideoEncodeH265WeightTable) {
    .flags = (StdVideoEncodeH265WeightTableFlags) {
        .luma_weight_l0_flag = 0,
        .chroma_weight_l0_flag = 0,
        .luma_weight_l1_flag = 0,
        .chroma_weight_l1_flag = 0,
    },
    .luma_log2_weight_denom = 0,
    .delta_chroma_log2_weight_denom = 0,
    .delta_luma_weight_l0 = { 0 },
    .luma_offset_l0 = { 0 },
    .delta_chroma_weight_l0 = { { 0 } },
    .delta_chroma_offset_l0 = { { 0 } },
    .delta_luma_weight_l1 = { 0 },
    .luma_offset_l1 = { 0 },
    .delta_chroma_weight_l1 = { { 0 } },
    .delta_chroma_offset_l1 = { { 0 } },
  };

  frame->slice_hdr = (StdVideoEncodeH265SliceSegmentHeader) {
      .flags = (StdVideoEncodeH265SliceSegmentHeaderFlags) {
        .first_slice_segment_in_pic_flag = 0,
        .dependent_slice_segment_flag = 0,
        .slice_sao_luma_flag = 0,
        .slice_sao_chroma_flag = 0,
        .num_ref_idx_active_override_flag = gst_vulkan_video_h265_slice_type (frame->type) != STD_VIDEO_H265_SLICE_TYPE_I ? 1 : 0,
        .mvd_l1_zero_flag = 0,
        .cabac_init_flag = 0,
        .cu_chroma_qp_offset_enabled_flag = 0,
        .deblocking_filter_override_flag = 0,
        .slice_deblocking_filter_disabled_flag = 0,
        .collocated_from_l0_flag = 0,
        .slice_loop_filter_across_slices_enabled_flag = 0,
    },
      .slice_type = gst_vulkan_video_h265_slice_type (frame->type),
      .slice_segment_address = 0,
      .collocated_ref_idx = 0,
      .MaxNumMergeCand = 0,
      .slice_cb_qp_offset = 0,
      .slice_cr_qp_offset = 0,
      .slice_beta_offset_div2 = 0,
      .slice_tc_offset_div2 = 0,
      .slice_act_y_qp_offset = 0,
      .slice_act_cb_qp_offset = 0,
      .slice_act_cr_qp_offset = 0,
      .slice_qp_delta = 0,
      .pWeightTable = &frame->slice_wt,
  };

  frame->slice_info = (VkVideoEncodeH265NaluSliceSegmentInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_EXT,
    .pNext = NULL,
    .pStdSliceSegmentHeader = &frame->slice_hdr,
    .constantQp = 25,
  };

  frame->pic_info = (StdVideoEncodeH265PictureInfo) {
    .flags = (StdVideoEncodeH265PictureInfoFlags) {
        .is_reference = frame->is_ref,
        .IrapPicFlag = (gst_vulkan_video_h265_picture_type(frame->type, frame->is_ref) == STD_VIDEO_H265_PICTURE_TYPE_IDR),
        .used_for_long_term_reference = 0,
        .discardable_flag = 0,
        .cross_layer_bla_flag = 0,
        .pic_output_flag = 0,
        .no_output_of_prior_pics_flag = 0,
        .short_term_ref_pic_set_sps_flag = 0,
        .slice_temporal_mvp_enabled_flag = 0,
    },
    .pic_type = gst_vulkan_video_h265_picture_type(frame->type, frame->is_ref),
    .sps_video_parameter_set_id = self->session_params.sps.sps_video_parameter_set_id,
    .pps_seq_parameter_set_id = self->session_params.sps.sps_seq_parameter_set_id,
    .pps_pic_parameter_set_id =  self->session_params.pps.pps_pic_parameter_set_id,
    .PicOrderCntVal = frame->poc,
  };

  if (gst_vulkan_encoder_get_n_ref_slots(self->encoder)) {
    frame->ref_list_info = (StdVideoEncodeH265ReferenceListsInfo) {
      .flags = (StdVideoEncodeH265ReferenceListsInfoFlags) {
        .ref_pic_list_modification_flag_l0 = 0,
        .ref_pic_list_modification_flag_l1 = 0,
      },
      .num_ref_idx_l0_active_minus1 = 0,
      .num_ref_idx_l1_active_minus1 = 0,
      .RefPicList0 = {0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, },
      .RefPicList1 = {0,STD_VIDEO_H265_NO_REFERENCE_PICTURE, },
      .list_entry_l0 = {0, },
      .list_entry_l1 = {0, },
    };
    frame->pic_info.pRefLists = &frame->ref_list_info;
  }
  memset(frame->ref_list_info.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, STD_VIDEO_H265_MAX_NUM_LIST_REF);
  memset(frame->ref_list_info.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE, STD_VIDEO_H265_MAX_NUM_LIST_REF);


  frame->rc_layer_info = (VkVideoEncodeH265RateControlLayerInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_EXT,
    .pNext = NULL,
    .useMinQp = TRUE,
    .minQp = (VkVideoEncodeH265QpEXT){ self->rc.min_qp, self->rc.min_qp, self->rc.min_qp },
    .useMaxQp = TRUE,
    .maxQp = (VkVideoEncodeH265QpEXT){ self->rc.max_qp, self->rc.max_qp, self->rc.max_qp },
    .useMaxFrameSize = 0,
    .maxFrameSize = (VkVideoEncodeH265FrameSizeEXT) {0, 0, 0},
  };

  frame->rc_info = (VkVideoEncodeH265RateControlInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_EXT,
    .pNext = NULL,
    .gopFrameCount = 0,
    .idrPeriod = 0,
    .consecutiveBFrameCount = 0,

  };

  frame->quality_level = (VkVideoEncodeH265QualityLevelPropertiesEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_EXT,
    .pNext = NULL,
    .preferredRateControlFlags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_EXT,
    .preferredGopFrameCount = 0,
    .preferredIdrPeriod = 0,
    .preferredConsecutiveBFrameCount = 0,
    .preferredConstantQp = (VkVideoEncodeH265QpEXT){ self->rc.qp_i, self->rc.qp_p, self->rc.qp_b },
    .preferredMaxL0ReferenceCount = 0,
    .preferredMaxL1ReferenceCount = 0,
  };

  frame->enc_pic_info = (VkVideoEncodeH265PictureInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_EXT,
    .pNext = NULL,
    .naluSliceSegmentEntryCount = 1,
    .pNaluSliceSegmentEntries = &frame->slice_info,
    .pStdPictureInfo = &frame->pic_info,
  };
  frame->ref_info = (StdVideoEncodeH265ReferenceInfo) {
    .flags = (StdVideoEncodeH265ReferenceInfoFlags) {
      .used_for_long_term_reference =0,
    },
    .pic_type = gst_vulkan_video_h265_picture_type(frame->type, frame->is_ref),
    .PicOrderCntVal = frame->poc, //display order
    .TemporalId = 0,
  };

  frame->dpb_slot_info = (VkVideoEncodeH265DpbSlotInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_EXT,
    .pNext = NULL,
    .pStdReferenceInfo = &frame->ref_info,
  };

  /* *INDENT-ON* */

  frame->picture->codec_pic_info = &frame->enc_pic_info;
  frame->picture->codec_rc_info = &frame->rc_info;
  frame->picture->codec_rc_layer_info = &frame->rc_layer_info;
  frame->picture->codec_dpb_slot_info = &frame->dpb_slot_info;
  frame->picture->codec_quality_level = &frame->quality_level;
  frame->picture->fps_n = GST_VIDEO_INFO_FPS_N (&base->input_state->info);
  frame->picture->fps_d = GST_VIDEO_INFO_FPS_D (&base->input_state->info);

  for (i = 0; i < list0_num; i++) {
    ref_pics[i] = list0[i]->picture;
    ref_pics_num++;
  }
  //TODO should be better handled to have the multiple ref used by the current picture
  if (list0_num)
    frame->ref_list_info.RefPicList0[0] = list0[0]->picture->slotIndex;

  if (!gst_vulkan_encoder_encode (self->encoder, frame->picture, ref_pics,
          ref_pics_num)) {
    GST_ERROR_OBJECT (self, "Encode frame error");
    return FALSE;
  }
  return TRUE;
}


static gboolean
_encode_one_frame (GstVulkanH265Enc * self, GstVideoCodecFrame * gst_frame)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  GstVulkanH265EncodeFrame *list0[16] = { NULL, };
  guint list0_num = 0;
  GstVulkanH265EncodeFrame *list1[16] = { NULL, };
  guint list1_num = 0;
  gint i;
  GstVulkanH265EncodeFrame *frame;


  g_return_val_if_fail (gst_frame, FALSE);

  frame = _enc_frame (gst_frame);

  if (self->aud && !_add_aud (self, frame))
    return FALSE;

  /* Repeat the SPS for IDR. */
  if (frame->poc == 0) {
    _add_vulkan_params_header (self, frame);
  }

  /* Non I frame, construct reference list. */
  if (frame->type != GST_H265_I_SLICE) {
    GstVulkanH265EncodeFrame *vaf;
    GstVideoCodecFrame *f;

    for (i = g_queue_get_length (&base->ref_list) - 1; i >= 0; i--) {
      f = g_queue_peek_nth (&base->ref_list, i);
      vaf = _enc_frame (f);
      if (vaf->poc > frame->poc)
        continue;

      list0[list0_num] = vaf;
      list0_num++;
    }

    /* reorder to select the most nearest forward frames. */
    g_qsort_with_data (list0, list0_num, sizeof (gpointer),
        (GCompareDataFunc) _poc_des_compare, NULL);

    if (list0_num > base->gop.ref_num_list0)
      list0_num = base->gop.ref_num_list0;
  }

  if (frame->type == GST_H265_B_SLICE) {
    GstVulkanH265EncodeFrame *vaf;
    GstVideoCodecFrame *f;

    for (i = 0; i < g_queue_get_length (&base->ref_list); i++) {
      f = g_queue_peek_nth (&base->ref_list, i);
      vaf = _enc_frame (f);
      if (vaf->poc < frame->poc)
        continue;

      list1[list1_num] = vaf;
      list1_num++;
    }

    /* reorder to select the most nearest backward frames. */
    g_qsort_with_data (list1, list1_num, sizeof (gpointer),
        (GCompareDataFunc) _poc_asc_compare, NULL);

    if (list1_num > base->gop.ref_num_list1)
      list1_num = base->gop.ref_num_list1;
  }

  g_assert (list0_num + list1_num <= base->gop.num_ref_frames);

  if (self->cc) {
    /* CC errors are not fatal */
    _add_sei_cc (self, gst_frame);
  }

  return _encode_one_vulkan_frame (self, frame, list0, list0_num, list1,
      list1_num);
}

static gboolean
gst_vulkan_h265enc_flush (GstVideoEncoder * venc)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (venc);
  GstH265Encoder *base = GST_H265_ENCODER (self);

  /* begin from an IDR after flush. */
  base->gop.cur_frame_index = 0;
  base->gop.cur_frame_num = 0;

  return GST_VIDEO_ENCODER_CLASS (parent_class)->flush (venc);
}

static void
gst_vulkan_h265enc_prepare_output (GstH265Encoder * base,
    GstVideoCodecFrame * frame)
{
  GstVulkanH265EncodeFrame *frame_enc;
  GstMapInfo info;

  frame_enc = _enc_frame (frame);

  frame->output_buffer = gst_buffer_ref (frame_enc->picture->out_buffer);

  frame->pts =
      base->start_pts + base->frame_duration * frame_enc->total_frame_count;
  /* The PTS should always be later than the DTS. */
  frame->dts = base->start_pts + base->frame_duration *
      ((gint64) base->output_frame_count -
      (gint64) base->gop.num_reorder_frames);
  base->output_frame_count++;
  frame->duration = base->frame_duration;

  gst_buffer_map (frame->output_buffer, &info, GST_MAP_READ);
  GST_MEMDUMP ("output buffer", info.data, info.size);
  gst_buffer_unmap (frame->output_buffer, &info);
}

static gint
_sort_by_frame_num (gconstpointer a, gconstpointer b, gpointer user_data)
{
  GstVulkanH265EncodeFrame *frame1 = _enc_frame ((GstVideoCodecFrame *) a);
  GstVulkanH265EncodeFrame *frame2 = _enc_frame ((GstVideoCodecFrame *) b);

  g_assert (frame1->frame_num != frame2->frame_num);

  return frame1->frame_num - frame2->frame_num;
}

static GstVideoCodecFrame *
_find_unused_reference_frame (GstVulkanH265Enc * self,
    GstVulkanH265EncodeFrame * frame)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  GstVulkanH265EncodeFrame *b_vaframe;
  GstVideoCodecFrame *b_frame;
  guint i;

  /* We still have more space. */
  if (g_queue_get_length (&base->ref_list) < base->gop.num_ref_frames)
    return NULL;

  /* Not b_pyramid, sliding window is enough. */
  if (!base->gop.b_pyramid)
    return g_queue_peek_head (&base->ref_list);

  /* I/P frame, just using sliding window. */
  if (frame->type != GST_H265_B_SLICE)
    return g_queue_peek_head (&base->ref_list);

  /* Choose the B frame with lowest POC. */
  b_frame = NULL;
  b_vaframe = NULL;
  for (i = 0; i < g_queue_get_length (&base->ref_list); i++) {
    GstVulkanH265EncodeFrame *vaf;
    GstVideoCodecFrame *f;

    f = g_queue_peek_nth (&base->ref_list, i);
    vaf = _enc_frame (f);
    if (vaf->type != GST_H265_B_SLICE)
      continue;

    if (!b_frame) {
      b_frame = f;
      b_vaframe = _enc_frame (b_frame);
      continue;
    }

    b_vaframe = _enc_frame (b_frame);
    g_assert (vaf->poc != b_vaframe->poc);
    if (vaf->poc < b_vaframe->poc) {
      b_frame = f;
      b_vaframe = _enc_frame (b_frame);
    }
  }

  /* No B frame as ref. */
  if (!b_frame)
    return g_queue_peek_head (&base->ref_list);

  if (b_frame != g_queue_peek_head (&base->ref_list)) {
    b_vaframe = _enc_frame (b_frame);
    frame->unused_for_reference_pic_num = b_vaframe->frame_num;
    GST_LOG_OBJECT (self, "The frame with POC: %d, pic_num %d will be"
        " replaced by the frame with POC: %d, pic_num %d explicitly by"
        " using memory_management_control_operation=1",
        b_vaframe->poc, b_vaframe->frame_num, frame->poc, frame->frame_num);
  }

  return b_frame;
}

static GstFlowReturn
gst_vulkan_h265enc_encode_frame (GstH265Encoder * base,
    GstVideoCodecFrame * gst_frame, gboolean is_last)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (base);
  GstVulkanH265EncodeFrame *frame;
  GstVideoCodecFrame *unused_ref = NULL;

  frame = _enc_frame (gst_frame);
  frame->last_frame = is_last;

  g_assert (frame->picture == NULL);
  frame->picture = gst_vulkan_encode_picture_new (self->encoder,
      gst_frame->input_buffer, base->width, base->height,
      frame->is_ref, frame->type != GST_H265_I_SLICE);

  if (!frame->picture) {
    GST_ERROR_OBJECT (self, "Failed to create the encode picture");
    return GST_FLOW_ERROR;
  }

  frame->picture->pic_order_cnt = frame->poc;
  frame->picture->pic_num = frame->frame_num;

  if (frame->is_ref)
    unused_ref = _find_unused_reference_frame (self, frame);

  if (!_encode_one_frame (self, gst_frame)) {
    GST_ERROR_OBJECT (self, "Failed to encode the frame");
    return GST_FLOW_ERROR;
  }

  g_queue_push_tail (&base->output_list, gst_video_codec_frame_ref (gst_frame));

  if (frame->is_ref) {
    if (unused_ref) {
      if (!g_queue_remove (&base->ref_list, unused_ref))
        g_assert_not_reached ();

      gst_video_codec_frame_unref (unused_ref);
    }

    /* Add it into the reference list. */
    g_queue_push_tail (&base->ref_list, gst_video_codec_frame_ref (gst_frame));
    g_queue_sort (&base->ref_list, _sort_by_frame_num, NULL);

    g_assert (g_queue_get_length (&base->ref_list) <= base->gop.num_ref_frames);
  }

  return GST_FLOW_OK;
}

static gboolean
gst_vulkan_h265enc_new_frame (GstH265Encoder * base, GstVideoCodecFrame * frame)
{
  GstVulkanH265EncodeFrame *frame_in;

  frame_in = gst_vulkan_h265_encode_frame_new ();
  frame_in->frame_num = base->input_frame_count;
  frame_in->total_frame_count = base->input_frame_count++;
  gst_video_codec_frame_set_user_data (frame, frame_in,
      gst_vulkan_h265_encode_frame_free);

  return TRUE;
}

static gboolean
_push_one_frame (GstH265Encoder * base, GstVideoCodecFrame * gst_frame,
    gboolean last)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (base);
  GstVulkanH265EncodeFrame *frame;

  g_return_val_if_fail (base->gop.cur_frame_index <= base->gop.idr_period,
      FALSE);

  if (gst_frame) {
    /* Begin a new GOP, should have a empty reorder_list. */
    if (base->gop.cur_frame_index == base->gop.idr_period) {
      g_assert (g_queue_is_empty (&base->reorder_list));
      base->gop.cur_frame_index = 0;
      base->gop.cur_frame_num = 0;
    }

    frame = _enc_frame (gst_frame);
    frame->poc =
        ((base->gop.cur_frame_index * 2) % base->gop.max_pic_order_cnt);

    if (base->gop.cur_frame_index == 0) {
      g_assert (frame->poc == 0);
      GST_LOG_OBJECT (self, "system_frame_number: %d, an IDR frame, starts"
          " a new GOP", gst_frame->system_frame_number);

      g_queue_clear_full (&base->ref_list,
          (GDestroyNotify) gst_video_codec_frame_unref);

      GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (gst_frame);
    }

    frame->type = base->gop.frame_types[base->gop.cur_frame_index].slice_type;
    frame->is_ref = base->gop.frame_types[base->gop.cur_frame_index].is_ref;
    frame->pyramid_level =
        base->gop.frame_types[base->gop.cur_frame_index].pyramid_level;
    frame->left_ref_poc_diff =
        base->gop.frame_types[base->gop.cur_frame_index].left_ref_poc_diff;
    frame->right_ref_poc_diff =
        base->gop.frame_types[base->gop.cur_frame_index].right_ref_poc_diff;

    if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (gst_frame)) {
      GST_DEBUG_OBJECT (self, "system_frame_number: %d, a force key frame,"
          " promote its type from %s to %s", gst_frame->system_frame_number,
          gst_h265_encoder_slice_type_name (frame->type),
          gst_h265_encoder_slice_type_name (GST_H265_I_SLICE));
      frame->type = GST_H265_I_SLICE;
      frame->is_ref = TRUE;
    }

    GST_LOG_OBJECT (self, "Push frame, system_frame_number: %d, poc %d, "
        "frame type %s", gst_frame->system_frame_number, frame->poc,
        gst_h265_encoder_slice_type_name (frame->type));

    base->gop.cur_frame_index++;
    g_queue_push_tail (&base->reorder_list,
        gst_video_codec_frame_ref (gst_frame));
  }

  /* ensure the last one a non-B and end the GOP. */
  if (last && base->gop.cur_frame_index < base->gop.idr_period) {
    GstVideoCodecFrame *last_frame;

    /* Ensure next push will start a new GOP. */
    base->gop.cur_frame_index = base->gop.idr_period;

    if (!g_queue_is_empty (&base->reorder_list)) {
      last_frame = g_queue_peek_tail (&base->reorder_list);
      frame = _enc_frame (last_frame);
      if (frame->type == GST_H265_B_SLICE) {
        frame->type = GST_H265_P_SLICE;
        frame->is_ref = TRUE;
      }
    }
  }

  return TRUE;
}

struct RefFramesCount
{
  gint poc;
  guint num;
};

static void
_count_backward_ref_num (gpointer data, gpointer user_data)
{
  GstVulkanH265EncodeFrame *frame = _enc_frame (data);
  struct RefFramesCount *count = (struct RefFramesCount *) user_data;

  g_assert (frame->poc != count->poc);
  if (frame->poc > count->poc)
    count->num++;
}

static GstVideoCodecFrame *
_pop_pyramid_b_frame (GstVulkanH265Enc * self)
{
  GstH265Encoder *base = GST_H265_ENCODER (self);
  guint i;
  gint index = -1;
  GstVulkanH265EncodeFrame *b_vkframe;
  GstVideoCodecFrame *b_frame;
  struct RefFramesCount count;

  g_assert (base->gop.ref_num_list1 == 1);

  b_frame = NULL;
  b_vkframe = NULL;

  /* Find the lowest level with smallest poc. */
  for (i = 0; i < g_queue_get_length (&base->reorder_list); i++) {
    GstVulkanH265EncodeFrame *vkf;
    GstVideoCodecFrame *f;

    f = g_queue_peek_nth (&base->reorder_list, i);

    if (!b_frame) {
      b_frame = f;
      b_vkframe = _enc_frame (b_frame);
      index = i;
      continue;
    }

    vkf = _enc_frame (f);
    if (b_vkframe->pyramid_level < vkf->pyramid_level) {
      b_frame = f;
      b_vkframe = vkf;
      index = i;
      continue;
    }

    if (b_vkframe->poc > vkf->poc) {
      b_frame = f;
      b_vkframe = vkf;
      index = i;
    }
  }

again:
  /* Check whether its refs are already poped. */
  g_assert (b_vkframe->left_ref_poc_diff != 0);
  g_assert (b_vkframe->right_ref_poc_diff != 0);
  for (i = 0; i < g_queue_get_length (&base->reorder_list); i++) {
    GstVulkanH265EncodeFrame *vkf;
    GstVideoCodecFrame *f;

    f = g_queue_peek_nth (&base->reorder_list, i);

    if (f == b_frame)
      continue;

    vkf = _enc_frame (f);
    if (vkf->poc == b_vkframe->poc + b_vkframe->left_ref_poc_diff
        || vkf->poc == b_vkframe->poc + b_vkframe->right_ref_poc_diff) {
      b_frame = f;
      b_vkframe = vkf;
      index = i;
      goto again;
    }
  }

  /* Ensure we already have enough backward refs */
  count.num = 0;
  count.poc = b_vkframe->poc;
  g_queue_foreach (&base->ref_list, (GFunc) _count_backward_ref_num, &count);
  if (count.num >= base->gop.ref_num_list1) {
    GstVideoCodecFrame *f;

    /* it will unref at pop_frame */
    f = g_queue_pop_nth (&base->reorder_list, index);
    g_assert (f == b_frame);
  } else {
    b_frame = NULL;
  }

  return b_frame;
}

static gboolean
_pop_one_frame (GstH265Encoder * base, GstVideoCodecFrame ** out_frame)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (base);
  GstVulkanH265EncodeFrame *vkframe;
  GstVideoCodecFrame *frame;
  struct RefFramesCount count;

  g_return_val_if_fail (base->gop.cur_frame_index <= base->gop.idr_period,
      FALSE);

  *out_frame = NULL;

  if (g_queue_is_empty (&base->reorder_list))
    return TRUE;

  /* Return the last pushed non-B immediately. */
  frame = g_queue_peek_tail (&base->reorder_list);
  vkframe = _enc_frame (frame);
  if (vkframe->type != GST_H265_B_SLICE) {
    frame = g_queue_pop_tail (&base->reorder_list);
    goto get_one;
  }

  if (base->gop.b_pyramid) {
    frame = _pop_pyramid_b_frame (self);
    if (frame == NULL)
      return TRUE;
    goto get_one;
  }

  g_assert (base->gop.ref_num_list1 > 0);

  /* If GOP end, pop anyway. */
  if (base->gop.cur_frame_index == base->gop.idr_period) {
    frame = g_queue_pop_head (&base->reorder_list);
    goto get_one;
  }

  /* Ensure we already have enough backward refs */
  frame = g_queue_peek_head (&base->reorder_list);
  vkframe = _enc_frame (frame);
  count.num = 0;
  count.poc = vkframe->poc;
  g_queue_foreach (&base->ref_list, _count_backward_ref_num, &count);
  if (count.num >= base->gop.ref_num_list1) {
    frame = g_queue_pop_head (&base->reorder_list);
    goto get_one;
  }

  return TRUE;

get_one:
  g_assert (base->gop.cur_frame_num < base->gop.max_frame_num);

  vkframe = _enc_frame (frame);
  vkframe->frame_num = base->gop.cur_frame_num;

  /* Add the frame number for ref frames. */
  if (vkframe->is_ref)
    base->gop.cur_frame_num++;

  if (vkframe->frame_num == 0)
    base->gop.total_idr_count++;

  if (base->gop.b_pyramid && vkframe->type == GST_H265_B_SLICE) {
    GST_LOG_OBJECT (self, "pop a pyramid B frame with system_frame_number:"
        " %d, poc: %d, frame num: %d, is_ref: %s, level %d",
        frame->system_frame_number, vkframe->poc, vkframe->frame_num,
        vkframe->is_ref ? "true" : "false", vkframe->pyramid_level);
  } else {
    GST_LOG_OBJECT (self, "pop a frame with system_frame_number: %d,"
        " frame type: %s, poc: %d, frame num: %d, is_ref: %s",
        frame->system_frame_number,
        gst_h265_encoder_slice_type_name (vkframe->type), vkframe->poc,
        vkframe->frame_num, vkframe->is_ref ? "true" : "false");
  }

  /* unref frame popped from queue or pyramid b_frame */
  gst_video_codec_frame_unref (frame);
  *out_frame = frame;
  return TRUE;
}

static gboolean
gst_vulkan_h265enc_reorder_frame (GstH265Encoder * base,
    GstVideoCodecFrame * frame, gboolean bump_all,
    GstVideoCodecFrame ** out_frame)
{
  if (!_push_one_frame (base, frame, bump_all)) {
    GST_ERROR_OBJECT (base, "Failed to push the input frame"
        " system_frame_number: %d into the reorder list",
        frame->system_frame_number);

    *out_frame = NULL;
    return FALSE;
  }

  if (!_pop_one_frame (base, out_frame)) {
    GST_ERROR_OBJECT (base, "Failed to pop the frame from the reorder list");
    *out_frame = NULL;
    return FALSE;
  }

  return TRUE;
}

static void
gst_vulkan_h265enc_init (GstVulkanH265Enc * self)
{
  gst_vulkan_buffer_memory_init_once ();
  self->prop.num_slices = 1;
  self->prop.min_qp = 1;
  self->prop.max_qp = 51;
  self->prop.qp_i = 26;
  self->prop.qp_p = 26;
  self->prop.qp_b = 26;
  self->prop.num_ref_frames = 3;
  self->prop.quality_level = 0;
  self->prop.aud = FALSE;
  self->prop.cc = FALSE;
}

static void
gst_vulkan_h265enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (object);

  switch (prop_id) {
    case PROP_RATE_CONTROL:
      g_object_set (self->encoder, "rate-control", g_value_get_enum (value),
          NULL);
      break;
    case PROP_VIDEO_USAGE:
      g_object_set (self->encoder, "video-usage", g_value_get_enum (value),
          NULL);
      break;
    case PROP_VIDEO_CONTENT:
      g_object_set (self->encoder, "video-content", g_value_get_enum (value),
          NULL);
      break;
    case PROP_TUNING_MODE:
      g_object_set (self->encoder, "tuning-mode", g_value_get_enum (value),
          NULL);
      break;
    case PROP_NUM_SLICES:
      self->prop.num_slices = g_value_get_uint (value);
      break;
    case PROP_MIN_QP:
      self->prop.min_qp = g_value_get_uint (value);
      break;
    case PROP_MAX_QP:
      self->prop.max_qp = g_value_get_uint (value);
      break;
    case PROP_QP_I:
      self->prop.qp_i = g_value_get_uint (value);
      break;
    case PROP_QP_P:
      self->prop.qp_p = g_value_get_uint (value);
      break;
    case PROP_QP_B:
      self->prop.qp_b = g_value_get_uint (value);
      break;
    case PROP_AVERAGE_BITRATE:
      if (self->encoder)
        g_object_set (self->encoder, "average-bitrate",
            g_value_get_uint (value), NULL);
      break;
    case PROP_QUALITY_LEVEL:
      self->prop.quality_level = g_value_get_uint (value);
      if (self->encoder)
        g_object_set (self->encoder, "quality-level", g_value_get_uint (value),
            NULL);
      break;
    case PROP_NUM_REF_FRAMES:
      self->prop.num_ref_frames = g_value_get_uint (value);
      break;
    case PROP_AUD:
      self->prop.aud = g_value_get_boolean (value);
      break;
    case PROP_CC:
      self->prop.cc = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_h265enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (object);

  switch (prop_id) {
    case PROP_RATE_CONTROL:
      guint rate_control = 0;
      if (self->encoder)
        g_object_get (self->encoder, "rate-control", &rate_control, NULL);
      g_value_set_enum (value, rate_control);
      break;
    case PROP_VIDEO_USAGE:
      guint video_usage = 0;
      if (self->encoder)
        g_object_get (self->encoder, "video-usage", &video_usage, NULL);
      g_value_set_enum (value, video_usage);
      break;
    case PROP_VIDEO_CONTENT:
      guint video_content = 0;
      if (self->encoder)
        g_object_get (self->encoder, "video-content", &video_content, NULL);
      g_value_set_enum (value, video_content);
      break;
    case PROP_TUNING_MODE:
      guint tuning_mode = 0;
      if (self->encoder)
        g_object_get (self->encoder, "tuning-mode", &tuning_mode, NULL);
      g_value_set_enum (value, tuning_mode);
      break;
    case PROP_NUM_SLICES:
      g_value_set_uint (value, self->prop.num_slices);
      break;
    case PROP_MIN_QP:
      g_value_set_uint (value, self->prop.min_qp);
      break;
    case PROP_MAX_QP:
      g_value_set_uint (value, self->prop.max_qp);
      break;
    case PROP_QP_I:
      g_value_set_uint (value, self->prop.qp_i);
      break;
    case PROP_QP_P:
      g_value_set_uint (value, self->prop.qp_p);
      break;
    case PROP_QP_B:
      g_value_set_uint (value, self->prop.qp_b);
      break;
    case PROP_AVERAGE_BITRATE:
      guint average_bitrate = 0;
      if (self->encoder)
        g_object_get (self->encoder, "average-bitrate", &average_bitrate, NULL);
      g_value_set_uint (value, average_bitrate);
      break;
    case PROP_QUALITY_LEVEL:
      guint quality_level = 0;
      if (self->encoder)
        g_object_get (self->encoder, "quality-level", &quality_level, NULL);
      g_value_set_uint (value, quality_level);
      break;
    case PROP_NUM_REF_FRAMES:
      g_value_set_uint (value, self->prop.num_ref_frames);
      break;
    case PROP_AUD:
      g_value_set_boolean (value, self->prop.aud);
      break;
    case PROP_CC:
      g_value_set_boolean (value, self->prop.cc);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vulkan_h265enc_propose_allocation (GstVideoEncoder * venc, GstQuery * query)
{
  gboolean need_pool;
  GstCaps *caps, *profile_caps;
  GstVideoInfo info;
  guint size;
  GstBufferPool *pool = NULL;
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (venc);

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  /* the normal size of a frame */
  size = info.size;

  if (need_pool) {
    GstStructure *config;

    pool = gst_vulkan_image_buffer_pool_new (self->device);

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
    profile_caps = gst_vulkan_encoder_profile_caps (self->encoder);
    gst_vulkan_image_buffer_pool_config_set_encode_caps (config, profile_caps);
    gst_vulkan_image_buffer_pool_config_set_allocation_params (config,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
        VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (!gst_buffer_pool_set_config (pool, config)) {
      g_object_unref (pool);
      return FALSE;
    }
  }

  gst_query_add_allocation_pool (query, pool, size, 1, 0);
  if (pool)
    g_object_unref (pool);

  return TRUE;
}

static gboolean
gst_vulkan_h265enc_max_num_reference (GstH265Encoder * base, guint32 * list0,
    guint32 * list1)
{
  GstVulkanH265Enc *self = GST_VULKAN_H265ENC (base);
  GstVulkanVideoCapabilites enc_caps;

  if (!gst_vulkan_encoder_vk_caps (self->encoder, &enc_caps))
    return FALSE;

  *list0 = enc_caps.codec.h265enc.maxPPictureL0ReferenceCount;
  *list1 = enc_caps.codec.h265enc.maxL1ReferenceCount;

  return TRUE;
}

static void
gst_vulkan_h265enc_class_init (GstVulkanH265EncClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GstH265EncoderClass *h265encoder_class = GST_H265_ENCODER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gint n_props = PROP_MAX;
  GParamFlags param_flags =
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT;
  gst_element_class_set_metadata (element_class, "Vulkan H.264 encoder",
      "Codec/Encoder/Video/Hardware", "A H.264 video encoder based on Vulkan",
      "Stéphane Cerveau <scerveau@igalia.com>");

  gst_element_class_add_static_pad_template (element_class,
      &gst_vulkan_h265enc_sink_template);

  gst_element_class_add_static_pad_template (element_class,
      &gst_vulkan_h265enc_src_template);


  gobject_class->set_property = gst_vulkan_h265enc_set_property;
  gobject_class->get_property = gst_vulkan_h265enc_get_property;

  properties[PROP_RATE_CONTROL] =
      g_param_spec_enum ("rate-control", "Vulkan rate control",
      "Choose the vulkan rate control", GST_TYPE_VULKAN_H265_RATE_CONTROL,
      VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  properties[PROP_VIDEO_USAGE] =
      g_param_spec_enum ("vulkan-usage", "Vulkan encode usage",
      "Choose the vulkan encoding usage", GST_TYPE_VULKAN_H265_ENCODE_USAGE,
      VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  properties[PROP_VIDEO_CONTENT] =
      g_param_spec_enum ("vulkan-content", "Vulkan encode content",
      "Choose the vulkan encoding content", GST_TYPE_VULKAN_H265_ENCODE_CONTENT,
      VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  properties[PROP_TUNING_MODE] =
      g_param_spec_enum ("tuning-mode", "Vulkan encode tuning",
      "Choose the vulkan encoding tuning",
      GST_TYPE_VULKAN_H265_ENCODE_TUNING_MODE,
      VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    /**
   * GstVkH265Enc:num-slices:
   *
   * The number of slices per frame.
   */
  properties[PROP_NUM_SLICES] = g_param_spec_uint ("num-slices",
      "Number of Slices", "Number of slices per frame", 1, 200, 1, param_flags);

  /**
   * GstVkH265Enc:max-qp:
   *
   * The maximum quantizer value.
   */
  properties[PROP_MAX_QP] = g_param_spec_uint ("max-qp", "Maximum QP",
      "Maximum quantizer value for each frame", 0, 51, 51, param_flags);

  /**
   * GstVkH265Enc:min-qp:
   *
   * The minimum quantizer value.
   */
  properties[PROP_MIN_QP] = g_param_spec_uint ("min-qp", "Minimum QP",
      "Minimum quantizer value for each frame", 0, 51, 1, param_flags);

  /**
   * GstVkH265Enc:qpi:
   *
   * The quantizer value for I frame.
   *
   * In CQP mode, it specifies the QP of I frame, in other mode, it specifies
   * the init QP of all frames.
   */
  properties[PROP_QP_I] = g_param_spec_uint ("qpi", "I Frame QP",
      "The quantizer value for I frame. In CQP mode, it specifies the QP of I "
      "frame, in other mode, it specifies the init QP of all frames", 0, 51, 26,
      param_flags | GST_PARAM_MUTABLE_PLAYING);

  /**
   * GstVkH265Enc:qpp:
   *
   * The quantizer value for P frame. Available only in CQP mode.
   */
  properties[PROP_QP_P] = g_param_spec_uint ("qpp",
      "The quantizer value for P frame",
      "The quantizer value for P frame. Available only in CQP mode",
      0, 51, 26, param_flags | GST_PARAM_MUTABLE_PLAYING);

  /**
   * GstVkH265Enc:qpb:
   *
   * The quantizer value for B frame. Available only in CQP mode.
   */
  properties[PROP_QP_B] = g_param_spec_uint ("qpb",
      "The quantizer value for B frame",
      "The quantizer value for B frame. Available only in CQP mode",
      0, 51, 26, param_flags | GST_PARAM_MUTABLE_PLAYING);
  /**
   * GstVkH265Enc:average-bitrate:
   *
   * The average bitrate to be used by the encoder.
   *
   */
  properties[PROP_AVERAGE_BITRATE] =
      g_param_spec_uint ("average-bitrate", "Vulkan encode average bitrate",
      "Choose the vulkan encoding bitrate", 0, UINT_MAX,
      DEFAULT_H265_AVERAGE_BIRATE, param_flags);

  /**
   * GstVkH265Enc:quality-level:
   *
   * The quality level to be used by the encoder.
   *
   */
  properties[PROP_QUALITY_LEVEL] =
      g_param_spec_uint ("quality-level", "Vulkan encode quality level",
      "Choose the vulkan encoding quality level", 0, UINT_MAX, 0, param_flags);
  /**
   * GstVkH265Enc:ref-frames:
   *
   * The number of reference frames.
   * FIXME: Se the right max num reference frames in Vulkan
   */
  properties[PROP_NUM_REF_FRAMES] = g_param_spec_uint ("ref-frames",
      "Number of Reference Frames",
      "Number of reference frames, including both the forward and the backward",
      0, 15, 3, param_flags);
  /**
   * GstVkH265Enc:aud:
   *
   * Insert the AU (Access Unit) delimeter for each frame.
   */
  properties[PROP_AUD] = g_param_spec_boolean ("aud", "Insert AUD",
      "Insert AU (Access Unit) delimeter for each frame", FALSE, param_flags);
  /**
   * GstVkH265Enc:cc-insert:
   *
   * Closed Caption Insert mode. Only CEA-708 RAW format is supported for now.
   */
  properties[PROP_CC] = g_param_spec_boolean ("cc-insert",
      "Insert Closed Captions",
      "Insert CEA-708 Closed Captions",
      FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT);

  g_object_class_install_properties (gobject_class, n_props, properties);

  encoder_class->open = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_open);
  encoder_class->close = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_close);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_stop);
  encoder_class->src_query = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_src_query);
  encoder_class->sink_query = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_sink_query);
  encoder_class->flush = GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_flush);
  encoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_propose_allocation);

  h265encoder_class->new_frame =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_new_frame);
  h265encoder_class->encode_frame =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_encode_frame);
  h265encoder_class->reorder_frame =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_reorder_frame);
  h265encoder_class->prepare_output =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_prepare_output);
  h265encoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_set_format);
  h265encoder_class->max_num_reference =
      GST_DEBUG_FUNCPTR (gst_vulkan_h265enc_max_num_reference);
}
