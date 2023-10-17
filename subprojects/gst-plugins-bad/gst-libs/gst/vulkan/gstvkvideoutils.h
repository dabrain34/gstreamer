/*
 * GStreamer
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#pragma once

#include <gst/gst.h>
#include <gst/vulkan/gstvkapi.h>


#ifndef GST_USE_UNSTABLE_API
# define GST_USE_UNSTABLE_API
#endif
#include <gst/codecparsers/gsth264parser.h>
#include <gst/codecparsers/gsth265parser.h>
#undef GST_USE_UNSTABLE_API

#include <gst/video/video.h>

G_BEGIN_DECLS

/**
 * GstVulkanVideoProfile:
 * @profile: the generic vulkan video profile
 * @codec: the specific codec profile
 *
 * Since: 1.24
 */
struct _GstVulkanVideoProfile
{
#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS
  VkVideoProfileInfoKHR profile;
  VkVideoDecodeUsageInfoKHR usage;
  union {
    VkBaseInStructure base;
    VkVideoDecodeH264ProfileInfoKHR h264dec;
    VkVideoDecodeH265ProfileInfoKHR h265dec;
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VkVideoEncodeH264ProfileInfoEXT h264enc;
#endif
  } codec;
#endif
  /* <private> */
  gpointer _reserved[GST_PADDING];
};

struct _GstVulkanVideoCapabilites
{
#if GST_VULKAN_HAVE_VIDEO_EXTENSIONS
  VkVideoCapabilitiesKHR caps;
  union
  {
    VkBaseInStructure base;
    VkVideoDecodeH264CapabilitiesKHR h264dec;
    VkVideoDecodeH265CapabilitiesKHR h265dec;
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VkVideoEncodeH264CapabilitiesEXT h264enc;
#endif
  } codec;
#endif
  /* <private> */
  gpointer _reserved[GST_PADDING];
};

typedef enum  {
  GST_VULKAN_VIDEO_OPERATION_DECODE = 0,
  GST_VULKAN_VIDEO_OPERATION_ENCODE,
  GST_VULKAN_VIDEO_OPERATION_UNKNOWN,
} GstVulkanVideoOperation;

GST_VULKAN_API
GstCaps *               gst_vulkan_video_profile_to_caps        (const GstVulkanVideoProfile * profile);
GST_VULKAN_API
gboolean                gst_vulkan_video_profile_from_caps      (GstVulkanVideoProfile * profile,
                                                                 GstCaps * caps,
                                                                 GstVulkanVideoOperation video_operation);
GST_VULKAN_API
gboolean                gst_vulkan_video_profile_is_valid       (GstVulkanVideoProfile * profile,
                                                                 guint codec);
GST_VULKAN_API
gboolean                gst_vulkan_video_profile_is_equal       (const GstVulkanVideoProfile * a,
                                                                 const GstVulkanVideoProfile * b);


GST_VULKAN_API
const gchar *                       gst_vulkan_video_get_profile_from_caps (GstCaps * caps);
GST_VULKAN_API
StdVideoH264ChromaFormatIdc         gst_vulkan_video_h264_chromat_from_format (GstVideoFormat format);
GST_VULKAN_API
gboolean                            gst_vulkan_video_get_chroma_info_from_format (GstVideoFormat format,
                                                                                  VkVideoChromaSubsamplingFlagBitsKHR * chroma_format,
                                                                                  VkVideoComponentBitDepthFlagsKHR * bit_depth_luma,
                                                                                  VkVideoComponentBitDepthFlagsKHR * bit_depth_chroma);

GST_VULKAN_API
StdVideoH264PictureType             gst_vulkan_video_h264_picture_type (GstH264SliceType type, gboolean key_type);

GST_VULKAN_API
StdVideoH264SliceType               gst_vulkan_video_h264_slice_type (GstH264SliceType type);

GST_VULKAN_API
StdVideoH264ProfileIdc              gst_vulkan_video_h264_profile_type (GstH264Profile profile);

GST_VULKAN_API
StdVideoH264LevelIdc                gst_vulkan_video_h264_level_idc (int level_idc);

GST_VULKAN_API
StdVideoH265ChromaFormatIdc         gst_vulkan_video_h265_chromat_from_format (GstVideoFormat format);

GST_VULKAN_API
StdVideoH265SliceType               gst_vulkan_video_h265_slice_type (GstH265SliceType type);

GST_VULKAN_API
StdVideoH265PictureType             gst_vulkan_video_h265_picture_type (GstH265SliceType type, gboolean key_type);

GST_VULKAN_API
StdVideoH265ProfileIdc              gst_vulkan_video_h265_profile_type (GstH265Profile profile);

GST_VULKAN_API
StdVideoH265LevelIdc                gst_vulkan_video_h265_level_idc (int level_idc);

G_END_DECLS
