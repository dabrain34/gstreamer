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
#include <gst/codecparsers/gsth265parser.h>
#include <gst/vulkan/vulkan.h>

static GstVulkanInstance *instance;

static GstVulkanQueue *video_queue = NULL;


#ifndef STD_VIDEO_H265_NO_REFERENCE_PICTURE
#define STD_VIDEO_H265_NO_REFERENCE_PICTURE 0xFF
#endif

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
  gst_object_unref (instance);
}

#define H265_MB_SIZE_ALIGNMENT 16

static GstBufferPool *
allocate_input_buffer_pool (GstVulkanEncoder * enc, uint32_t width,
    uint32_t height)
  /* initialize the input buffer pool */
{
  GstVideoFormat format = GST_VIDEO_FORMAT_NV12;
  GstCaps *caps = gst_caps_new_simple ("video/x-raw", "format",
      G_TYPE_STRING, gst_video_format_to_string (format), "width", G_TYPE_INT,
      width, "height", G_TYPE_INT, height, NULL);
  GstStructure *config;

  GstBufferPool *pool = gst_vulkan_image_buffer_pool_new (video_queue->device);
  config = gst_buffer_pool_get_config (pool);

  gst_caps_set_features_simple (caps,
      gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_VULKAN_IMAGE, NULL));

  gst_buffer_pool_config_set_params (config, caps, 1024, 1, 0);
  gst_caps_unref (caps);
  gst_vulkan_image_buffer_pool_config_set_allocation_params (config,
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  gst_vulkan_image_buffer_pool_config_set_encode_caps (config,
      gst_vulkan_encoder_profile_caps (enc));

  fail_unless (gst_buffer_pool_set_config (pool, config));
  fail_unless (gst_buffer_pool_set_active (pool, TRUE));
  return pool;
}

static GstVulkanEncodePicture *
allocate_picture (GstVulkanEncoder * enc, GstBufferPool * pool, int width,
    int height, gboolean is_ref, gint nb_refs)
{
  GstVulkanEncodePicture *picture;
  GstBuffer *input_buffer;

  fail_unless (gst_buffer_pool_acquire_buffer (pool, &input_buffer,
          NULL) == GST_FLOW_OK);

  fail_unless (gst_buffer_n_memory (input_buffer) > 0);

  picture =
      gst_vulkan_encode_picture_new (enc, input_buffer, width, height, is_ref,
      nb_refs);
  fail_unless (picture);
  gst_buffer_unref (input_buffer);

  return picture;
}

static void
encode_h265_picture (GstVulkanEncoder * enc, GstVulkanEncodePicture * picture,
    guint frame_num, GstVulkanEncodePicture ** ref_pics, gint ref_pics_num,
    GstH265SliceType slice_type, gint vps_id, gint sps_id, gint pps_id)
{
  StdVideoEncodeH265WeightTable slice_wt;
  StdVideoEncodeH265SliceSegmentHeader slice_hdr;
  StdVideoEncodeH265PictureInfo pic_info;
  StdVideoEncodeH265ReferenceInfo ref_info;
  StdVideoEncodeH265ReferenceListsInfo ref_list_info;
  VkVideoEncodeH265NaluSliceSegmentInfoEXT slice_info;
  VkVideoEncodeH265RateControlInfoEXT rc_info;
  VkVideoEncodeH265RateControlLayerInfoEXT rc_layer_info;
  VkVideoEncodeH265QualityLevelPropertiesEXT quality_level;
  VkVideoEncodeH265PictureInfoEXT enc_pic_info;
  VkVideoEncodeH265DpbSlotInfoEXT dpb_slot_info;
  GstVulkanVideoCapabilites enc_caps;

  GST_DEBUG ("Encoding frame num:%d", frame_num);

  fail_unless (gst_vulkan_encoder_vk_caps (enc, &enc_caps));


  guint qp_i = 26;
  guint qp_p = 26;
  guint qp_b = 26;

  /* *INDENT-OFF* */
  slice_wt = (StdVideoEncodeH265WeightTable) {
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

  slice_hdr = (StdVideoEncodeH265SliceSegmentHeader) {
      .flags = (StdVideoEncodeH265SliceSegmentHeaderFlags) {
        .first_slice_segment_in_pic_flag = 0,
        .dependent_slice_segment_flag = 0,
        .slice_sao_luma_flag = 0,
        .slice_sao_chroma_flag = 0,
        .num_ref_idx_active_override_flag = 0,
        .mvd_l1_zero_flag = 0,
        .cabac_init_flag = 0,
        .cu_chroma_qp_offset_enabled_flag = 0,
        .deblocking_filter_override_flag = 0,
        .slice_deblocking_filter_disabled_flag = 0,
        .collocated_from_l0_flag = 0,
        .slice_loop_filter_across_slices_enabled_flag = 0,
    },
      .slice_type = gst_vulkan_video_h265_slice_type (slice_type),
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
      .pWeightTable = &slice_wt,
  };

  pic_info = (StdVideoEncodeH265PictureInfo) {
    .flags = (StdVideoEncodeH265PictureInfoFlags) {
          .is_reference = picture->is_ref,
          .IrapPicFlag = (gst_vulkan_video_h265_slice_type (slice_type) == STD_VIDEO_H265_SLICE_TYPE_I && picture->is_ref),
          .used_for_long_term_reference = 0,
          .discardable_flag = 0,
          .cross_layer_bla_flag = 0,
          .pic_output_flag = 0,
          .no_output_of_prior_pics_flag = 0,
          .short_term_ref_pic_set_sps_flag = 0,
          .slice_temporal_mvp_enabled_flag = 0,
    },
    .pic_type = gst_vulkan_video_h265_picture_type(slice_type, picture->is_ref),
    .sps_video_parameter_set_id = vps_id,
    .pps_seq_parameter_set_id = sps_id,
    .pps_pic_parameter_set_id = pps_id,
    .PicOrderCntVal = picture->pic_order_cnt,
  };

  if (picture->nb_refs) {
    ref_list_info = (StdVideoEncodeH265ReferenceListsInfo) {
      .flags = (StdVideoEncodeH265ReferenceListsInfoFlags) {
        .ref_pic_list_modification_flag_l0 = 0,
        .ref_pic_list_modification_flag_l1 = 0,
      },
      .num_ref_idx_l0_active_minus1 = 0,
      .num_ref_idx_l1_active_minus1 = 0,
      .RefPicList0 = {0, },
      .RefPicList1 = {0, },
      .list_entry_l0 = {0, },
      .list_entry_l1 = {0, },
    };
    pic_info.pRefLists = &ref_list_info;
  }

  memset(ref_list_info.RefPicList0, STD_VIDEO_H265_NO_REFERENCE_PICTURE, STD_VIDEO_H265_MAX_NUM_LIST_REF);
  memset(ref_list_info.RefPicList1, STD_VIDEO_H265_NO_REFERENCE_PICTURE, STD_VIDEO_H265_MAX_NUM_LIST_REF);

  slice_info = (VkVideoEncodeH265NaluSliceSegmentInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_EXT,
    .pNext = NULL,
    .pStdSliceSegmentHeader = &slice_hdr,
  };

  rc_layer_info = (VkVideoEncodeH265RateControlLayerInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_EXT,
    .pNext = NULL,
    .useMinQp = TRUE,
    .minQp = (VkVideoEncodeH265QpEXT){ qp_i, qp_p, qp_b },
    .useMaxQp = TRUE,
    .maxQp = (VkVideoEncodeH265QpEXT){ qp_i, qp_p, qp_b },
    .useMaxFrameSize = 0,
    .maxFrameSize = (VkVideoEncodeH265FrameSizeEXT) {0, 0, 0},
  };

  rc_info = (VkVideoEncodeH265RateControlInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_EXT,
    .pNext = &rc_layer_info,
    .gopFrameCount = 0,
    .idrPeriod = 0,
    .consecutiveBFrameCount = 0,
  };

  quality_level = (VkVideoEncodeH265QualityLevelPropertiesEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_EXT,
    .pNext = NULL,
    .preferredRateControlFlags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_EXT,
    .preferredGopFrameCount = 0,
    .preferredIdrPeriod = 0,
    .preferredConsecutiveBFrameCount = 0,
    .preferredConstantQp = (VkVideoEncodeH265QpEXT){ qp_i, qp_p, qp_b },
    .preferredMaxL0ReferenceCount = 0,
    .preferredMaxL1ReferenceCount = 0,
  };

  enc_pic_info = (VkVideoEncodeH265PictureInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_EXT,
    .pNext = NULL,
    .naluSliceSegmentEntryCount = 1,
    .pNaluSliceSegmentEntries = &slice_info,
    .pStdPictureInfo = &pic_info,
  };

  ref_info = (StdVideoEncodeH265ReferenceInfo) {
    .flags = (StdVideoEncodeH265ReferenceInfoFlags) {
      .used_for_long_term_reference = 0,
      .unused_for_reference = 0,
    },
    .pic_type = gst_vulkan_video_h265_picture_type (slice_type, picture->is_ref),
    .PicOrderCntVal = picture->pic_order_cnt,
    .TemporalId = 0,
  };

  dpb_slot_info = (VkVideoEncodeH265DpbSlotInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_EXT,
    .pNext = NULL,
    .pStdReferenceInfo = &ref_info,

  };
  /* *INDENT-ON* */

  picture->codec_pic_info = &enc_pic_info;
  picture->codec_rc_layer_info = &rc_layer_info;
  picture->codec_rc_info = &rc_info;
  picture->codec_quality_level = &quality_level;
  picture->codec_dpb_slot_info = &dpb_slot_info;

  if (ref_pics_num)
    ref_list_info.RefPicList0[0] = ref_pics[0]->slotIndex;

  fail_unless (gst_vulkan_encoder_encode (enc, picture, ref_pics,
          ref_pics_num));

  {
    GstMapInfo info;
    gst_buffer_map (picture->out_buffer, &info, GST_MAP_READ);
    GST_MEMDUMP ("out buffer", info.data, info.size);
    gst_buffer_unmap (picture->out_buffer, &info);
  }
}

static GstVulkanEncoder *
setup_h265_encoder (uint32_t width, uint32_t height, gint vps_id, gint sps_id,
    gint pps_id)
{
  GstVulkanEncoder *enc;
  GError *err = NULL;

  /* initialize the encoder */
  uint32_t mbAlignedWidth, mbAlignedHeight;
  StdVideoH265ProfileIdc profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
  StdVideoH265LevelIdc level_idc = STD_VIDEO_H265_LEVEL_IDC_6_2;
  StdVideoH265ChromaFormatIdc chromat_format_idc =
      STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
  StdVideoH265VideoParameterSet vps;
  StdVideoH265SequenceParameterSet sps;
  StdVideoH265PictureParameterSet pps;
  GstVulkanVideoProfile profile;
  GstVulkanEncoderParameters enc_params;
  VkVideoEncodeH265SessionParametersAddInfoEXT params_add;
  StdVideoH265ProfileTierLevel profile_tier_level;
  StdVideoH265SequenceParameterSetVui vui_params;

  // Set arbitrary value for VPS
  memset (&vps, 0, sizeof (StdVideoH265VideoParameterSet));
  vps.flags.vps_temporal_id_nesting_flag = 1;   // FIXME: This parameter blocks the session init with STD_VIDEO_H265_PROFILE_IDC_MAIN not with STD_VIDEO_H265_PROFILE_IDC_HIGH
  vps.flags.vps_sub_layer_ordering_info_present_flag = 1;
  vps.vps_video_parameter_set_id = vps_id;

// Set arbitrary value for SPS
  memset (&sps, 0, sizeof (StdVideoH265SequenceParameterSet));
  sps.flags.sps_temporal_id_nesting_flag = 1;
  sps.flags.sps_sub_layer_ordering_info_present_flag = 1;
  sps.flags.sample_adaptive_offset_enabled_flag = 1;
  sps.flags.sps_temporal_mvp_enabled_flag = 1;
  sps.flags.strong_intra_smoothing_enabled_flag = 1;
  sps.flags.vui_parameters_present_flag = 1;


  sps.sps_seq_parameter_set_id = sps_id;
  mbAlignedWidth =
      (width + H265_MB_SIZE_ALIGNMENT - 1) & ~(H265_MB_SIZE_ALIGNMENT - 1);
  mbAlignedHeight =
      (height + H265_MB_SIZE_ALIGNMENT - 1) & ~(H265_MB_SIZE_ALIGNMENT - 1);

  sps.sps_video_parameter_set_id = vps_id;
  /* *INDENT-OFF* */
  profile_tier_level = (StdVideoH265ProfileTierLevel) {
    .flags = (StdVideoH265ProfileTierLevelFlags) {
      .general_tier_flag = 0,
      .general_progressive_source_flag = 1,
      .general_interlaced_source_flag = 0,
      .general_non_packed_constraint_flag = 0,
      .general_frame_only_constraint_flag = 1,
    },
    .general_profile_idc = profile_idc,
    .general_level_idc = level_idc,
  };

  vui_params = (StdVideoH265SequenceParameterSetVui) {
    .flags = (StdVideoH265SpsVuiFlags) {
      .aspect_ratio_info_present_flag = 0,
      .overscan_info_present_flag = 0,
      .overscan_appropriate_flag = 0,
      .video_signal_type_present_flag = 1,
      .video_full_range_flag = 0,
      .colour_description_present_flag = 0,
      .chroma_loc_info_present_flag = 0,
      .neutral_chroma_indication_flag = 0,
      .field_seq_flag = 0,
      .frame_field_info_present_flag = 0,
      .default_display_window_flag = 0,
      .vui_timing_info_present_flag = 1,
      .vui_poc_proportional_to_timing_flag = 0,
      .vui_hrd_parameters_present_flag = 0,
      .bitstream_restriction_flag = 0,
      .tiles_fixed_structure_flag = 0,
      .motion_vectors_over_pic_boundaries_flag = 0,
      .restricted_ref_pic_lists_flag = 0,
    },
    .aspect_ratio_idc = STD_VIDEO_H265_ASPECT_RATIO_IDC_UNSPECIFIED,
    .sar_width = 0,
    .sar_height = 0,
    .video_format = 1,
    .colour_primaries = 0,
    .transfer_characteristics = 0,
    .matrix_coeffs = 0,
    .chroma_sample_loc_type_top_field = 0,
    .chroma_sample_loc_type_bottom_field = 0,
    .def_disp_win_left_offset = 0,
    .def_disp_win_right_offset = 0,
    .def_disp_win_top_offset = 0,
    .def_disp_win_bottom_offset = 0,
    .vui_num_units_in_tick = 0,
    .vui_time_scale = 25,
    .vui_num_ticks_poc_diff_one_minus1 = 0,
    .min_spatial_segmentation_idc = 0,
    .max_bytes_per_pic_denom = 0,
    .max_bits_per_min_cu_denom = 0,
    .log2_max_mv_length_horizontal = 0,
    .log2_max_mv_length_vertical = 0,
    .pHrdParameters = NULL,
  };
  /* *INDENT-ON* */
  sps.pProfileTierLevel = &profile_tier_level;
  sps.chroma_format_idc = chromat_format_idc;
  sps.pic_width_in_luma_samples = mbAlignedWidth;
  sps.pic_height_in_luma_samples = mbAlignedHeight;
  sps.log2_max_pic_order_cnt_lsb_minus4 = 4;
  sps.pSequenceParameterSetVui = &vui_params;
  sps.log2_diff_max_min_luma_coding_block_size = 2;
  sps.log2_diff_max_min_luma_transform_block_size = 2;

  // Set arbitrary value for PPS
  memset (&pps, 0, sizeof (StdVideoH265PictureParameterSet));
  pps.flags.cu_qp_delta_enabled_flag = 1;       // FIXME: This parameter blocks the session init with STD_VIDEO_H265_PROFILE_IDC_MAIN not with STD_VIDEO_H265_PROFILE_IDC_HIGH
  pps.flags.deblocking_filter_control_present_flag = 1;
  pps.flags.pps_loop_filter_across_slices_enabled_flag = 1;
  pps.sps_video_parameter_set_id = vps_id;
  pps.pps_seq_parameter_set_id = sps_id;
  pps.pps_pic_parameter_set_id = pps_id;

  /* *INDENT-OFF* */
  params_add = (VkVideoEncodeH265SessionParametersAddInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_EXT,
    .pStdVPSs = &vps,
    .stdVPSCount = 1,
    .pStdSPSs = &sps,
    .stdSPSCount = 1,
    .pStdPPSs = &pps,
    .stdPPSCount = 1,
  };

  enc_params.create.h265 = (VkVideoEncodeH265SessionParametersCreateInfoEXT) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_EXT,
    .maxStdVPSCount = 1,
    .maxStdSPSCount = 1,
    .maxStdPPSCount = 1,
    .pParametersAddInfo = &params_add
  };

  profile = (GstVulkanVideoProfile) {
    .profile = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
      .pNext = &profile.codec,
      .videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT,
      .chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR,
      .chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
      .lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR,
    },
    .codec.h265enc = {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_EXT,
      .stdProfileIdc = profile_idc,
    }
  };
  /* *INDENT-ON* */

  video_queue =
      gst_vulkan_select_queue (instance, VK_QUEUE_VIDEO_ENCODE_BIT_KHR);


  if (!video_queue) {
    GST_WARNING ("Unable to find encoding queue");
    return NULL;
  }
  enc = gst_vulkan_queue_create_encoder (video_queue,
      VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT);

  if (!enc) {
    GST_WARNING ("Unable to create a vulkan encoder, queue=%p", video_queue);
    return NULL;
  }

  fail_unless (gst_vulkan_encoder_start (enc, &profile, &enc_params, &err));
  return enc;
}

static void
check_h265_session_params (GstVulkanEncoder * enc, gint vps_id, gint sps_id,
    gint pps_id)
{
  VkVideoEncodeH265SessionParametersGetInfoEXT video_codec_session_info;
  void *header;
  gsize header_size;

  video_codec_session_info = (VkVideoEncodeH265SessionParametersGetInfoEXT) {
    /* *INDENT-OFF* */
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_EXT,
    .pNext = NULL,
    .writeStdVPS = TRUE,
    .writeStdSPS = TRUE,
    .writeStdPPS = TRUE,
    .stdVPSId = sps_id,
    .stdSPSId = sps_id,
    .stdPPSId = pps_id,
    /* *INDENT-ON* */
  };

  fail_unless (gst_vulkan_encoder_get_session_params (enc,
          &video_codec_session_info, &header_size, NULL));
  fail_unless (header_size != 0);
  header = g_malloc0 (header_size);
  fail_unless (gst_vulkan_encoder_get_session_params (enc,
          &video_codec_session_info, &header_size, header));
  GST_MEMDUMP ("params buffer", header, header_size);
  //TODO check the SPS/PPS content
  g_free (header);
}

GST_START_TEST (test_encoder_h265_i)
{
  GstVulkanEncoder *enc;
  GstBufferPool *pool;

  uint32_t width = 176;
  uint32_t height = 144;
  gint vps_id = 0;
  gint sps_id = 0;
  gint pps_id = 0;
  GstVulkanEncodePicture *picture;
  int frame_num = 0;
  int i;

  enc = setup_h265_encoder (width, height, vps_id, sps_id, pps_id);
  if (!enc) {
    GST_WARNING ("Unable to initialize H265 encoder");
    return;
  }

  /* retrieve the SPS/PPS from the device */
  check_h265_session_params (enc, vps_id, sps_id, pps_id);

  pool = allocate_input_buffer_pool (enc, width, height);

  /* encode second picture 32 I-Frame */
  for (i = 0; i < 32; i++) {
    /* allocate picture */
    picture = allocate_picture (enc, pool, width, height, TRUE, 0);
    fail_unless (picture != NULL);

    /* encode frame */
    encode_h265_picture (enc, picture, frame_num, NULL, 0, GST_H265_I_SLICE,
        vps_id, sps_id, pps_id);

    fail_unless (picture->out_buffer != NULL);
    {
      GstMapInfo info;
      gst_buffer_map (picture->out_buffer, &info, GST_MAP_READ);
      GST_MEMDUMP ("out buffer", info.data, info.size);
      gst_buffer_unmap (picture->out_buffer, &info);
    }
    gst_vulkan_encode_picture_free (picture);
    frame_num++;
  }

  fail_unless (gst_buffer_pool_set_active (pool, FALSE));
  gst_object_unref (pool);

  fail_unless (gst_vulkan_encoder_stop (enc));

  gst_object_unref (enc);
}

GST_END_TEST;

GST_START_TEST (test_encoder_h265_i_p)
{
  GstVulkanEncoder *enc;
  GstBufferPool *pool;
  uint32_t width = 176;
  uint32_t height = 144;
  gint vps_id = 0;
  gint sps_id = 0;
  gint pps_id = 0;
  GstVulkanEncodePicture *picture;
  GstVulkanEncodePicture *ref_pics[16] = { NULL, };
  gint ref_pics_num = 1;
  int frame_num = 0;
  int i;

  enc = setup_h265_encoder (width, height, vps_id, sps_id, pps_id);
  if (!enc) {
    GST_WARNING ("Unable to initialize H265 encoder");
    return;
  }

  /* retrieve the SPS/PPS from the device */
  check_h265_session_params (enc, vps_id, sps_id, pps_id);

  pool = allocate_input_buffer_pool (enc, width, height);

  /* encode first picture as an IDR-Frame */
  {
    /* allocate picture */
    picture = allocate_picture (enc, pool, width, height, TRUE, 0);
    fail_unless (picture != NULL);

    /* encode frame */
    encode_h265_picture (enc, picture, frame_num, NULL, 0, GST_H265_I_SLICE,
        vps_id, sps_id, pps_id);
    fail_unless (picture->out_buffer != NULL);

  }
  ref_pics[0] = picture;
  /* encode second picture as a P-Frame */
  for (i = 0; i < 32; i++) {
    /* allocate picture */
    frame_num++;
    picture = allocate_picture (enc, pool, width, height, TRUE, ref_pics_num);
    picture->pic_num = frame_num;
    fail_unless (picture != NULL);

    /* encode frame */
    encode_h265_picture (enc, picture, frame_num, ref_pics, 1,
        GST_H265_P_SLICE, vps_id, sps_id, pps_id);
    fail_unless (picture->out_buffer != NULL);

    gst_vulkan_encode_picture_free (ref_pics[0]);
    ref_pics[0] = picture;
  }

  gst_vulkan_encode_picture_free (ref_pics[0]);

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
    tcase_add_test (tc_basic, test_encoder_h265_i);
    tcase_add_test (tc_basic, test_encoder_h265_i_p);
#endif
  }

  return s;
}

GST_CHECK_MAIN (vkvideo);
