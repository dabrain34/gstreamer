/* GStreamer
 *
 * Copyright (C) 2023 Igalia, S.L.
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/check/gstcheck.h>
#include <gst/vulkan/vulkan.h>

static GstVulkanInstance *instance;
static GstVulkanDevice *device;
static GstVulkanQueue *video_queue = NULL;

static void
setup (void)
{
  instance = gst_vulkan_instance_new ();
  fail_unless (gst_vulkan_instance_open (instance, NULL));
}

static void
teardown (void)
{
  gst_clear_object (&video_queue);
  gst_clear_object (&device);
  gst_object_unref (instance);
}

#define H264_MB_SIZE_ALIGNMENT 16

static GstVulkanEncodePicture *
allocate_picture (GstVulkanEncoder * enc, GstBufferPool * pool, int width,
    int height, GstH264SliceType type, gboolean is_ref)
{
  GstVulkanEncodePicture *picture;
  GstBuffer *input_buffer;

  fail_unless (gst_buffer_pool_acquire_buffer (pool, &input_buffer,
          NULL) == GST_FLOW_OK);

  fail_unless (gst_buffer_n_memory (input_buffer) > 0);

  picture =
      gst_vulkan_encode_picture_new (enc, input_buffer, width, height, type,
      is_ref);
  fail_unless (picture);
  gst_buffer_unref (input_buffer);

  return picture;
}

static void
encode_picture (GstVulkanEncoder * enc, GstVulkanEncodePicture * picture,
    guint frame_num)
{
  StdVideoEncodeH264WeightTable slice_wt;
  StdVideoEncodeH264SliceHeader slice_hdr;
  StdVideoEncodeH264PictureInfo pic_info;
  StdVideoDecodeH264ReferenceInfo ref_info;
  StdVideoEncodeH264ReferenceListsInfo ref_list_info;
  VkVideoEncodeH264NaluSliceInfoEXT slice_info;
  VkVideoEncodeH264RateControlInfoEXT rc_info;
  VkVideoEncodeH264RateControlLayerInfoEXT rc_layer_info;
  VkVideoEncodeH264PictureInfoEXT enc_pic_info;
  VkVideoDecodeH264DpbSlotInfoKHR dpb_slot_info;
  GstVulkanVideoCapabilites enc_caps;

  fail_unless (gst_vulkan_encoder_vk_caps (enc, &enc_caps));

  gint sps_id = 0;
  gint pps_id = 0;
  guint qp_i = 26;
  guint qp_p = 26;
  guint qp_b = 26;

  /* *INDENT-OFF* */
  slice_wt = (StdVideoEncodeH264WeightTable) {
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

  slice_hdr = (StdVideoEncodeH264SliceHeader) {
      .flags = (StdVideoEncodeH264SliceHeaderFlags) {
        .direct_spatial_mv_pred_flag = 0,
        .num_ref_idx_active_override_flag = 0,
    },
    .first_mb_in_slice = 0,
    .slice_type = gst_vulkan_video_h264_slice_type (picture->type),
    .cabac_init_idc = 0,
    .disable_deblocking_filter_idc = 1,
    .slice_alpha_c0_offset_div2 = 0,
    .slice_beta_offset_div2 = 0,
    .pWeightTable = &slice_wt,
  };

  pic_info = (StdVideoEncodeH264PictureInfo) {
    .flags = (StdVideoEncodeH264PictureInfoFlags) {
        .IdrPicFlag = (gst_vulkan_video_h264_picture_type (picture->type, picture->is_ref) == STD_VIDEO_H264_PICTURE_TYPE_IDR),
        .is_reference = picture->is_ref, // TODO: Check why it creates a deadlock in query result when TRUE
        .no_output_of_prior_pics_flag = 0,
        .long_term_reference_flag = 0,
        .adaptive_ref_pic_marking_mode_flag = 0,
    },
    .seq_parameter_set_id = sps_id,
    .pic_parameter_set_id = pps_id,
    .primary_pic_type = gst_vulkan_video_h264_picture_type (picture->type, picture->is_ref),
    .frame_num = frame_num,
    .PicOrderCnt = picture->pic_order_cnt,
  };

  if (gst_vulkan_encoder_get_n_ref_slots(enc)) {
    ref_list_info = (StdVideoEncodeH264ReferenceListsInfo) {
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
    pic_info.pRefLists = &ref_list_info;
  }

  slice_info = (VkVideoEncodeH264NaluSliceInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_EXT,
    .pNext = NULL,
    .pStdSliceHeader = &slice_hdr,
  };

  rc_layer_info = (VkVideoEncodeH264RateControlLayerInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_EXT,
    .pNext = NULL,
    .useMinQp = TRUE,
    .minQp = (VkVideoEncodeH264QpEXT){ qp_i, qp_p, qp_b },
    .useMaxQp = TRUE,
    .maxQp = (VkVideoEncodeH264QpEXT){ qp_i, qp_p, qp_b },
    .useMaxFrameSize = 0,
    .maxFrameSize = (VkVideoEncodeH264FrameSizeEXT) {0, 0, 0},
  };

  rc_info = (VkVideoEncodeH264RateControlInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_EXT,
    .pNext = &rc_layer_info,
    .gopFrameCount = 0,
    .idrPeriod = 0,
    .consecutiveBFrameCount = 0,
    .temporalLayerCount = 1,
  };

  enc_pic_info = (VkVideoEncodeH264PictureInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_EXT,
    .pNext = NULL,
    .naluSliceEntryCount = 1,
    .pNaluSliceEntries = &slice_info,
    .pStdPictureInfo = &pic_info,
    .generatePrefixNalu = (enc_caps.codec.h264enc.flags & VK_VIDEO_ENCODE_H264_CAPABILITY_GENERATE_PREFIX_NALU_BIT_EXT),
  };

  ref_info = (StdVideoDecodeH264ReferenceInfo) {
    .flags = (StdVideoDecodeH264ReferenceInfoFlags) {
      .top_field_flag = 0,
      .bottom_field_flag = 0,
      .used_for_long_term_reference = 0,
      .is_non_existing = 0,
    },
    .FrameNum = 0,
    .PicOrderCnt = {0, 0},
  };

  dpb_slot_info = (VkVideoDecodeH264DpbSlotInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_EXT,
    .pNext = NULL,
    .pStdReferenceInfo = &ref_info,

  };
  /* *INDENT-ON* */

  picture->codec_pic_info = &enc_pic_info;
  picture->codec_rc_layer_info = &rc_layer_info;
  picture->codec_rc_info = &rc_info;
  picture->codec_dpb_slot_info = &dpb_slot_info;

  fail_unless (gst_vulkan_encoder_encode (enc, picture));
}

GST_START_TEST (test_encoder)
{
  GstVulkanEncoder *enc;
  GstBufferPool *pool;
  GError *err = NULL;
  uint32_t width = 176;
  uint32_t height = 144;
  gint sps_id = 0;
  gint pps_id = 0;
  GstVulkanEncodePicture *picture;
  int frame_num = 0;

  /* initialize the encoder */
  {
    uint32_t mbAlignedWidth, mbAlignedHeight;
    StdVideoH264ProfileIdc profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
    StdVideoH264LevelIdc level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
    StdVideoH264ChromaFormatIdc chromat_format_idc =
        STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
    StdVideoH264SequenceParameterSet sps;
    StdVideoH264PictureParameterSet pps;
    GstVulkanVideoProfile profile;
    GstVulkanEncoderParameters enc_params;
    VkVideoEncodeH264SessionParametersAddInfoEXT params_add;

    memset (&sps, 0, sizeof (StdVideoH264SequenceParameterSet));
    sps.seq_parameter_set_id = sps_id;
    mbAlignedWidth =
        (width + H264_MB_SIZE_ALIGNMENT - 1) & ~(H264_MB_SIZE_ALIGNMENT - 1);
    mbAlignedHeight =
        (height + H264_MB_SIZE_ALIGNMENT - 1) & ~(H264_MB_SIZE_ALIGNMENT - 1);

    sps.flags.direct_8x8_inference_flag = 1u;
    sps.flags.frame_mbs_only_flag = 1u;
    sps.profile_idc = profile_idc;
    sps.level_idc = level_idc;
    sps.chroma_format_idc = chromat_format_idc;
    sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
    sps.max_num_ref_frames = 1u;
    sps.pic_width_in_mbs_minus1 = mbAlignedWidth / H264_MB_SIZE_ALIGNMENT - 1;
    sps.pic_height_in_map_units_minus1 =
        mbAlignedHeight / H264_MB_SIZE_ALIGNMENT - 1;
    sps.frame_crop_right_offset = mbAlignedWidth - width;
    sps.frame_crop_bottom_offset = mbAlignedHeight - height;
    if (sps.frame_crop_right_offset || sps.frame_crop_bottom_offset) {
      sps.flags.frame_cropping_flag = TRUE;
      if (sps.chroma_format_idc == STD_VIDEO_H264_CHROMA_FORMAT_IDC_420) {
        sps.frame_crop_right_offset >>= 1;
        sps.frame_crop_bottom_offset >>= 1;
      }
    }
    // Set arbitrary value for PPS
    memset (&pps, 0, sizeof (StdVideoH264PictureParameterSet));
    pps.seq_parameter_set_id = sps_id;
    pps.pic_parameter_set_id = pps_id;

    /* *INDENT-OFF* */
    params_add = (VkVideoEncodeH264SessionParametersAddInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_EXT,
      .pStdSPSs = &sps,
      .stdSPSCount = 1,
      .pStdPPSs = &pps,
      .stdPPSCount = 1,
    };

    enc_params.h264.params_create = (VkVideoEncodeH264SessionParametersCreateInfoEXT) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_EXT,
      .maxStdSPSCount = 1,
      .maxStdPPSCount = 1,
      .pParametersAddInfo = &params_add
    };
      /* *INDENT-OFF* */
    profile = (GstVulkanVideoProfile) {
      .profile = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
        .pNext = &profile.codec,
        .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT,
        .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
        .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
        .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
      },
      .codec.h264enc = {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_EXT,
        .stdProfileIdc = profile_idc,
      }
    };
    /* *INDENT-ON* */

    video_queue =
        gst_vulkan_select_queue (instance, VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
    if (!video_queue) {
      GST_WARNING ("Unable to find encoding queue");
      return;
    }
    enc = gst_vulkan_queue_create_encoder (video_queue,
        VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT);

    if (!enc) {
      GST_WARNING ("Unable to create a vulkan encoder, queue=%p", video_queue);
      return;
    }

    fail_unless (gst_vulkan_encoder_start (enc, &profile, &enc_params, &err));
  }

  /* retrieve the SPS/PPS from the device */
  {
    VkVideoEncodeH264SessionParametersGetInfoEXT video_codec_session_info;
    void *header;
    gsize header_size;


     /* *INDENT-OFF* */
    video_codec_session_info = (VkVideoEncodeH264SessionParametersGetInfoEXT)
	  {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_EXT,
      .pNext = NULL,
      .writeStdSPS = TRUE,
      .writeStdPPS = TRUE,
      .stdSPSId = 0,
      .stdPPSId = 0,
	  };
    /* *INDENT-ON* */

    fail_unless (gst_vulkan_encoder_get_session_params (enc,
            &video_codec_session_info, &header_size, NULL));
    fail_unless (header_size != 0);
    header = g_malloc0 (header_size);
    fail_unless (gst_vulkan_encoder_get_session_params (enc,
            &video_codec_session_info, &header_size, header));
    GST_MEMDUMP ("params buffer", header, header_size);
    g_free (header);
  }
  /* initialize the input buffer pool */
  {
    GstVideoFormat format = GST_VIDEO_FORMAT_NV12;
    GstCaps *caps = gst_caps_new_simple ("video/x-raw", "format",
        G_TYPE_STRING, gst_video_format_to_string (format), "width", G_TYPE_INT,
        width, "height", G_TYPE_INT, height, NULL);
    GstStructure *config;

    pool = gst_vulkan_image_buffer_pool_new (video_queue->device);
    config = gst_buffer_pool_get_config (pool);

    gst_caps_set_features_simple (caps,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_VULKAN_IMAGE, NULL));

    gst_buffer_pool_config_set_params (config, caps, 1024, 1, 0);
    gst_caps_unref (caps);
    gst_vulkan_image_buffer_pool_config_set_allocation_params (config,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
        VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    gst_vulkan_image_buffer_pool_config_set_encode_caps (config,
        gst_vulkan_encoder_profile_caps (enc));

    fail_unless (gst_buffer_pool_set_config (pool, config));
    fail_unless (gst_buffer_pool_set_active (pool, TRUE));
  }

  /* encode first picture as an IDR-Frame */
  {
    /* allocate picture */
    picture =
        allocate_picture (enc, pool, width, height, GST_H264_I_SLICE, TRUE);
    fail_unless (picture != NULL);

    /* encode frame */
    encode_picture (enc, picture, frame_num);
    fail_unless (picture->out_buffer != NULL);

    gst_vulkan_encode_picture_free (picture);
  }

  /* encode second picture as a I-Frame */
  {
    /* allocate picture */
    frame_num++;
    picture =
        allocate_picture (enc, pool, width, height, GST_H264_P_SLICE, FALSE);
    fail_unless (picture != NULL);

    /* encode frame */
    encode_picture (enc, picture, frame_num);
    fail_unless (picture->out_buffer != NULL);

    gst_vulkan_encode_picture_free (picture);
  }

  fail_unless (gst_buffer_pool_set_active (pool, FALSE));
  gst_object_unref (pool);

  fail_unless (gst_vulkan_encoder_stop (enc));

  gst_object_unref (enc);
}

GST_END_TEST;

static Suite *
vkvideo_suite (void)
{
  Suite *s = suite_create ("vkvideo");
  TCase *tc_basic = tcase_create ("general");
  gboolean have_instance;

  suite_add_tcase (s, tc_basic);
  tcase_add_checked_fixture (tc_basic, setup, teardown);

  /* FIXME: CI doesn't have a software vulkan renderer (and none exists currently) */
  instance = gst_vulkan_instance_new ();
  have_instance = gst_vulkan_instance_open (instance, NULL);
  gst_object_unref (instance);
  if (have_instance) {
#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS && VK_ENABLE_BETA_EXTENSIONS
    tcase_add_test (tc_basic, test_encoder);
#endif
  }

  return s;
}

GST_CHECK_MAIN (vkvideo);
