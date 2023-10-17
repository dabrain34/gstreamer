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

#pragma once

#include <gst/vulkan/gstvkqueue.h>
#include <gst/vulkan/gstvkvideoutils.h>
#include <gst/vulkan/vulkan.h>

#define GST_TYPE_VULKAN_ENCODER         (gst_vulkan_encoder_get_type())
#define GST_VULKAN_ENCODER(o)           (G_TYPE_CHECK_INSTANCE_CAST((o), GST_TYPE_VULKAN_ENCODER, GstVulkanEncoder))
#define GST_VULKAN_ENCODER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GST_TYPE_VULKAN_ENCODER, GstVulkanEncoderClass))
#define GST_IS_VULKAN_ENCODER(o)        (G_TYPE_CHECK_INSTANCE_TYPE((o), GST_TYPE_VULKAN_ENCODER))
#define GST_IS_VULKAN_ENCODER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE((k), GST_TYPE_VULKAN_ENCODER))
#define GST_VULKAN_ENCODER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), GST_TYPE_VULKAN_ENCODER, GstVulkanEncoderClass))
GST_VULKAN_API
GType gst_vulkan_encoder_get_type       (void);


typedef enum _VulkanPackedHeaderType {
 VULKAN_PACKED_HEADER_TYPE_SPS = 0x01,
 VULKAN_PACKED_HEADER_TYPE_PPS = 0x02,
 VULKAN_PACKED_HEADER_TYPE_VPS = 0x04,
 VULKAN_PACKED_HEADER_TYPE_SLICE = 0x08,
 VULKAN_PACKED_HEADER_TYPE_RAW = 0x10,
 VULKAN_PACKED_HEADER_TYPE_UNKNOWN = 0x20,
} VulkanPackedHeaderType;

typedef struct _GstVulkanEncodePicture
{
  gboolean is_ref;
  gint nb_refs;
  gint slotIndex;
  /* picture parameters */
  GPtrArray *packed_headers;

  gint pic_num;
  gint pic_order_cnt;

  gint width;
  gint height;

  gint fps_n;
  gint fps_d;

  GstBuffer *in_buffer;
  GstBuffer *out_buffer;
  /* Input frame*/
  GstVulkanImageView        *img_view;
  GstVulkanImageView        *dpb_view;

  VkVideoPictureResourceInfoKHR *dpb_pic;

  void              *codec_rc_info;
  void              *codec_pic_info;
  void              *codec_rc_layer_info;
  void              *codec_dpb_slot_info;
  void              *codec_quality_level;

  void              *priv_data;
} GstVulkanEncodePicture;

/**
 * GstVulkanEncoder:
 * @parent: the parent #GstObject
 * @queue: the #GstVulkanQueue to command buffers will be allocated from
 *
 * Since: 1.24
 **/
struct _GstVulkanEncoder
{
  GstObject parent;

  GstVulkanQueue *queue;

  guint codec;
  GstVulkanVideoProfile profile;

  /* <private> */
  gpointer _reserved        [GST_PADDING];
};

/**
 * GstVulkanEncoderClass:
 * @parent_class: the parent #GstObjectClass
 *
 * Since: 1.24
 */
struct _GstVulkanEncoderClass
{
  GstObjectClass parent;
  /* <private> */
  gpointer _reserved        [GST_PADDING];
};

union _GstVulkanEncoderParameters
{
  union {
    VkVideoEncodeH264SessionParametersCreateInfoEXT   h264;
    VkVideoEncodeH265SessionParametersCreateInfoEXT   h265;
  } create;
};

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstVulkanEncoder, gst_object_unref)

GST_VULKAN_API
gboolean                gst_vulkan_encoder_start                (GstVulkanEncoder * self,
                                                                 GstVulkanVideoProfile * profile,
                                                                 GstVulkanEncoderParameters *enc_params,
                                                                 GError ** error);
GST_VULKAN_API
gboolean                gst_vulkan_encoder_stop                 (GstVulkanEncoder * self);
GST_VULKAN_API
gboolean                gst_vulkan_encoder_flush                (GstVulkanEncoder * self,
                                                                 GError ** error);
GST_VULKAN_API
gboolean                gst_vulkan_encoder_add_packed_header    (GstVulkanEncoder * self,
                                                                 GstVulkanEncodePicture * pic,
                                                                 gpointer data,
                                                                 gsize size);
GST_VULKAN_API
gboolean                gst_vulkan_encoder_get_session_params   (GstVulkanEncoder * self, void *codec_session_params,
                                                                 gsize *data_size, void *data);
GST_VULKAN_API
gint                    gst_vulkan_encoder_get_n_ref_slots       (GstVulkanEncoder * self);

GST_VULKAN_API
gboolean                gst_vulkan_encoder_encode               (GstVulkanEncoder * self,
                                                                 GstVulkanEncodePicture * pic,
                                                                 GstVulkanEncodePicture ** ref_pics,
                                                                 gint ref_pics_num);
GST_VULKAN_API
gboolean                gst_vulkan_encoder_vk_caps              (GstVulkanEncoder * self,
                                                                 GstVulkanVideoCapabilites * caps);
GST_VULKAN_API
GstCaps *               gst_vulkan_encoder_profile_caps         (GstVulkanEncoder * self);
GST_VULKAN_API
GstVulkanEncodePicture * gst_vulkan_encode_picture_new          (GstVulkanEncoder * self,
                                                                 GstBuffer * raw_buffer,
                                                                 gint width,
                                                                 gint height,
                                                                 gboolean is_ref,
                                                                 gint nb_refs
                                                                 );
GST_VULKAN_API
void                     gst_vulkan_encode_picture_free         (GstVulkanEncodePicture * pic);
