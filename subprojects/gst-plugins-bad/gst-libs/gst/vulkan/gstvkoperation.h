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

#include <gst/vulkan/gstvkqueue.h>

#define GST_TYPE_VULKAN_OPERATION         (gst_vulkan_operation_get_type())
#define GST_VULKAN_OPERATION(o)           (G_TYPE_CHECK_INSTANCE_CAST((o), GST_TYPE_VULKAN_OPERATION, GstVulkanOperation))
#define GST_VULKAN_OPERATION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GST_TYPE_VULKAN_OPERATION, GstVulkanOperationClass))
#define GST_IS_VULKAN_OPERATION(o)        (G_TYPE_CHECK_INSTANCE_TYPE((o), GST_TYPE_VULKAN_OPERATION))
#define GST_IS_VULKAN_OPERATION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE((k), GST_TYPE_VULKAN_OPERATION))
#define GST_VULKAN_OPERATION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), GST_TYPE_VULKAN_OPERATION, GstVulkanOperationClass))

GST_VULKAN_API
GType gst_vulkan_operation_get_type       (void);

/**
 * GstVulkanOperation:
 * @parent: the parent #GstObject
 * @cmd_buf: the current #GstVulkanCommandBuffer
 * @vk_cmd_pipeline_barrier2: if synchronization2 enabled it's the function
 *   pointer to vkCmdPipelineBarrier2
 *
 * Since: 1.24
 */
struct _GstVulkanOperation
{
  GstObject parent;

  GstVulkanCommandBuffer *cmd_buf;

#if defined(VK_KHR_synchronization2)
  PFN_vkCmdPipelineBarrier2KHR vk_cmd_pipeline_barrier2;
#else
  gpointer vk_cmd_pipeline_barrier2;
#endif

  /* <private> */
  gpointer _reserved        [GST_PADDING];
};

struct _GstVulkanOperationClass
{
  GstObjectClass parent_class;

  /* <private> */
  gpointer _reserved        [GST_PADDING];

};

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GstVulkanOperation, gst_object_unref)

GST_VULKAN_API
GstVulkanOperation *    gst_vulkan_operation_new                (GstVulkanCommandPool * cmd_pool);
GST_VULKAN_API
gboolean                gst_vulkan_operation_start              (GstVulkanOperation * self,
                                                                 GError ** error);
GST_VULKAN_API
gboolean                gst_vulkan_operation_wait               (GstVulkanOperation * self);
GST_VULKAN_API
gboolean                gst_vulkan_operation_submit             (GstVulkanOperation * self,
                                                                 GError ** error);
GST_VULKAN_API
void                    gst_vulkan_operation_reset              (GstVulkanOperation * self);
GST_VULKAN_API
GArray *                gst_vulkan_operation_create_barriers    (GstVulkanOperation * self);
GST_VULKAN_API
gboolean                gst_vulkan_operation_add_frame_barrier  (GstVulkanOperation * self,
                                                                 GstBuffer * frame,
                                                                 GArray * barriers,
                                                                 guint64 dst_stage,
                                                                 guint64 new_access,
                                                                 VkImageLayout new_layout,
                                                                 GstVulkanQueue * new_queue);
GST_VULKAN_API
void                    gst_vulkan_operation_update_frame       (GstVulkanOperation * self,
                                                                 GstBuffer * frame,
                                                                 guint64 dst_stage,
                                                                 guint64 new_access,
                                                                 VkImageLayout new_layout,
                                                                 GstVulkanQueue * new_queue);
GST_VULKAN_API
gboolean                gst_vulkan_operation_add_dependecy_frame(GstVulkanOperation * self,
                                                                 GstBuffer * frame,
                                                                 guint64 wait_stage,
                                                                 guint64 signal_stage);
GST_VULKAN_API
void                    gst_vulkan_operation_discard_dependencies
                                                                (GstVulkanOperation * self);
GST_VULKAN_API
gboolean                gst_vulkan_operation_enable_query       (GstVulkanOperation * self,
                                                                 VkQueryType query_type,
                                                                 gpointer pnext,
                                                                 GError ** error);
GST_VULKAN_API
gboolean                gst_vulkan_operation_get_query          (GstVulkanOperation * self,
                                                                 gint32 * ret,
                                                                 GError ** error);
GST_VULKAN_API
gboolean                gst_vulkan_operation_begin_query        (GstVulkanOperation * self);
GST_VULKAN_API
gboolean                gst_vulkan_operation_end_query          (GstVulkanOperation * self);

GST_VULKAN_API
gboolean                gst_vulkan_operation_use_sync2         (GstVulkanOperation * self);
