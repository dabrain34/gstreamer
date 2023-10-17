/*
 * GStreamer
 * Copyright (C) 2022 Igalia, S.L.
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

#include "gstvkencoder.h"

#include "gstvkvideo-private.h"


enum
{
  PROP_0,
  PROP_RATE_CONTROL,
  PROP_VIDEO_USAGE,
  PROP_VIDEO_CONTENT,
  PROP_TUNING_MODE,
  PROP_AVERAGE_BITRATE,
  PROP_QUALITY_LEVEL,
  PROP_MAX
};

static GParamSpec *properties[PROP_MAX];

extern const VkExtensionProperties vk_codec_extensions[3];

extern const uint32_t _vk_codec_supported_extensions[4];


struct _GstVulkanEncoderPrivate
{
  GstVulkanHandle *session_params;

  GstCaps *profile_caps;

  GstVulkanOperation *exec;

  GstVulkanVideoSession session;
  GstVulkanVideoCapabilites caps;
  VkVideoFormatPropertiesKHR format;
  VkVideoEncodeCapabilitiesKHR enc_caps;
  VkVideoEncodeRateControlInfoKHR rate_control_info;

  GstVulkanVideoFunctions vk;

  VkVideoReferenceSlotInfoKHR ref_slots[16];
  gint ref_slots_num;
  gint current_slot_index;

  gboolean started;
  gboolean encoding_setup;
  struct
  {
    guint rate_control;
    guint average_bitrate;
    guint quality_level;
    guint video_usage;
    guint video_content;
    guint tuning_mode;
  } prop;
};

/**
 * SECTION:vkencoder
 * @title: GstVulkanEncoder
 * @short_description: Generic Vulkan Video Encoder
 */

#define GST_CAT_DEFAULT gst_vulkan_encoder_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define gst_vulkan_encoder_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanEncoder, gst_vulkan_encoder,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstVulkanEncoder)
    GST_DEBUG_CATEGORY_INIT (gst_vulkan_encoder_debug,
        "vulkanencoder", 0, "Vulkan device encoder"));

static gpointer
_populate_function_table (gpointer data)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (data);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GstVulkanInstance *instance;
  gboolean ret = FALSE;

  instance = gst_vulkan_device_get_instance (self->queue->device);
  if (!instance) {
    GST_ERROR_OBJECT (self, "Failed to get instance from the device");
    return GINT_TO_POINTER (FALSE);
  }

  ret = gst_vulkan_video_get_vk_functions (instance, &priv->vk, TRUE);
  gst_object_unref (instance);
  return GINT_TO_POINTER (ret);
}

static void
gst_vulkan_encoder_finalize (GObject * object)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);

  gst_clear_object (&self->queue);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vulkan_encoder_init (GstVulkanEncoder * self)
{
}

static void
gst_vulkan_encoder_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);

  switch (prop_id) {
    case PROP_RATE_CONTROL:
      priv->prop.rate_control = g_value_get_uint (value);
      break;
    case PROP_VIDEO_USAGE:
      priv->prop.video_usage = g_value_get_uint (value);
      break;
    case PROP_VIDEO_CONTENT:
      priv->prop.video_content = g_value_get_uint (value);
      break;
    case PROP_TUNING_MODE:
      priv->prop.tuning_mode = g_value_get_uint (value);
      break;
    case PROP_AVERAGE_BITRATE:
      priv->prop.average_bitrate = g_value_get_uint (value);
      break;
    case PROP_QUALITY_LEVEL:
      priv->prop.quality_level = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_encoder_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);

  switch (prop_id) {
    case PROP_RATE_CONTROL:
      g_value_set_uint (value, priv->prop.rate_control);
      break;
    case PROP_VIDEO_USAGE:
      g_value_set_uint (value, priv->prop.video_usage);
      break;
    case PROP_VIDEO_CONTENT:
      g_value_set_uint (value, priv->prop.video_content);
      break;
    case PROP_TUNING_MODE:
      g_value_set_uint (value, priv->prop.tuning_mode);
      break;
    case PROP_AVERAGE_BITRATE:
      g_value_set_uint (value, priv->prop.average_bitrate);
      break;
    case PROP_QUALITY_LEVEL:
      g_value_set_uint (value, priv->prop.quality_level);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vulkan_encoder_class_init (GstVulkanEncoderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gint n_props = PROP_MAX;
  GParamFlags param_flags =
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT;

  gobject_class->finalize = gst_vulkan_encoder_finalize;
  gobject_class->set_property = gst_vulkan_encoder_set_property;
  gobject_class->get_property = gst_vulkan_encoder_get_property;

  properties[PROP_RATE_CONTROL] =
      g_param_spec_uint ("rate-control", "Vulkan rate control",
      "Choose the vulkan rate control",
      VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR,
      VK_VIDEO_ENCODE_RATE_CONTROL_MODE_FLAG_BITS_MAX_ENUM_KHR,
      VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR, param_flags);
  properties[PROP_VIDEO_USAGE] =
      g_param_spec_uint ("vulkan-usage", "Vulkan encode usage",
      "Choose the vulkan encoding usage", VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR,
      VK_VIDEO_ENCODE_USAGE_FLAG_BITS_MAX_ENUM_KHR,
      VK_VIDEO_ENCODE_USAGE_DEFAULT_KHR, param_flags);
  properties[PROP_VIDEO_CONTENT] =
      g_param_spec_uint ("vulkan-content", "Vulkan encode content",
      "Choose the vulkan encoding content", VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR,
      VK_VIDEO_ENCODE_CONTENT_FLAG_BITS_MAX_ENUM_KHR,
      VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR, param_flags);
  properties[PROP_TUNING_MODE] =
      g_param_spec_uint ("tuning-mode", "Vulkan encode tuning",
      "Choose the vulkan encoding tuning",
      VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR,
      VK_VIDEO_ENCODE_TUNING_MODE_MAX_ENUM_KHR,
      VK_VIDEO_ENCODE_TUNING_MODE_DEFAULT_KHR, param_flags);
  properties[PROP_AVERAGE_BITRATE] =
      g_param_spec_uint ("average-bitrate", "Vulkan encode average bitrate",
      "Choose the vulkan encoding bitrate", 0, UINT_MAX, 0, param_flags);
  properties[PROP_QUALITY_LEVEL] =
      g_param_spec_uint ("quality-level", "Vulkan encode quality level",
      "Choose the vulkan encoding quality level", 0, UINT_MAX, 0, param_flags);

  g_object_class_install_properties (gobject_class, n_props, properties);
}

static VkFormat
gst_vulkan_video_encoder_get_format (GstVulkanEncoder * self,
    VkImageUsageFlagBits imageUsage, GError ** error)
{
  VkResult res;
  VkVideoFormatPropertiesKHR *fmts = NULL;
  guint i, n_fmts;
  VkPhysicalDevice gpu =
      gst_vulkan_device_get_physical_device (self->queue->device);;
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;

  VkVideoProfileListInfoKHR profile_list = {
    .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
    .profileCount = 1,
  };
  VkPhysicalDeviceVideoFormatInfoKHR fmt_info = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
    .pNext = &profile_list,
  };

  profile_list.pProfiles = &self->profile.profile;

  fmt_info.imageUsage = imageUsage;

  res = priv->vk.GetPhysicalDeviceVideoFormatProperties (gpu, &fmt_info,
      &n_fmts, NULL);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoFormatPropertiesKHR") != VK_SUCCESS)
    goto beach;

  if (n_fmts == 0) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "Profile doesn't have an output format");
    goto beach;
  }

  fmts = g_new0 (VkVideoFormatPropertiesKHR, n_fmts);
  for (i = 0; i < n_fmts; i++)
    fmts[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;

  res = priv->vk.GetPhysicalDeviceVideoFormatProperties (gpu, &fmt_info,
      &n_fmts, fmts);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoFormatPropertiesKHR") != VK_SUCCESS) {
    goto beach;
  }

  if (n_fmts == 0) {
    g_free (fmts);
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "Profile doesn't have an output format");
  }

  /* find the best output format */
  for (i = 0; i < n_fmts; i++) {
    format = gst_vulkan_format_to_video_format (fmts[i].format);
    if (format == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_WARNING_OBJECT (self, "Unknown Vulkan format %i", fmts[i].format);
      continue;
    } else {
      vk_format = fmts[i].format;
      priv->format = fmts[i];
      break;
    }
  }

  if (vk_format == VK_FORMAT_UNDEFINED) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "No valid output format found");
  }

beach:
  g_clear_pointer (&fmts, g_free);
  return vk_format;

}

gboolean
gst_vulkan_encoder_vk_caps (GstVulkanEncoder * self,
    GstVulkanVideoCapabilites * caps)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return FALSE;

  if (caps) {
    *caps = priv->caps;
    caps->caps.pNext = &caps->codec;
  }

  return TRUE;
}

GstCaps *
gst_vulkan_encoder_profile_caps (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), NULL);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return NULL;

  return priv->profile_caps;
}


static GstVulkanHandle *
gst_vulkan_encoder_create_video_session_parameters (GstVulkanEncoder * self,
    GstVulkanEncoderParameters * params, GError ** error)
{
  GstVulkanEncoderPrivate *priv;
  VkVideoSessionParametersCreateInfoKHR session_params_info;
  VkResult res;
  VkVideoSessionParametersKHR session_params;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), NULL);
  g_return_val_if_fail (params, NULL);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->session.session)
    return NULL;

  /* *INDENT-OFF* */
  session_params_info = (VkVideoSessionParametersCreateInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
    .pNext = params,
    .videoSession = priv->session.session->handle,
  };
  /* *INDENT-ON* */

  res = priv->vk.CreateVideoSessionParameters (self->queue->device->device,
      &session_params_info, NULL, &session_params);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkCreateVideoSessionParametersKHR") != VK_SUCCESS)
    return NULL;

  return gst_vulkan_handle_new_wrapped (self->queue->device,
      GST_VULKAN_HANDLE_TYPE_VIDEO_SESSION_PARAMETERS,
      (GstVulkanHandleTypedef) session_params,
      gst_vulkan_video_session_handle_free_parameters,
      priv->vk.DestroyVideoSessionParameters);
}

gboolean
gst_vulkan_encoder_start (GstVulkanEncoder * self,
    GstVulkanVideoProfile * profile, GstVulkanEncoderParameters * enc_params,
    GError ** error)
{
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  VkResult res;
  VkVideoSessionCreateInfoKHR session_create;
  VkPhysicalDevice gpu =
      gst_vulkan_device_get_physical_device (self->queue->device);

  VkFormat pic_format = VK_FORMAT_UNDEFINED;
  VkFormat dpb_format = VK_FORMAT_UNDEFINED;
  int codec_idx;
  GstVulkanCommandPool *cmd_pool;

  VkQueryPoolVideoEncodeFeedbackCreateInfoKHR query_create = {
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
    .encodeFeedbackFlags =
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
        VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR,
  };

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);

  if (priv->started)
    return TRUE;

  _populate_function_table (self);

  switch (self->codec) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT:
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT:
      if (!gst_vulkan_video_profile_is_valid (profile, self->codec)) {
        g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
            "Invalid profile");
        return FALSE;
      }
      break;
    default:
      g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
          "Invalid codec");
      return FALSE;
  }

  self->profile = *profile;
  self->profile.profile.pNext = &self->profile.codec;

  switch (self->profile.profile.videoCodecOperation) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_EXT:
      /* *INDENT-OFF* */
      priv->caps.codec.h264enc = (VkVideoEncodeH264CapabilitiesEXT) {
          .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_EXT,
      };
      codec_idx = GST_VK_VIDEO_EXTENSION_ENCODE_H264;
      /* *INDENT-ON* */
      break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_EXT:
      /* *INDENT-OFF* */
      priv->caps.codec.h265enc = (VkVideoEncodeH265CapabilitiesEXT) {
          .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_EXT,
      };
      codec_idx = GST_VK_VIDEO_EXTENSION_ENCODE_H265;
      /* *INDENT-ON* */
      break;
    default:
      g_assert_not_reached ();
  }

  /* *INDENT-OFF* */
  priv->enc_caps = (VkVideoEncodeCapabilitiesKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
    .pNext = &priv->caps.codec.h264enc,
  };
  priv->caps.caps =  (VkVideoCapabilitiesKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
    .pNext = &priv->enc_caps,
  };

  gpu = gst_vulkan_device_get_physical_device (self->queue->device);
  res = priv->vk.GetPhysicalDeviceVideoCapabilities (gpu,
      &self->profile.profile, &priv->caps.caps);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoCapabilitiesKHR") != VK_SUCCESS)
    return FALSE;

  if(_vk_codec_extensions[codec_idx].specVersion < _vk_codec_supported_extensions[codec_idx]) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "STD version headers [%i.%i.%i] not supported, need at least [%i.%i.%i], check your SDK path.", VK_CODEC_VERSION (_vk_codec_extensions[codec_idx].specVersion), VK_CODEC_VERSION (_vk_codec_supported_extensions[codec_idx]));
    return FALSE;
  }

  if (_vk_codec_extensions[codec_idx].specVersion < priv->caps.caps.stdHeaderVersion.specVersion) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "The driver needs a newer version [%i.%i.%i] of the current headers %d.%d.%d, please update the code to support this driver.",VK_CODEC_VERSION (priv->caps.caps.stdHeaderVersion.specVersion), VK_CODEC_VERSION (_vk_codec_extensions[codec_idx].specVersion));
    return FALSE;
  }


  priv->profile_caps = gst_vulkan_video_profile_to_caps (&self->profile);

  priv->caps.caps.pNext = NULL;

  /* Get output format */
  pic_format =
      gst_vulkan_video_encoder_get_format (self,
      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR, error);
  dpb_format =
      gst_vulkan_video_encoder_get_format (self,
      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, error);

  if (pic_format == VK_FORMAT_UNDEFINED) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "No valid picture format found");
    goto failed;
  }

  if (dpb_format == VK_FORMAT_UNDEFINED) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "No valid DPB format found");
    goto failed;
  }

  session_create = (VkVideoSessionCreateInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
    .queueFamilyIndex = self->queue->index,
    .pVideoProfile = &profile->profile,
    .pictureFormat = pic_format,
    .maxCodedExtent = priv->caps.caps.maxCodedExtent,
    .referencePictureFormat = dpb_format,
    .maxDpbSlots = priv->caps.caps.maxDpbSlots,
    .maxActiveReferencePictures = priv->caps.caps.maxActiveReferencePictures,
    .pStdHeaderVersion = &_vk_codec_extensions[codec_idx],
  };
  /* *INDENT-ON* */

  if (!gst_vulkan_video_session_create (&priv->session, self->queue->device,
          &priv->vk, &session_create, error))
    goto failed;

  priv->session_params =
      gst_vulkan_encoder_create_video_session_parameters (self, enc_params,
      error);

  if (!priv->session_params)
    goto failed;

  cmd_pool = gst_vulkan_queue_create_command_pool (self->queue, error);
  if (!cmd_pool)
    goto failed;
  priv->exec = gst_vulkan_operation_new (cmd_pool);
  gst_object_unref (cmd_pool);


  query_create.pNext = &profile->profile;
  if (!gst_vulkan_operation_enable_query (priv->exec,
          VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR, 1, &query_create, error))
    goto failed;

  if (!gst_vulkan_encoder_flush (self, error))
    goto failed;


  priv->started = TRUE;

  return TRUE;

failed:
  {
    gst_clear_caps (&priv->profile_caps);

    return FALSE;
  }
}

gboolean
gst_vulkan_encoder_stop (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return TRUE;

  gst_vulkan_video_session_destroy (&priv->session);

  gst_clear_caps (&priv->profile_caps);

  gst_clear_mini_object ((GstMiniObject **) & priv->session_params);

  gst_clear_object (&priv->exec);

  priv->started = FALSE;

  return TRUE;
}

gboolean
gst_vulkan_encoder_flush (GstVulkanEncoder * self, GError ** error)
{
  GstVulkanEncoderPrivate *priv;
  VkVideoBeginCodingInfoKHR encode_start;
  VkVideoCodingControlInfoKHR encode_ctrl = {
    .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
    .flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR,
  };
  VkVideoEndCodingInfoKHR encode_end = {
    .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
  };
  gboolean ret;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!(priv->session_params && priv->exec))
    return FALSE;

  /* *INDENT-OFF* */
  encode_start = (VkVideoBeginCodingInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
    .videoSession = priv->session.session->handle,
    .videoSessionParameters = priv->session_params->handle,
  };
  /* *INDENT-ON* */

  if (!gst_vulkan_operation_begin (priv->exec, error))
    return FALSE;

  priv->vk.CmdBeginVideoCoding (priv->exec->cmd_buf->cmd, &encode_start);
  priv->vk.CmdControlVideoCoding (priv->exec->cmd_buf->cmd, &encode_ctrl);
  priv->vk.CmdEndVideoCoding (priv->exec->cmd_buf->cmd, &encode_end);

  ret = gst_vulkan_operation_end (priv->exec, error);
  gst_vulkan_operation_wait (priv->exec);

  return ret;
}

/* Add packed header such as SPS, PPS, SEI, etc. If adding slice header,
   it is attached to the last slice parameter. */
gboolean
gst_vulkan_encoder_add_packed_header (GstVulkanEncoder * self,
    GstVulkanEncodePicture * pic, gpointer data, gsize size)
{
  g_ptr_array_add (pic->packed_headers, gst_buffer_new_wrapped (data, size));
  return TRUE;
}

gboolean
gst_vulkan_encoder_get_session_params (GstVulkanEncoder * self,
    void *codec_session_params, gsize * data_size, void *data)
{
  VkVideoEncodeSessionParametersGetInfoKHR video_params_info;
  VkVideoEncodeSessionParametersFeedbackInfoKHR feedback_info;
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);

  /* *INDENT-OFF* */
  video_params_info = (VkVideoEncodeSessionParametersGetInfoKHR) {
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
		.pNext = codec_session_params,
		priv->session_params->handle,
	};

  feedback_info = (VkVideoEncodeSessionParametersFeedbackInfoKHR)
	{
		.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
		.pNext = NULL,
		.hasOverrides = FALSE,
	};
  /* *INDENT-ON* */

  priv->vk.GetEncodedVideoSessionParameters (self->queue->device->device,
      &video_params_info, &feedback_info, data_size, data);

  return TRUE;
}

gint
gst_vulkan_encoder_get_n_ref_slots (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);

  return priv->ref_slots_num;
}

gboolean
gst_vulkan_encoder_encode (GstVulkanEncoder * self,
    GstVulkanEncodePicture * pic, GstVulkanEncodePicture ** ref_pics,
    gint ref_pics_num)
{
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GError *err = NULL;
  gboolean ret = TRUE;
  GstMemory *mem;
  int i;
  GstVulkanEncodeQueryResult *encode_res;
  gsize output_size;
  guint n_mems = 0;
  gsize params_size = 0;
  VkVideoEncodeQualityLevelInfoKHR quality_level_info;
  VkVideoCodingControlInfoKHR coding_ctrl;
  VkVideoBeginCodingInfoKHR begin_coding;
  VkVideoEncodeInfoKHR encode_info;
  gboolean reset_cctrl = FALSE;
  gint maxDpbSlots = priv->caps.caps.maxDpbSlots;

  /* *INDENT-OFF* */
  VkVideoEncodeRateControlLayerInfoKHR rate_control_layer = (VkVideoEncodeRateControlLayerInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
    .pNext = pic->codec_rc_layer_info,
    .averageBitrate = priv->prop.average_bitrate < priv->enc_caps.maxBitrate ? priv->prop.average_bitrate: priv->enc_caps.maxBitrate, //FIXME
    .maxBitrate = priv->enc_caps.maxBitrate,
    .frameRateNumerator = pic->fps_n,
    .frameRateDenominator = pic->fps_d,
  };
  /* *INDENT-ON* */

  pic->dpb_view = gst_vulkan_get_image_view (pic->in_buffer, NULL);

  /* Current picture */
  pic->dpb_pic = g_malloc0 (sizeof (VkVideoPictureResourceInfoKHR));
  pic->dpb_pic->sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
  pic->dpb_pic->pNext = NULL;
  pic->dpb_pic->codedOffset = (VkOffset2D) {
  0, 0};
  pic->dpb_pic->codedExtent = (VkExtent2D) {
  pic->width, pic->height};
  pic->dpb_pic->baseArrayLayer = 0;
  pic->dpb_pic->imageViewBinding = pic->dpb_view->view;

  /* *INDENT-OFF* */
  coding_ctrl = (VkVideoCodingControlInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
  };

  if (!priv->encoding_setup) {
    priv->ref_slots_num = 0;
    priv->current_slot_index = 0;
    reset_cctrl = TRUE;
  }

  if (!priv->encoding_setup) {
    priv->rate_control_info		= (VkVideoEncodeRateControlInfoKHR)
    {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
      .rateControlMode = priv->prop.rate_control,
      .layerCount = 0,
      .pLayers = NULL,
      .initialVirtualBufferSizeInMs = 0,
      .virtualBufferSizeInMs = 0,
    };

    switch (priv->prop.rate_control) {
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR:
        rate_control_layer.maxBitrate = rate_control_layer.averageBitrate;
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR:
        priv->rate_control_info.layerCount = 1;
        priv->rate_control_info.pLayers = &rate_control_layer;
        priv->rate_control_info.virtualBufferSizeInMs = 1;
    };
  }

  if (!gst_vulkan_operation_begin (priv->exec, &err))
    goto bail;

  if (pic->is_ref) {
    if (priv->current_slot_index >= maxDpbSlots)
      priv->current_slot_index = 0;
    priv->ref_slots[priv->current_slot_index] = (VkVideoReferenceSlotInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .pNext = pic->codec_dpb_slot_info,
      .slotIndex = -1,
      .pPictureResource = pic->dpb_pic,
    };
    pic->slotIndex = priv->current_slot_index;
    priv->current_slot_index++;
    if(priv->ref_slots_num < maxDpbSlots)
    priv->ref_slots_num ++;
  }

  begin_coding = (VkVideoBeginCodingInfoKHR){
    .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
    .pNext = NULL,
    .videoSession = priv->session.session->handle,
    .videoSessionParameters = priv->session_params->handle,
    .referenceSlotCount = priv->ref_slots_num,
    .pReferenceSlots = priv->ref_slots,
  };

  if (priv->encoding_setup) {
    switch (priv->prop.rate_control) {
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR:
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR:
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR:
        begin_coding.pNext = &priv->rate_control_info;
    };
  }
  /* *INDENT-ON* */

  priv->vk.CmdBeginVideoCoding (priv->exec->cmd_buf->cmd, &begin_coding);

  // 42.9. Video Coding Control
  // To apply dynamic controls to the currently bound video session object
  if (reset_cctrl) {
    coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    coding_ctrl.pNext = NULL;
    priv->vk.CmdControlVideoCoding (priv->exec->cmd_buf->cmd, &coding_ctrl);


    if (priv->prop.quality_level
        && priv->prop.quality_level <= priv->enc_caps.maxQualityLevels) {
      quality_level_info = (VkVideoEncodeQualityLevelInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,.pNext =
            NULL,.qualityLevel = priv->prop.quality_level,};
      coding_ctrl.pNext = &quality_level_info;
      coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
      GST_INFO ("quality_level_info.qualityLevel %d",
          quality_level_info.qualityLevel);
      priv->vk.CmdControlVideoCoding (priv->exec->cmd_buf->cmd, &coding_ctrl);
    }
    if (priv->prop.rate_control !=
        VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {
      coding_ctrl.pNext = &priv->rate_control_info;
      coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
      GST_INFO ("rate_control_info.rateControlMode %d",
          priv->rate_control_info.rateControlMode);
      priv->vk.CmdControlVideoCoding (priv->exec->cmd_buf->cmd, &coding_ctrl);
    }

    priv->encoding_setup = TRUE;
  }

  priv->ref_slots[pic->slotIndex].slotIndex = pic->slotIndex;

  output_size = GST_ROUND_UP_N (3 * 1024 * 1024,
      priv->caps.caps.minBitstreamBufferSizeAlignment);
  pic->out_buffer = gst_vulkan_video_codec_buffer_new (self->queue->device,
      &self->profile, VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, output_size);

  if (!pic->out_buffer)
    return GST_FLOW_ERROR;

  for (i = 0; i < pic->packed_headers->len; i++) {
    GstBuffer *buffer;
    GstMapInfo info;
    buffer = g_ptr_array_index (pic->packed_headers, i);
    gst_buffer_map (buffer, &info, GST_MAP_READ);
    GST_MEMDUMP ("params buffer", info.data, info.size);
    gst_buffer_unmap (buffer, &info);
    params_size += gst_buffer_get_size (buffer);
    mem = gst_buffer_peek_memory (buffer, 0);
    gst_buffer_insert_memory (pic->out_buffer, i, mem);
    n_mems++;
    g_ptr_array_free (pic->packed_headers, FALSE);
  }

  mem = gst_buffer_peek_memory (pic->out_buffer, n_mems);
  pic->img_view = gst_vulkan_get_image_view (pic->in_buffer, NULL);


  /* *INDENT-OFF* */
  encode_info = (VkVideoEncodeInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
    .pNext = pic->codec_pic_info,
    .flags = 0x0,
    .dstBuffer = ((GstVulkanBufferMemory *) mem)->buffer,
    .dstBufferOffset = 0,
    .dstBufferRange = ((GstVulkanBufferMemory *) mem)->barrier.size, //FIXME is it the correct value ?
    .srcPictureResource = (VkVideoPictureResourceInfoKHR) { // SPEC: this should be separate
        .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
        .pNext = NULL,
        .codedOffset = 0,
        .codedExtent = (VkExtent2D){ pic->width, pic->height },
        .baseArrayLayer = 0,
        .imageViewBinding = pic->img_view->view,
    },
    .pSetupReferenceSlot = pic->is_ref ? &priv->ref_slots[pic->slotIndex] : NULL,
    .referenceSlotCount = pic->nb_refs,
    .pReferenceSlots = pic->nb_refs ? &priv->ref_slots[ref_pics[0]->slotIndex]  : NULL,
    .precedingExternallyEncodedBytes = 0,
  };
  /* *INDENT-ON* */
  gst_vulkan_operation_begin_query (priv->exec, 0);
  priv->vk.CmdEncodeVideo (priv->exec->cmd_buf->cmd, &encode_info);
  gst_vulkan_operation_end_query (priv->exec, 0);


  /* *INDENT-OFF* */
  VkVideoEndCodingInfoKHR end_coding = {
    .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
    /* .pNext = */
    /* .flags =  */
  };
  /* *INDENT-ON* */

  /* 41.5 4. vkCmdEndVideoCodingKHR signals the end of the recording of the
   * Vulkan Video Context, as established by vkCmdBeginVideoCodingKHR. */
  priv->vk.CmdEndVideoCoding (priv->exec->cmd_buf->cmd, &end_coding);

  if (!gst_vulkan_operation_end (priv->exec, &err)) {
    GST_ERROR_OBJECT (self, "The operation did not complete properly");
    goto bail;
  }
  gst_vulkan_operation_wait (priv->exec);

  gst_vulkan_operation_get_query (priv->exec, (gpointer *) & encode_res, &err);
  if (encode_res->status == VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
    GST_INFO_OBJECT (self, "The frame %d has been encoded with size %lu",
        pic->pic_num, encode_res->data_size + params_size);
    gst_buffer_resize (pic->out_buffer, encode_res->offset,
        encode_res->data_size + params_size);
  } else {
    GST_ERROR_OBJECT (self,
        "The operation did not complete properly, query status = %d",
        encode_res->status);
    goto bail;
  }

  return ret;
bail:
  {
    return FALSE;
  }
}


/**
 * gst_vulkan_encode_picture_new:
 * @self: the #GstVulkanEncoder with the pool's configuration.
 * @in_buffer: the input buffer. Take a reference to the buffer
 * @width: the picture width
 * @height: the picture height
 * @is_ref: the picture ref flag
 * @nb_refs: the picture nb refs
 *
 * Create a new vulkan encode picture from the input buffer.
 *
 * Returns: a new #GstVulkanEncodePicture.
 *
 * Since: 1.24
 */
GstVulkanEncodePicture *
gst_vulkan_encode_picture_new (GstVulkanEncoder * self, GstBuffer * in_buffer,
    int width, int height, gboolean is_ref, gint nb_refs)
{
  GstVulkanEncodePicture *pic;

  g_return_val_if_fail (self && GST_IS_VULKAN_ENCODER (self), NULL);
  g_return_val_if_fail (in_buffer && GST_IS_BUFFER (in_buffer), NULL);

  pic = g_new0 (GstVulkanEncodePicture, 1);
  pic->in_buffer = gst_buffer_ref (in_buffer);
  pic->width = width;
  pic->height = height;

  pic->is_ref = is_ref;
  pic->nb_refs = nb_refs;
  pic->packed_headers =
      g_ptr_array_new_with_free_func ((GDestroyNotify) gst_buffer_unref);
  pic->slotIndex = -1;

  return pic;
}

/**
 * gst_vulkan_encode_picture_free:
 * @pic: the #GstVulkanEncodePicture to free.
 *
 * Free the picture.
 *
 * Since: 1.24
 */
void
gst_vulkan_encode_picture_free (GstVulkanEncodePicture * pic)
{
  g_return_if_fail (pic);

  gst_buffer_unref (pic->in_buffer);
  gst_buffer_unref (pic->out_buffer);

  if (pic->img_view) {
    gst_vulkan_image_view_unref (pic->img_view);
    pic->img_view = NULL;
  }
  if (pic->dpb_view) {
    gst_vulkan_image_view_unref (pic->dpb_view);
    pic->dpb_view = NULL;
  }

  g_ptr_array_free (pic->packed_headers, FALSE);
  g_free (pic->dpb_pic);
  g_free (pic);
}
