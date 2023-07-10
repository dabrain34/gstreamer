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
 * SECTION:element-vkh264enc
 * @title: vkh264enc
 * @short_description: A Vulkan based H264 video encoder
 *
 * vkh264enc encodes raw video surfaces into H.264 bitstreams using
 * Vulkan
 *
 *
 * ## Example launch line
 * ```
 * gst-launch-1.0 videotestsrc num-buffers=60 ! timeoverlay ! vulkanupload ! vulkanh264enc ! h264parse ! mp4mux ! filesink location=test.mp4
 * ```
 *
 * Since: 1.24
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vkh264enc.h"

#include <gst/codecparsers/gsth264bitwriter.h>

#include "gstvulkanelements.h"

#include <gst/vulkan/gstvkdevice.h>
#include <gst/vulkan/gstvkencoder.h>

static GstStaticPadTemplate gst_vulkan_h264enc_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
        (GST_CAPS_FEATURE_MEMORY_VULKAN_IMAGE, "NV12")));

static GstStaticPadTemplate gst_vulkan_h264enc_src_template =
GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "profile = { (string) baseline, (string) main, (string) high } ,"
        "stream-format = { (string) avc, (string) byte-stream }, "
        "alignment = (string) au"));

typedef struct _GstVulkanH264EncodeFrame GstVulkanH264EncodeFrame;

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

#define MAX_GOP_SIZE  1024

#define H264_MB_SIZE_ALIGNMENT 16

enum
{
  PROP_0,
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
  PROP_MAX
};

static GParamSpec *properties[PROP_MAX];

#define GST_TYPE_VULKAN_H264_ENCODE_USAGE (gst_vulkan_h264_enc_usage_get_type ())
static GType
gst_vulkan_h264_enc_usage_get_type (void)
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

    qtype = g_enum_register_static ("GstVulkanH264EncUsage", values);
  }
  return qtype;
}

#define GST_TYPE_VULKAN_H264_ENCODE_CONTENT (gst_vulkan_h264_enc_content_get_type ())
static GType
gst_vulkan_h264_enc_content_get_type (void)
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

    qtype = g_enum_register_static ("GstVulkanH264EncContent", values);
  }
  return qtype;
}

#define GST_TYPE_VULKAN_H264_ENCODE_TUNING_MODE (gst_vulkan_h264_enc_tuning_mode_get_type ())
static GType
gst_vulkan_h264_enc_tuning_mode_get_type (void)
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

    qtype = g_enum_register_static ("GstVulkanH264EncTuningMode", values);
  }
  return qtype;
}

typedef struct _VkH264Params
{
  StdVideoH264SequenceParameterSet sps;
  StdVideoH264PictureParameterSet pps;

  StdVideoH264HrdParameters hrd;
  StdVideoH264SequenceParameterSetVui vui;
  StdVideoH264ScalingLists scaling_lists;
  int32_t offset_for_ref_frame[255];
} VkH264Params;


struct _GstVulkanH264Enc
{
  /*< private > */
  GstH264Encoder parent;

  GstVideoCodecState *output_state;
  gint width;
  gint height;

  GstVulkanInstance *instance;
  GstVulkanDevice *device;
  GstVulkanQueue *queue;
  GstVulkanEncoder *encoder;

  gint dpb_size;

  GstVulkanVideoProfile profile;
  VkVideoEncodeH264CapabilitiesEXT caps;
  VkVideoEncodeH264RateControlInfoEXT rate_control;

  VkH264Params session_params;

  /* H264 fields */
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

  GstH264SPS sequence_hdr;
  GstH264PPS picture_hdr;
  GstH264SliceHdr slice_hdr;
};

struct _GstVulkanH264EncodeFrame
{
  GstVulkanEncodePicture *picture;
  GstH264SliceType type;
  gboolean is_ref;

  gint poc;
  gint frame_num;
  /* The pic_num will be marked as unused_for_reference, which is
   * replaced by this frame. -1 if we do not need to care about it
   * explicitly. */
  gint unused_for_reference_pic_num;

  /* The total frame count we handled. */
  guint total_frame_count;

  gboolean last_frame;

  StdVideoEncodeH264WeightTable slice_wt;
  StdVideoEncodeH264SliceHeader slice_hdr;
  VkVideoEncodeH264NaluSliceInfoEXT slice_info;
  VkVideoEncodeH264RateControlInfoEXT rc_info;
  VkVideoEncodeH264RateControlLayerInfoEXT rc_layer_info;
  VkVideoEncodeH264PictureInfoEXT enc_pic_info;
  VkVideoDecodeH264DpbSlotInfoKHR dpb_slot_info;

  StdVideoEncodeH264PictureInfo pic_info;
  StdVideoDecodeH264ReferenceInfo ref_info;
  StdVideoEncodeH264ReferenceListsInfo ref_list_info;
};

GST_DEBUG_CATEGORY_STATIC (gst_vulkan_h264enc_debug);
#define GST_CAT_DEFAULT gst_vulkan_h264enc_debug

#define gst_vulkan_h264enc_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanH264Enc, gst_vulkan_h264enc,
    GST_TYPE_H264_ENCODER, GST_DEBUG_CATEGORY_INIT (gst_vulkan_h264enc_debug,
        "vulkanh264enc", 0, "Vulkan H.264 enccoder"));
GST_ELEMENT_REGISTER_DEFINE_WITH_CODE (vulkanh264enc, "vulkanh264enc",
    GST_RANK_NONE, GST_TYPE_VULKAN_H264ENC, vulkan_element_init (plugin));

static GstVulkanH264EncodeFrame *
gst_vulkan_h264_encode_frame_new (void)
{
  GstVulkanH264EncodeFrame *frame;

  frame = g_new (GstVulkanH264EncodeFrame, 1);
  frame->type = GST_H264_I_SLICE;
  frame->is_ref = TRUE;
  frame->frame_num = 0;
  frame->unused_for_reference_pic_num = -1;
  frame->picture = NULL;
  frame->total_frame_count = 0;
  frame->last_frame = FALSE;

  return frame;
}

static void
gst_vulkan_h264_encode_frame_free (gpointer pframe)
{
  GstVulkanH264EncodeFrame *frame = pframe;
  g_clear_pointer (&frame->picture, gst_vulkan_encode_picture_free);
  g_free (frame);
}

static inline GstVulkanH264EncodeFrame *
_enc_frame (GstVideoCodecFrame * frame)
{
  GstVulkanH264EncodeFrame *enc_frame =
      gst_video_codec_frame_get_user_data (frame);
  g_assert (enc_frame);
  return enc_frame;
}

static void
gst_vulkan_h264enc_init_std_sps (GstVulkanH264Enc * self)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  VkVideoChromaSubsamplingFlagBitsKHR chroma_format;
  VkVideoComponentBitDepthFlagsKHR bit_depth_luma, bit_depth_chroma;

  StdVideoH264SequenceParameterSet *vksps = &self->session_params.sps;
  StdVideoH264SequenceParameterSetVui *vkvui = &self->session_params.vui;
  memset (vksps, 0, sizeof (StdVideoH264SequenceParameterSet));
  memset (vkvui, 0, sizeof (StdVideoH264SequenceParameterSet));

  gst_vulkan_video_get_chroma_info_from_format (base->input_state->info.
      finfo->format, &chroma_format, &bit_depth_luma, &bit_depth_chroma);

  const uint32_t mbAlignedWidth =
      (self->width + H264_MB_SIZE_ALIGNMENT - 1) & ~(H264_MB_SIZE_ALIGNMENT -
      1);
  const uint32_t mbAlignedHeight =
      (self->height + H264_MB_SIZE_ALIGNMENT - 1) & ~(H264_MB_SIZE_ALIGNMENT -
      1);

  vksps->flags.direct_8x8_inference_flag = 1u;
  vksps->flags.frame_mbs_only_flag = 1u;

  vksps->profile_idc = self->profile.codec.h264enc.stdProfileIdc;
  vksps->level_idc = self->level_idc;
  vksps->seq_parameter_set_id = 0u;
  vksps->chroma_format_idc =
      gst_vulkan_video_h264_chromat_from_format (base->input_state->info.
      finfo->format);

  vksps->bit_depth_luma_minus8 = bit_depth_luma - 8;
  vksps->bit_depth_chroma_minus8 = bit_depth_chroma - 8;
  vksps->log2_max_frame_num_minus4 = 0u;
  vksps->pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
  vksps->max_num_ref_frames = 1u;
  vksps->pic_width_in_mbs_minus1 = mbAlignedWidth / H264_MB_SIZE_ALIGNMENT - 1;
  vksps->pic_height_in_map_units_minus1 =
      mbAlignedHeight / H264_MB_SIZE_ALIGNMENT - 1;
  vksps->frame_crop_right_offset = mbAlignedWidth - self->width;
  vksps->frame_crop_bottom_offset = mbAlignedHeight - self->height;

  // This allows for picture order count values in the range [0, 255].
  vksps->log2_max_pic_order_cnt_lsb_minus4 = 4u;

  if (vksps->frame_crop_right_offset || vksps->frame_crop_bottom_offset) {
    vksps->flags.frame_cropping_flag = TRUE;
    if (vksps->chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420) {
      vksps->frame_crop_right_offset >>= 1;
      vksps->frame_crop_bottom_offset >>= 1;
    }
  }

  vkvui->flags = (StdVideoH264SpsVuiFlags) {
  .aspect_ratio_info_present_flag = TRUE,.timing_info_present_flag = TRUE,};

  // Set the VUI parameters
  vkvui->aspect_ratio_idc = 0xFF;
  vkvui->sar_width = GST_VIDEO_INFO_PAR_N (&base->input_state->info);
  vkvui->sar_height = GST_VIDEO_INFO_PAR_D (&base->input_state->info);
  vkvui->num_units_in_tick = GST_VIDEO_INFO_FPS_D (&base->input_state->info);
  vkvui->time_scale = GST_VIDEO_INFO_FPS_N (&base->input_state->info) * 2;

  vksps->flags.vui_parameters_present_flag = TRUE;
  vksps->pSequenceParameterSetVui = &self->session_params.vui;
}

static void
gst_vulkan_h264enc_init_std_pps (GstVulkanH264Enc * self)
{
  StdVideoH264PictureParameterSet *vkpps = &self->session_params.pps;
  memset (vkpps, 0, sizeof (StdVideoH264PictureParameterSet));

  // vkpps->flags.transform_8x8_mode_flag = 1u; // FIXME: This parameter blocks the session init
  vkpps->flags.constrained_intra_pred_flag = 0u;
  vkpps->flags.deblocking_filter_control_present_flag = 1u;
  vkpps->flags.entropy_coding_mode_flag = 1u;

  vkpps->seq_parameter_set_id = 0u;
  vkpps->pic_parameter_set_id = 0u;
  vkpps->num_ref_idx_l0_default_active_minus1 = 0u;
}

static gboolean
_init_packed_headers (GstVulkanH264Enc * self)
{
  // FIXME: Check the capability to generate packed headers from the implementation
  // See
  self->packed_headers = VULKAN_PACKED_HEADER_TYPE_SPS
      | VULKAN_PACKED_HEADER_TYPE_PPS;

  return TRUE;
}


static gboolean
gst_vulkan_h264enc_get_session_params (GstVulkanH264Enc * self,
    void **packed_params, gsize * size, gint sps_id, gint pps_id)
{
  VkVideoEncodeH264SessionParametersGetInfoEXT video_codec_session_info;
  gsize header_size;
     /* *INDENT-OFF* */
    video_codec_session_info = (VkVideoEncodeH264SessionParametersGetInfoEXT)
	  {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_EXT,
      .pNext = NULL,
      .writeStdSPS = (sps_id >= 0) ,
      .writeStdPPS = (pps_id >= 0),
      .stdSPSId = sps_id,
      .stdPPSId = pps_id,
	  };
    /* *INDENT-ON* */

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
gst_vulkan_h264enc_reset (GstVulkanH264Enc * self)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  self->num_slices = self->prop.num_slices;
  self->rc.min_qp = self->prop.min_qp;
  self->rc.max_qp = self->prop.max_qp;
  self->rc.qp_i = self->prop.qp_i;
  self->rc.qp_p = self->prop.qp_p;
  self->rc.qp_b = self->prop.qp_b;
  base->gop.num_ref_frames = self->prop.num_ref_frames;
}

static gboolean
gst_vulkan_h264enc_init_session (GstVulkanH264Enc * self)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (self);
  GError *err = NULL;
  GstVulkanEncoderParameters h264params;
  VkVideoEncodeH264SessionParametersAddInfoEXT params_add;
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
      gst_h264_encoder_get_profile_from_str
      (gst_vulkan_video_get_profile_from_caps (output_state->caps));

  /* *INDENT-OFF* */
  self->profile = (GstVulkanVideoProfile) {
  .profile = (VkVideoProfileInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
      .pNext = &self->profile.codec.h264enc,
      .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT,
      .chromaSubsampling = chroma_format,
      .chromaBitDepth = bit_depth_luma,
      .lumaBitDepth = bit_depth_chroma,
  },
  .codec.h264enc = (VkVideoEncodeH264ProfileInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_EXT,
      .pNext = NULL,
      .stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN,
    }
  };

  self->level_idc = gst_h264_encoder_get_level_limit (base);

  self->caps = (VkVideoEncodeH264CapabilitiesEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT
  };
  /* *INDENT-ON* */

  gst_vulkan_h264enc_init_std_sps (self);
  gst_vulkan_h264enc_init_std_pps (self);

  /* *INDENT-OFF* */
  params_add = (VkVideoEncodeH264SessionParametersAddInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT,
        .pStdSPSs = &self->session_params.sps,
        .stdSPSCount = 1,
        .pStdPPSs = &self->session_params.pps,
        .stdPPSCount = 1,
  };
  h264params.h264.params_create = (VkVideoEncodeH264SessionParametersCreateInfoEXT) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT,
        .maxStdSPSCount = 1,
        .maxStdPPSCount = 1,
        .pParametersAddInfo = &params_add,
  };
   /* *INDENT-ON* */

  if (!gst_vulkan_encoder_start (self->encoder, &self->profile, &h264params,
          &err)) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("Unable to start vulkan encoder with error %s", err->message), (NULL));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_vulkan_h264enc_open (GstVideoEncoder * encoder)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (encoder);

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
      VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT);

  return TRUE;
}

static gboolean
gst_vulkan_h264enc_close (GstVideoEncoder * encoder)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (encoder);

  gst_clear_object (&self->encoder);
  gst_clear_object (&self->queue);
  gst_clear_object (&self->device);
  gst_clear_object (&self->instance);

  return TRUE;
}

static gboolean
gst_vulkan_h264enc_stop (GstVideoEncoder * encoder)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (encoder);
  gst_vulkan_encoder_stop (self->encoder);
  return GST_VIDEO_ENCODER_CLASS (parent_class)->stop (encoder);
}

static gboolean
_query_context (GstVulkanH264Enc * self, GstQuery * query)
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
gst_vulkan_h264enc_src_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = _query_context (GST_VULKAN_H264ENC (encoder), query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->src_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_vulkan_h264enc_sink_query (GstVideoEncoder * encoder, GstQuery * query)
{
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
      ret = _query_context (GST_VULKAN_H264ENC (encoder), query);
      break;
    default:
      ret = GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (encoder, query);
      break;
  }

  return ret;
}

static gboolean
gst_vulkan_h264enc_set_format (GstH264Encoder * h264enc,
    GstVideoCodecState * state)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (h264enc);
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (self);
  GstCaps *outcaps;

  GST_DEBUG_OBJECT (self, "Set format");

  if (h264enc->input_state)
    gst_video_codec_state_unref (h264enc->input_state);
  h264enc->input_state = gst_video_codec_state_ref (state);

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

  gst_vulkan_h264enc_reset (self);

  _init_packed_headers (self);

  if (GST_VIDEO_ENCODER_CLASS (parent_class)->negotiate (encoder)) {
    return gst_vulkan_h264enc_init_session (self);
  }

  return FALSE;
}

/* STOP SCE IMPORT*/


static inline gboolean
_fill_sps (GstVulkanH264Enc * self,
    StdVideoH264SequenceParameterSet * seq_param)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  GstH264Profile profile = base->profile;
  guint32 constraint_set0_flag, constraint_set1_flag;
  guint32 constraint_set2_flag, constraint_set3_flag;
  guint32 max_dec_frame_buffering;

  /* let max_num_ref_frames <= MaxDpbFrames. */
  max_dec_frame_buffering =
      MIN (base->gop.num_ref_frames + 1 /* Last frame before bump */ ,
      16 /* DPB_MAX_SIZE */ );

  constraint_set0_flag = 0;
  constraint_set1_flag = 0;
  constraint_set2_flag = 0;
  constraint_set3_flag = 0;

  switch (profile) {
    case GST_H264_PROFILE_BASELINE:
      /* A.2.1 (baseline profile constraints) */
      constraint_set0_flag = 1;
      constraint_set1_flag = 1;
      break;
    case GST_H264_PROFILE_MAIN:
      profile = GST_H264_PROFILE_MAIN;
      /* A.2.2 (main profile constraints) */
      constraint_set1_flag = 1;
      break;
    default:
      return FALSE;
  }

  /* seq_scaling_matrix_present_flag not supported now */
  g_assert (seq_param->flags.seq_scaling_matrix_present_flag == 0);
  /* pic_order_cnt_type only support 0 now */
  g_assert (seq_param->pic_order_cnt_type == 0);
  /* only progressive frames encoding is supported now */
  g_assert (seq_param->flags.frame_mbs_only_flag);

  /* *INDENT-OFF* */
  GST_DEBUG_OBJECT (self, "filling SPS");
  self->sequence_hdr = (GstH264SPS) {
    .id = 0,
    .profile_idc = profile,
    .constraint_set0_flag = constraint_set0_flag,
    .constraint_set1_flag = constraint_set1_flag,
    .constraint_set2_flag = constraint_set2_flag,
    .constraint_set3_flag = constraint_set3_flag,
    .level_idc = self->level_idc,

    .chroma_format_idc = seq_param->chroma_format_idc,
    .bit_depth_luma_minus8 = seq_param->bit_depth_luma_minus8,
    .bit_depth_chroma_minus8 = seq_param->bit_depth_chroma_minus8,

    .log2_max_frame_num_minus4 = seq_param->log2_max_frame_num_minus4,
    .pic_order_cnt_type = seq_param->pic_order_cnt_type,
    .log2_max_pic_order_cnt_lsb_minus4 = seq_param->log2_max_pic_order_cnt_lsb_minus4,

    .num_ref_frames = seq_param->max_num_ref_frames,
    .gaps_in_frame_num_value_allowed_flag = 0,
    .pic_width_in_mbs_minus1 = seq_param->pic_width_in_mbs_minus1 - 1,
    .pic_height_in_map_units_minus1 =
        (seq_param->flags.frame_mbs_only_flag ?
            seq_param->pic_height_in_map_units_minus1 - 1 :
            seq_param->pic_height_in_map_units_minus1 / 2 - 1),
    .frame_mbs_only_flag = seq_param->flags.frame_mbs_only_flag,
    .mb_adaptive_frame_field_flag = 0,
    .direct_8x8_inference_flag =
        seq_param->flags.direct_8x8_inference_flag,
    .frame_cropping_flag = seq_param->flags.frame_cropping_flag,
    .frame_crop_left_offset = seq_param->frame_crop_left_offset,
    .frame_crop_right_offset = seq_param->frame_crop_right_offset,
    .frame_crop_top_offset = seq_param->frame_crop_top_offset,
    .frame_crop_bottom_offset = seq_param->frame_crop_bottom_offset,

    .vui_parameters_present_flag = seq_param->flags.vui_parameters_present_flag,
    .vui_parameters = {
      .aspect_ratio_info_present_flag =
          seq_param->pSequenceParameterSetVui->flags.aspect_ratio_info_present_flag,
      .aspect_ratio_idc = seq_param->pSequenceParameterSetVui->aspect_ratio_idc,
      .sar_width = seq_param->pSequenceParameterSetVui->sar_width,
      .sar_height = seq_param->pSequenceParameterSetVui->sar_height,
      .overscan_info_present_flag = 0,
      .overscan_appropriate_flag = 0,
      .chroma_loc_info_present_flag = 0,
      .timing_info_present_flag =
          seq_param->pSequenceParameterSetVui->flags.timing_info_present_flag,
      .num_units_in_tick = seq_param->pSequenceParameterSetVui->num_units_in_tick,
      .time_scale = seq_param->pSequenceParameterSetVui->time_scale,
      .fixed_frame_rate_flag = seq_param->pSequenceParameterSetVui->flags.fixed_frame_rate_flag,

      /* We do not write hrd and no need for buffering period SEI. */
      .nal_hrd_parameters_present_flag = 0,
      .vcl_hrd_parameters_present_flag = 0,
      //FIXME: to be confirmed
      .low_delay_hrd_flag = seq_param->pSequenceParameterSetVui->flags.vcl_hrd_parameters_present_flag,
      .pic_struct_present_flag = 1,
      .bitstream_restriction_flag =
          seq_param->pSequenceParameterSetVui->flags.bitstream_restriction_flag,
      // FIXME: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkVideoEncodeH264CapabilitiesEXT.html
      // .motion_vectors_over_pic_boundaries_flag =
      //     seq_param->pSequenceParameterSetVui->flags.motion_vectors_over_pic_boundaries_flag,
      .max_bytes_per_pic_denom = 2,
      .max_bits_per_mb_denom = 1,
      // FIXME: https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkVideoEncodeH264CapabilitiesEXT.html
      // .log2_max_mv_length_horizontal =
      //     seq_param->pSequenceParameterSetVui->flags.log2_max_mv_length_horizontal,
      // .log2_max_mv_length_vertical =
      //     seq_param->vui_fields.bits.log2_max_mv_length_vertical,
      .num_reorder_frames = base->gop.num_reorder_frames,
      .max_dec_frame_buffering = max_dec_frame_buffering,
    },
  };
  /* *INDENT-ON* */

  return TRUE;
}

static gboolean
_add_sequence_header (GstVulkanH264Enc * self, GstVulkanH264EncodeFrame * frame)
{
  guint size;
#define SPS_SIZE 4 + GST_ROUND_UP_8 (MAX_SPS_HDR_SIZE + MAX_VUI_PARAMS_SIZE + \
    2 * MAX_HRD_PARAMS_SIZE) / 8
  guint8 *packed_sps = g_malloc0 (SPS_SIZE);

  size = SPS_SIZE;
#undef SPS_SIZE
  if (gst_h264_bit_writer_sps (&self->sequence_hdr, TRUE, packed_sps,
          &size) != GST_H264_BIT_WRITER_OK) {
    GST_ERROR_OBJECT (self, "Failed to generate the sequence header");
    return FALSE;
  }

  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          packed_sps, size)) {
    GST_ERROR_OBJECT (self, "Failed to add the packed sequence header");
    return FALSE;
  }

  return TRUE;
}

static void
_fill_pps (GstVulkanH264Enc * self,
    StdVideoH264PictureParameterSet * pic_param, GstH264SPS * sps)
{
  /* *INDENT-OFF* */
  self->picture_hdr = (GstH264PPS) {
    .id = 0,
    .sequence = sps,
    .entropy_coding_mode_flag =
        pic_param->flags.entropy_coding_mode_flag,
    // FIXME: not defined in H264 ?
    // .pic_order_present_flag =
    //     pic_param->flags.pic_order_present_flag,
    .num_slice_groups_minus1 = 0,

    .num_ref_idx_l0_active_minus1 = pic_param->num_ref_idx_l0_default_active_minus1,
    .num_ref_idx_l1_active_minus1 = pic_param->num_ref_idx_l1_default_active_minus1,

    .weighted_pred_flag = pic_param->flags.weighted_pred_flag,
    .weighted_bipred_idc = pic_param->weighted_bipred_idc,
    .pic_init_qp_minus26 = pic_param->pic_init_qp_minus26,
    .pic_init_qs_minus26 = pic_param->pic_init_qs_minus26,
    .chroma_qp_index_offset = pic_param->chroma_qp_index_offset,
    .deblocking_filter_control_present_flag =
        pic_param->flags.deblocking_filter_control_present_flag,
    .constrained_intra_pred_flag =
        pic_param->flags.constrained_intra_pred_flag,
    .redundant_pic_cnt_present_flag =
        pic_param->flags.redundant_pic_cnt_present_flag,
    .transform_8x8_mode_flag =
        pic_param->flags.transform_8x8_mode_flag,
    /* unsupport scaling lists */
    .pic_scaling_matrix_present_flag = pic_param->flags.pic_scaling_matrix_present_flag,
    .second_chroma_qp_index_offset = pic_param->second_chroma_qp_index_offset,
  };
  /* *INDENT-ON* */
}

static gboolean
_add_picture_header (GstVulkanH264Enc * self, GstVulkanH264EncodeFrame * frame)
{

#define PPS_SIZE 4 + GST_ROUND_UP_8 (MAX_PPS_HDR_SIZE) / 8
  guint8 packed_pps[PPS_SIZE] = { 0, };
#undef PPS_SIZE
  guint size;

  size = sizeof (packed_pps);
  if (gst_h264_bit_writer_pps (&self->picture_hdr, TRUE, packed_pps,
          &size) != GST_H264_BIT_WRITER_OK) {
    GST_ERROR_OBJECT (self, "Failed to generate the picture header");
    return FALSE;
  }

  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          packed_pps, size)) {
    GST_ERROR_OBJECT (self, "Failed to add the packed picture header");
    return FALSE;
  }

  return TRUE;
}

static gboolean
_add_vulkan_params_header (GstVulkanH264Enc * self,
    GstVulkanH264EncodeFrame * frame)
{
  void *header = NULL;
  gsize header_size = 0;

  gst_vulkan_h264enc_get_session_params (self, &header, &header_size, 0, 0);
  GST_WARNING_OBJECT (self, "Adding params header of size %lu", header_size);
  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          header, header_size)) {
    GST_ERROR_OBJECT (self, "Failed to add the packed params header");
    return FALSE;
  }

  return TRUE;
}

// static gboolean
// _add_one_slice (GstVulkanH264Enc * self, GstVulkanH264EncodeFrame * frame,
//     gint start_mb, gint mb_size,
//     StdVideoEncodeH264SliceHeader * slice,
//     GstVulkanH264EncodeFrame * list0[16], guint list0_num,
//     GstVulkanH264EncodeFrame * list1[16], guint list1_num)
// {
//   int8_t slice_qp_delta = 0;
//   gint i;

//   /* *INDENT-OFF* */
//   // if (self->rc.rc_ctrl_mode == VA_RC_CQP) {
//   //   if (frame->type == GST_H264_P_SLICE) {
//   //     slice_qp_delta = self->rc.qp_p - self->rc.qp_i;
//   //   } else if (frame->type == GST_H264_B_SLICE) {
//   //     slice_qp_delta = (int8_t) (self->rc.qp_b - self->rc.qp_i);
//   //   }
//   //   g_assert (slice_qp_delta <= 51 && slice_qp_delta >= -51);
//   // }

//   *slice = (StdVideoEncodeH264SliceHeader*) {
//     .macroblock_address = start_mb,
//     .num_macroblocks = mb_size,
//     .macroblock_info = VA_INVALID_ID,
//     .slice_type = (uint8_t) frame->type,
//     /* Only one parameter set supported now. */
//     .pic_parameter_set_id = 0,
//     .idr_pic_id = self->gop.total_idr_count,
//     .pic_order_cnt_lsb = frame->poc,
//     /* Not support top/bottom. */
//     .delta_pic_order_cnt_bottom = 0,
//     .delta_pic_order_cnt[0] = 0,
//     .delta_pic_order_cnt[1] = 0,

//     .direct_spatial_mv_pred_flag = TRUE,
//     /* .num_ref_idx_active_override_flag = , */
//     /* .num_ref_idx_l0_active_minus1 = , */
//     /* .num_ref_idx_l1_active_minus1 = , */
//     /* Set the reference list later. */

//     .luma_log2_weight_denom = 0,
//     .chroma_log2_weight_denom = 0,
//     .luma_weight_l0_flag = 0,
//     .chroma_weight_l0_flag = 0,
//     .luma_weight_l1_flag = 0,
//     .chroma_weight_l1_flag = 0,

//     .cabac_init_idc = 0,
//     /* Just use picture default setting. */
//     .slice_qp_delta = slice_qp_delta,

//     .disable_deblocking_filter_idc = 0,
//     .slice_alpha_c0_offset_div2 = 2,
//     .slice_beta_offset_div2 = 2,
//   };
//   /* *INDENT-ON* */

//   if (frame->type == GST_H264_B_SLICE || frame->type == GST_H264_P_SLICE) {
//     slice->num_ref_idx_active_override_flag = (list0_num > 0 || list1_num > 0);
//     slice->num_ref_idx_l0_active_minus1 = list0_num > 0 ? list0_num - 1 : 0;
//     if (frame->type == GST_H264_B_SLICE)
//       slice->num_ref_idx_l1_active_minus1 = list1_num > 0 ? list1_num - 1 : 0;
//   }

//   i = 0;
//   if (frame->type != GST_H264_I_SLICE) {
//     for (; i < list0_num; i++) {
//       slice->RefPicList0[i].picture_id =
//           gst_va_encode_picture_get_reconstruct_surface (list0[i]->picture);
//       slice->RefPicList0[i].TopFieldOrderCnt = list0[i]->poc;
//       slice->RefPicList0[i].flags |= VA_PICTURE_H264_SHORT_TERM_REFERENCE;
//       slice->RefPicList0[i].frame_idx = list0[i]->frame_num;
//     }
//   }
//   for (; i < G_N_ELEMENTS (slice->RefPicList0); ++i) {
//     slice->RefPicList0[i].picture_id = VA_INVALID_SURFACE;
//     slice->RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
//   }

//   i = 0;
//   if (frame->type == GST_H264_B_SLICE) {
//     for (; i < list1_num; i++) {
//       slice->RefPicList1[i].picture_id =
//           gst_va_encode_picture_get_reconstruct_surface (list1[i]->picture);
//       slice->RefPicList1[i].TopFieldOrderCnt = list1[i]->poc;
//       slice->RefPicList1[i].flags |= VA_PICTURE_H264_SHORT_TERM_REFERENCE;
//       slice->RefPicList1[i].frame_idx = list1[i]->frame_num;
//     }
//   }
//   for (; i < G_N_ELEMENTS (slice->RefPicList1); ++i) {
//     slice->RefPicList1[i].picture_id = VA_INVALID_SURFACE;
//     slice->RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
//   }

//   if (!gst_va_encoder_add_param (self->encoder, frame->picture,
//           VAEncSliceParameterBufferType, slice,
//           sizeof (VAEncSliceParameterBufferH264))) {
//     GST_ERROR_OBJECT (self, "Failed to create the slice parameter");
//     return FALSE;
//   }

//   return TRUE;
// }

static gint
_poc_asc_compare (const GstVulkanH264EncodeFrame ** a,
    const GstVulkanH264EncodeFrame ** b)
{
  return (*a)->poc - (*b)->poc;
}

static gint
_poc_des_compare (const GstVulkanH264EncodeFrame ** a,
    const GstVulkanH264EncodeFrame ** b)
{
  return (*b)->poc - (*a)->poc;
}

static gint
_frame_num_asc_compare (const GstVulkanH264EncodeFrame ** a,
    const GstVulkanH264EncodeFrame ** b)
{
  return (*a)->frame_num - (*b)->frame_num;
}

static gint
_frame_num_des_compare (const GstVulkanH264EncodeFrame ** a,
    const GstVulkanH264EncodeFrame ** b)
{
  return (*b)->frame_num - (*a)->frame_num;
}

/* If all the pic_num in the same order, OK. */
static gboolean
_ref_list_need_reorder (GstVulkanH264EncodeFrame * list[16], guint list_num,
    gboolean is_asc)
{
  guint i;
  gint pic_num_diff;

  if (list_num <= 1)
    return FALSE;

  for (i = 1; i < list_num; i++) {
    pic_num_diff = list[i]->frame_num - list[i - 1]->frame_num;
    g_assert (pic_num_diff != 0);

    if (pic_num_diff > 0 && !is_asc)
      return TRUE;

    if (pic_num_diff < 0 && is_asc)
      return TRUE;
  }

  return FALSE;
}

static void
_insert_ref_pic_list_modification (GstH264SliceHdr * slice_hdr,
    GstVulkanH264EncodeFrame * list[16], guint list_num, gboolean is_asc)
{
  GstVulkanH264EncodeFrame *list_by_pic_num[16] = { NULL, };
  guint modification_num, i;
  GstH264RefPicListModification *ref_pic_list_modification = NULL;
  gint pic_num_diff, pic_num_lx_pred;

  memcpy (list_by_pic_num, list,
      sizeof (GstVulkanH264EncodeFrame *) * list_num);

  if (is_asc) {
    g_qsort_with_data (list_by_pic_num, list_num, sizeof (gpointer),
        (GCompareDataFunc) _frame_num_asc_compare, NULL);
  } else {
    g_qsort_with_data (list_by_pic_num, list_num, sizeof (gpointer),
        (GCompareDataFunc) _frame_num_des_compare, NULL);
  }

  modification_num = 0;
  for (i = 0; i < list_num; i++) {
    if (list_by_pic_num[i]->poc != list[i]->poc)
      modification_num = i + 1;
  }
  g_assert (modification_num > 0);

  if (is_asc) {
    slice_hdr->ref_pic_list_modification_flag_l1 = 1;
    slice_hdr->n_ref_pic_list_modification_l1 =
        modification_num + 1 /* The end operation. */ ;
    ref_pic_list_modification = slice_hdr->ref_pic_list_modification_l1;
  } else {
    slice_hdr->ref_pic_list_modification_flag_l0 = 1;
    slice_hdr->n_ref_pic_list_modification_l0 =
        modification_num + 1 /* The end operation. */ ;
    ref_pic_list_modification = slice_hdr->ref_pic_list_modification_l0;
  }

  pic_num_lx_pred = slice_hdr->frame_num;
  for (i = 0; i < modification_num; i++) {
    pic_num_diff = list[i]->frame_num - pic_num_lx_pred;
    /* For the nex loop. */
    pic_num_lx_pred = list[i]->frame_num;

    g_assert (pic_num_diff != 0);

    if (pic_num_diff > 0) {
      ref_pic_list_modification->modification_of_pic_nums_idc = 1;
      ref_pic_list_modification->value.abs_diff_pic_num_minus1 =
          pic_num_diff - 1;
    } else {
      ref_pic_list_modification->modification_of_pic_nums_idc = 0;
      ref_pic_list_modification->value.abs_diff_pic_num_minus1 =
          (-pic_num_diff) - 1;
    }

    ref_pic_list_modification++;
  }

  ref_pic_list_modification->modification_of_pic_nums_idc = 3;
}

static void
_insert_ref_pic_marking_for_unused_frame (GstH264SliceHdr * slice_hdr,
    gint cur_frame_num, gint unused_frame_num)
{
  GstH264RefPicMarking *refpicmarking;

  slice_hdr->dec_ref_pic_marking.adaptive_ref_pic_marking_mode_flag = 1;
  slice_hdr->dec_ref_pic_marking.n_ref_pic_marking = 2;

  refpicmarking = &slice_hdr->dec_ref_pic_marking.ref_pic_marking[0];

  refpicmarking->memory_management_control_operation = 1;
  refpicmarking->difference_of_pic_nums_minus1 =
      cur_frame_num - unused_frame_num - 1;

  refpicmarking = &slice_hdr->dec_ref_pic_marking.ref_pic_marking[1];
  refpicmarking->memory_management_control_operation = 0;
}

static gboolean
_add_slice_header (GstVulkanH264Enc * self, GstVulkanH264EncodeFrame * frame,
    GstH264PPS * pps, StdVideoEncodeH264SliceHeader * slice,
    GstVulkanH264EncodeFrame * list0[16], guint list0_num,
    GstVulkanH264EncodeFrame * list1[16], guint list1_num)
{
  GstH264SliceHdr slice_hdr;
  guint size, trail_bits;
  GstH264NalUnitType nal_type = GST_H264_NAL_SLICE;
#define SLICE_HDR_SIZE 4 + GST_ROUND_UP_8 (MAX_SLICE_HDR_SIZE) / 8
  guint8 packed_slice_hdr[SLICE_HDR_SIZE] = { 0, };
#undef SLICE_HDR_SIZE

  if (frame->frame_num == 0)
    nal_type = GST_H264_NAL_SLICE_IDR;

  /* *INDENT-OFF* */
  slice_hdr = (GstH264SliceHdr) {
    .first_mb_in_slice = slice->first_mb_in_slice,
    .type = slice->slice_type,
    .pps = pps,
    .frame_num = frame->frame_num,
    /* interlaced not supported now. */
    .field_pic_flag = 0,
    .bottom_field_flag = 0,
    .idr_pic_id = 0,// Removed in 0.9.10 (frame->frame_num == 0 ? slice->idr_pic_id : 0),
    /* only pic_order_cnt_type 1 is supported now. */
    //FIXME: unable to find it in StdVideoEncodeH264SliceHeader
    // .pic_order_cnt_lsb = slice->pic_order_cnt_lsb,
    // .delta_pic_order_cnt_bottom = slice->delta_pic_order_cnt_bottom,
     /* Only for B frame. */
    .direct_spatial_mv_pred_flag =
        (frame->type == GST_H264_B_SLICE ?
         slice->flags.direct_spatial_mv_pred_flag : 0),

    .num_ref_idx_active_override_flag = slice->flags.num_ref_idx_active_override_flag,
    .num_ref_idx_l0_active_minus1 = 0, // Removed in 0.9.10 slice->num_ref_idx_l0_active_minus1,
    .num_ref_idx_l1_active_minus1 = 0, // Removed in 0.9.10 slice->num_ref_idx_l1_active_minus1,
    /* Calculate it later. */
    .ref_pic_list_modification_flag_l0 = 0,
    .ref_pic_list_modification_flag_l1 = 0,
    /* We have weighted_pred_flag and weighted_bipred_idc 0 here, no
     * need weight_table. */

    .dec_ref_pic_marking = {
      .no_output_of_prior_pics_flag = 0,
      .long_term_reference_flag = 0,
      /* If not sliding_window, we set it later. */
      .adaptive_ref_pic_marking_mode_flag = 0,
    },

    .cabac_init_idc = slice->cabac_init_idc,
    //FIXME: unable to find it in StdVideoEncodeH264SliceHeader
    //.slice_qp_delta = slice->slice_qp_delta,

    .disable_deblocking_filter_idc = slice->disable_deblocking_filter_idc,
    .slice_alpha_c0_offset_div2 = slice->slice_alpha_c0_offset_div2,
    .slice_beta_offset_div2 = slice->slice_beta_offset_div2,
  };
  /* *INDENT-ON* */

  /* Reorder the ref lists if needed. */
  if (list0_num > 1) {
    /* list0 is in poc descend order now. */
    if (_ref_list_need_reorder (list0, list0_num, FALSE))
      _insert_ref_pic_list_modification (&slice_hdr, list0, list0_num, FALSE);
  }

  if (list0_num > 1) {
    /* list0 is in poc ascend order now. */
    if (_ref_list_need_reorder (list1, list1_num, TRUE)) {
      _insert_ref_pic_list_modification (&slice_hdr, list1, list1_num, TRUE);
    }
  }

  /* Mark the unused reference explicitly which this frame replaces. */
  if (frame->unused_for_reference_pic_num >= 0) {
    g_assert (frame->is_ref);
    _insert_ref_pic_marking_for_unused_frame (&slice_hdr, frame->frame_num,
        frame->unused_for_reference_pic_num);
  }

  size = sizeof (packed_slice_hdr);
  trail_bits = 0;
  if (gst_h264_bit_writer_slice_hdr (&slice_hdr, TRUE, nal_type, frame->is_ref,
          packed_slice_hdr, &size, &trail_bits) != GST_H264_BIT_WRITER_OK) {
    GST_ERROR_OBJECT (self, "Failed to generate the slice header");
    return FALSE;
  }

  if (!gst_vulkan_encoder_add_packed_header (self->encoder, frame->picture,
          packed_slice_hdr, size)) {
    GST_ERROR_OBJECT (self, "Failed to add the packed slice header");
    return FALSE;
  }

  return TRUE;
}

static gboolean
_add_aud (GstVulkanH264Enc * self, GstVulkanH264EncodeFrame * frame)
{
  guint8 aud_data[8] = { 0, };
  guint size;
  guint8 primary_pic_type = 0;

  switch (frame->type) {
    case GST_H264_I_SLICE:
      primary_pic_type = 0;
      break;
    case GST_H264_P_SLICE:
      primary_pic_type = 1;
      break;
    case GST_H264_B_SLICE:
      primary_pic_type = 2;
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  size = sizeof (aud_data);
  if (gst_h264_bit_writer_aud (primary_pic_type, TRUE, aud_data,
          &size) != GST_H264_BIT_WRITER_OK) {
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
    GstH264SEIMessage * sei_msg)
{
  guint8 *data;
  GstH264RegisteredUserData *user_data;

  sei_msg->payloadType = GST_H264_SEI_REGISTERED_USER_DATA;

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
_create_sei_cc_data (GPtrArray * cc_list, guint8 * sei_data, guint * data_size)
{
  GArray *msg_list = NULL;
  GstH264BitWriterResult ret;
  gint i;

  msg_list = g_array_new (TRUE, TRUE, sizeof (GstH264SEIMessage));
  g_array_set_clear_func (msg_list, (GDestroyNotify) gst_h264_sei_clear);
  g_array_set_size (msg_list, cc_list->len);

  for (i = 0; i < cc_list->len; i++) {
    GstH264SEIMessage *msg = &g_array_index (msg_list, GstH264SEIMessage, i);
    _create_sei_cc_message (g_ptr_array_index (cc_list, i), msg);
  }

  ret = gst_h264_bit_writer_sei (msg_list, TRUE, sei_data, data_size);

  g_array_unref (msg_list);

  return (ret == GST_H264_BIT_WRITER_OK);
}

static void
_add_sei_cc (GstVulkanH264Enc * self, GstVideoCodecFrame * gst_frame)
{
  GstVulkanH264EncodeFrame *frame;
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

  if (!_create_sei_cc_data (cc_list, packed_sei, &sei_size)) {
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
_encode_one_frame (GstVulkanH264Enc * self, GstVideoCodecFrame * gst_frame)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  GstH264PPS pps;
  GstVulkanH264EncodeFrame *list0[16] = { NULL, };
  guint list0_num = 0;
  GstVulkanH264EncodeFrame *list1[16] = { NULL, };
  guint list1_num = 0;
  guint slice_of_mbs, slice_mod_mbs, slice_start_mb, slice_mbs;
  gint i;
  GstVulkanH264EncodeFrame *frame;
  GstVulkanVideoCapabilites enc_caps;

  if (!gst_vulkan_encoder_vk_caps (self->encoder, &enc_caps))
    return FALSE;

  g_return_val_if_fail (gst_frame, FALSE);

  frame = _enc_frame (gst_frame);
  frame->poc = ((base->gop.cur_frame_index * 2) % base->gop.max_pic_order_cnt);


  if (self->aud && !_add_aud (self, frame))
    return FALSE;

  /* Repeat the SPS for IDR. */
  GST_WARNING_OBJECT (self, "coucou frame->poc %d", frame->poc);
  if (frame->poc == 0) {
    //FIXME: this generation is buggy, to be validated
    // _fill_sps (self, &self->session_params.sps);
    // _add_sequence_header(self, frame);
    // _fill_pps (self, &self->session_params.pps, &self->sequence_hdr);
    // _add_picture_header(self, frame);
    _add_vulkan_params_header (self, frame);
  }

  /* Non I frame, construct reference list. */
  if (frame->type != GST_H264_I_SLICE) {
    GstVulkanH264EncodeFrame *vaf;
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

  if (frame->type == GST_H264_B_SLICE) {
    GstVulkanH264EncodeFrame *vaf;
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

  slice_of_mbs = base->mb_width * base->mb_height / self->num_slices;
  slice_mod_mbs = base->mb_width * base->mb_height % self->num_slices;
  slice_start_mb = 0;
  slice_mbs = 0;
  for (i = 0; i < self->num_slices; i++) {
    StdVideoEncodeH264SliceHeader slice;

    slice_mbs = slice_of_mbs;
    /* divide the remainder to each equally */
    if (slice_mod_mbs) {
      slice_mbs++;
      slice_mod_mbs--;
    }
    // if (!_add_one_slice (self, frame, slice_start_mb, slice_mbs, &slice,
    //         list0, list0_num, list1, list1_num))
    //   return FALSE;

    if ((self->packed_headers & VULKAN_PACKED_HEADER_TYPE_SLICE) &&
        (!_add_slice_header (self, frame, &pps, &slice, list0, list0_num,
                list1, list1_num)))
      return FALSE;

    slice_start_mb += slice_mbs;
  }

  /* *INDENT-OFF* */
  frame->slice_wt = (StdVideoEncodeH264WeightTable) {
    .flags = (StdVideoEncodeH264WeightTableFlags) {
        .luma_weight_l0_flag = 0,
        .chroma_weight_l0_flag = 0,
        .luma_weight_l1_flag = 0,
        .chroma_weight_l1_flag = 0,
    },
    .luma_log2_weight_denom = 0,
    .chroma_log2_weight_denom = 0,
    .luma_weight_l0 = { 0 },
    .luma_offset_l0 = { 0 },
    .chroma_weight_l0 = { { 0 } },
    .chroma_offset_l0 = { { 0 } },
    .luma_weight_l1 = { 0 },
    .luma_offset_l1 = { 0 },
    .chroma_weight_l1 = { { 0 } },
    .chroma_offset_l1 = { { 0 } },
  };

  frame->slice_hdr = (StdVideoEncodeH264SliceHeader) {
      .flags = (StdVideoEncodeH264SliceHeaderFlags) {
        .direct_spatial_mv_pred_flag = 0,
        .num_ref_idx_active_override_flag = 0,
    },

    .first_mb_in_slice = 0,
    .slice_type = gst_vulkan_video_h264_slice_type(frame->type),
    .cabac_init_idc = 0,
    .disable_deblocking_filter_idc = 1,
    .slice_alpha_c0_offset_div2 = 0,
    .slice_beta_offset_div2 = 0,
    .pWeightTable = &frame->slice_wt,
  };

  frame->slice_info = (VkVideoEncodeH264NaluSliceInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_EXT,
    .pNext = NULL,
    .pStdSliceHeader = &frame->slice_hdr,
  };

  frame->pic_info = (StdVideoEncodeH264PictureInfo) {
    .flags = (StdVideoEncodeH264PictureInfoFlags) {
        .IdrPicFlag = (gst_vulkan_video_h264_picture_type(frame->type, frame->is_ref) == STD_VIDEO_H264_PICTURE_TYPE_IDR),
        .is_reference = frame->is_ref,
        .no_output_of_prior_pics_flag = 0,
        .long_term_reference_flag = 0,
        .adaptive_ref_pic_marking_mode_flag = 0,
    },
    .seq_parameter_set_id = self->session_params.sps.seq_parameter_set_id,
    .pic_parameter_set_id = self->session_params.pps.pic_parameter_set_id,
    .primary_pic_type = gst_vulkan_video_h264_picture_type(frame->type, frame->is_ref),
    .frame_num = frame->frame_num,
    .PicOrderCnt = frame->picture->pic_order_cnt,
  };


  if (gst_vulkan_encoder_get_n_ref_slots(self->encoder)) {
    frame->ref_list_info = (StdVideoEncodeH264ReferenceListsInfo) {
      .flags = (StdVideoEncodeH264ReferenceListsInfoFlags) {
        .ref_pic_list_modification_flag_l0 = 0,
        .ref_pic_list_modification_flag_l1 = 0,
      },
      .num_ref_idx_l0_active_minus1 = 0,
      .num_ref_idx_l1_active_minus1 = 0,
      .RefPicList0 = {},
      .RefPicList1 = {},
      .refList0ModOpCount = 0,
      .refList1ModOpCount = 0,
      .refPicMarkingOpCount = 0,
      .reserved1 = {},
      .pRefList0ModOperations = NULL,
      .pRefList1ModOperations = NULL,
      .pRefPicMarkingOperations = NULL,
    };
    frame->pic_info.pRefLists = &frame->ref_list_info;
  }

  frame->rc_info = (VkVideoEncodeH264RateControlInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_EXT,
    .pNext = NULL,
    .gopFrameCount = 0,
    .idrPeriod = 0,
    .consecutiveBFrameCount = 0,
    .temporalLayerCount = 1,
  };

  frame->rc_layer_info = (VkVideoEncodeH264RateControlLayerInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT,
    .pNext = NULL,
    .useMinQp = TRUE,
    .minQp = (VkVideoEncodeH264QpEXT){ self->rc.min_qp, self->rc.min_qp, self->rc.min_qp },
    .useMaxQp = TRUE,
    .maxQp = (VkVideoEncodeH264QpEXT){ self->rc.max_qp, self->rc.max_qp, self->rc.max_qp },
    .useMaxFrameSize = 0,
    .maxFrameSize = (VkVideoEncodeH264FrameSizeEXT) {0, 0, 0},
  };

  frame->enc_pic_info = (VkVideoEncodeH264PictureInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_EXT,
    .pNext = NULL,
    .naluSliceEntryCount = 1,
    .pNaluSliceEntries = &frame->slice_info,
    .pStdPictureInfo = &frame->pic_info,
    .generatePrefixNalu = (enc_caps.codec.h264enc.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_GENERATE_PREFIX_NALU_BIT_EXT),
  };
  frame->ref_info = (StdVideoDecodeH264ReferenceInfo) {
    .flags = (StdVideoDecodeH264ReferenceInfoFlags) {
      .top_field_flag = 0,
      .bottom_field_flag = 0,
      .used_for_long_term_reference = 0,
      .is_non_existing = 0,
    },
    .FrameNum = 0,
    .PicOrderCnt = {0, 0},
  };

  frame->dpb_slot_info = (VkVideoDecodeH264DpbSlotInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_EXT,
    .pNext = NULL,
    .pStdReferenceInfo = &frame->ref_info,

  };
  /* *INDENT-ON* */

  frame->picture->codec_pic_info = &frame->enc_pic_info;
  frame->picture->codec_rc_info = &frame->rc_info;
  frame->picture->codec_rc_layer_info = &frame->rc_layer_info;
  frame->picture->codec_dpb_slot_info = &frame->dpb_slot_info;
  frame->picture->fps_n = GST_VIDEO_INFO_FPS_N (&base->input_state->info);
  frame->picture->fps_d = GST_VIDEO_INFO_FPS_D (&base->input_state->info);

  if (!gst_vulkan_encoder_encode (self->encoder, frame->picture)) {
    GST_ERROR_OBJECT (self, "Encode frame error");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_vulkan_h264enc_flush (GstVideoEncoder * venc)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (venc);
  GstH264Encoder *base = GST_H264_ENCODER (self);

  /* begin from an IDR after flush. */
  base->gop.cur_frame_index = 0;
  base->gop.cur_frame_num = 0;

  return GST_VIDEO_ENCODER_CLASS (parent_class)->flush (venc);
}

static void
gst_vulkan_h264enc_prepare_output (GstH264Encoder * base,
    GstVideoCodecFrame * frame)
{
  GstVulkanH264EncodeFrame *frame_enc;
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
  GstVulkanH264EncodeFrame *frame1 = _enc_frame ((GstVideoCodecFrame *) a);
  GstVulkanH264EncodeFrame *frame2 = _enc_frame ((GstVideoCodecFrame *) b);

  g_assert (frame1->frame_num != frame2->frame_num);

  return frame1->frame_num - frame2->frame_num;
}

static GstVideoCodecFrame *
_find_unused_reference_frame (GstVulkanH264Enc * self,
    GstVulkanH264EncodeFrame * frame)
{
  GstH264Encoder *base = GST_H264_ENCODER (self);
  GstVulkanH264EncodeFrame *b_vaframe;
  GstVideoCodecFrame *b_frame;
  guint i;

  /* We still have more space. */
  if (g_queue_get_length (&base->ref_list) < base->gop.num_ref_frames)
    return NULL;

  /* Not b_pyramid, sliding window is enough. */
  if (!base->gop.b_pyramid)
    return g_queue_peek_head (&base->ref_list);

  /* I/P frame, just using sliding window. */
  if (frame->type != GST_H264_B_SLICE)
    return g_queue_peek_head (&base->ref_list);

  /* Choose the B frame with lowest POC. */
  b_frame = NULL;
  b_vaframe = NULL;
  for (i = 0; i < g_queue_get_length (&base->ref_list); i++) {
    GstVulkanH264EncodeFrame *vaf;
    GstVideoCodecFrame *f;

    f = g_queue_peek_nth (&base->ref_list, i);
    vaf = _enc_frame (f);
    if (vaf->type != GST_H264_B_SLICE)
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
gst_vulkan_h264enc_encode_frame (GstH264Encoder * base,
    GstVideoCodecFrame * gst_frame, gboolean is_last)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (base);
  GstVulkanH264EncodeFrame *frame;
  GstVideoCodecFrame *unused_ref = NULL;

  frame = _enc_frame (gst_frame);
  frame->last_frame = is_last;

  g_assert (frame->picture == NULL);
  frame->picture = gst_vulkan_encode_picture_new (self->encoder,
      gst_frame->input_buffer, base->width, base->height,
      frame->type, frame->is_ref);

  if (!frame->picture) {
    GST_ERROR_OBJECT (self, "Failed to create the encode picture");
    return GST_FLOW_ERROR;
  }

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
gst_vulkan_h264enc_new_frame (GstH264Encoder * base, GstVideoCodecFrame * frame)
{
  GstVulkanH264EncodeFrame *frame_in;

  frame_in = gst_vulkan_h264_encode_frame_new ();
  frame_in->frame_num = base->input_frame_count;
  frame_in->total_frame_count = base->input_frame_count++;
  gst_video_codec_frame_set_user_data (frame, frame_in,
      gst_vulkan_h264_encode_frame_free);

  return TRUE;
}

static void
gst_vulkan_h264enc_init (GstVulkanH264Enc * self)
{
  gst_vulkan_buffer_memory_init_once ();
  self->prop.num_slices = 1;
  self->prop.min_qp = 1;
  self->prop.max_qp = 51;
  self->prop.qp_i = 26;
  self->prop.qp_p = 26;
  self->prop.qp_b = 26;
  self->prop.num_ref_frames = 3;
}

static void
gst_vulkan_h264enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (object);

  switch (prop_id) {
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
    case PROP_NUM_REF_FRAMES:
      self->prop.num_ref_frames = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_h264enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (object);

  switch (prop_id) {
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
    case PROP_NUM_REF_FRAMES:
      g_value_set_uint (value, self->prop.num_ref_frames);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_vulkan_h264enc_propose_allocation (GstVideoEncoder * venc, GstQuery * query)
{
  gboolean need_pool;
  GstCaps *caps, *profile_caps;
  GstVideoInfo info;
  guint size;
  GstBufferPool *pool = NULL;
  GstVulkanH264Enc *self = GST_VULKAN_H264ENC (venc);

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

static void
gst_vulkan_h264enc_class_init (GstVulkanH264EncClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GstH264EncoderClass *h264encoder_class = GST_H264_ENCODER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gint n_props = PROP_MAX;
  GParamFlags param_flags =
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT;
  gst_element_class_set_metadata (element_class, "Vulkan H.264 encoder",
      "Codec/Encoder/Video/Hardware", "A H.264 video encoder based on Vulkan",
      "Stéphane Cerveau <scerveau@igalia.com>");

  gst_element_class_add_static_pad_template (element_class,
      &gst_vulkan_h264enc_sink_template);

  gst_element_class_add_static_pad_template (element_class,
      &gst_vulkan_h264enc_src_template);


  gobject_class->set_property = gst_vulkan_h264enc_set_property;
  gobject_class->get_property = gst_vulkan_h264enc_get_property;


  properties[PROP_VIDEO_USAGE] =
      g_param_spec_enum ("vulkan-usage", "Vulkan encode usage",
      "Choose the vulkan encoding usage", GST_TYPE_VULKAN_H264_ENCODE_USAGE,
      VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  properties[PROP_VIDEO_CONTENT] =
      g_param_spec_enum ("vulkan-content", "Vulkan encode content",
      "Choose the vulkan encoding content", GST_TYPE_VULKAN_H264_ENCODE_CONTENT,
      VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
  properties[PROP_TUNING_MODE] =
      g_param_spec_enum ("tuning-mode", "Vulkan encode tuning",
      "Choose the vulkan encoding tuning",
      GST_TYPE_VULKAN_H264_ENCODE_TUNING_MODE,
      VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY);
    /**
   * GstVkH264Enc:num-slices:
   *
   * The number of slices per frame.
   */
  properties[PROP_NUM_SLICES] = g_param_spec_uint ("num-slices",
      "Number of Slices", "Number of slices per frame", 1, 200, 1, param_flags);

  /**
   * GstVkH264Enc:max-qp:
   *
   * The maximum quantizer value.
   */
  properties[PROP_MAX_QP] = g_param_spec_uint ("max-qp", "Maximum QP",
      "Maximum quantizer value for each frame", 0, 51, 51, param_flags);

  /**
   * GstVkH264Enc:min-qp:
   *
   * The minimum quantizer value.
   */
  properties[PROP_MIN_QP] = g_param_spec_uint ("min-qp", "Minimum QP",
      "Minimum quantizer value for each frame", 0, 51, 1, param_flags);

  /**
   * GstVkH264Enc:qpi:
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
   * GstVkH264Enc:qpp:
   *
   * The quantizer value for P frame. Available only in CQP mode.
   */
  properties[PROP_QP_P] = g_param_spec_uint ("qpp",
      "The quantizer value for P frame",
      "The quantizer value for P frame. Available only in CQP mode",
      0, 51, 26, param_flags | GST_PARAM_MUTABLE_PLAYING);

  /**
   * GstVkH264Enc:qpb:
   *
   * The quantizer value for B frame. Available only in CQP mode.
   */
  properties[PROP_QP_B] = g_param_spec_uint ("qpb",
      "The quantizer value for B frame",
      "The quantizer value for B frame. Available only in CQP mode",
      0, 51, 26, param_flags | GST_PARAM_MUTABLE_PLAYING);

  /**
   * GstVkH264Enc:ref-frames:
   *
   * The number of reference frames.
   * FIXME: Se the right max num reference frames in Vulkan
   */
  properties[PROP_NUM_REF_FRAMES] = g_param_spec_uint ("ref-frames",
      "Number of Reference Frames",
      "Number of reference frames, including both the forward and the backward",
      0, 15, 3, param_flags);

  g_object_class_install_properties (gobject_class, n_props, properties);

  encoder_class->open = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_open);
  encoder_class->close = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_close);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_stop);
  encoder_class->src_query = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_src_query);
  encoder_class->sink_query = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_sink_query);
  encoder_class->flush = GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_flush);
  encoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_propose_allocation);

  h264encoder_class->new_frame =
      GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_new_frame);
  h264encoder_class->encode_frame =
      GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_encode_frame);
  h264encoder_class->prepare_output =
      GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_prepare_output);
  h264encoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_vulkan_h264enc_set_format);
}
